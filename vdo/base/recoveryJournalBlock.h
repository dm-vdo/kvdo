/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournalBlock.h#5 $
 */

#ifndef RECOVERY_JOURNAL_BLOCK_H
#define RECOVERY_JOURNAL_BLOCK_H

#include "permassert.h"

#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalInternals.h"
#include "ringNode.h"
#include "types.h"
#include "waitQueue.h"

struct recoveryJournalBlock {
  /** The doubly linked pointers for the free or active lists */
  RingNode             ringNode;
  /** The journal to which this block belongs */
  RecoveryJournal     *journal;
  /** A pointer to a block-sized buffer holding the packed block data */
  char                *block;
  /** A pointer to the current sector in the packed block buffer */
  PackedJournalSector *sector;
  /** The VIO for writing this block */
  VIO                 *vio;
  /** The sequence number for this block */
  SequenceNumber       sequenceNumber;
  /** The location of this block in the on-disk journal */
  PhysicalBlockNumber  blockNumber;
  /** Whether this block is being committed */
  bool                 committing;
  /** Whether this block has an uncommitted increment for a partial write */
  bool                 hasPartialWriteEntry;
  /** Whether this block has an uncommitted increment for a write with FUA */
  bool                 hasFUAEntry;
  /** The total number of entries in this block */
  JournalEntryCount    entryCount;
  /** The total number of uncommitted entries (queued or committing) */
  JournalEntryCount    uncommittedEntryCount;
  /** The number of new entries in the current commit */
  JournalEntryCount    entriesInCommit;
  /** The queue of VIOs which will make entries for the next commit */
  WaitQueue            entryWaiters;
  /** The queue of VIOs waiting for the current commit */
  WaitQueue            commitWaiters;
};

/**
 * Return the block associated with a ring node.
 *
 * @param node The ring node to recast as a block
 *
 * @return The block
 **/
static inline RecoveryJournalBlock *blockFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(RecoveryJournalBlock, ringNode) == 0);
  return (RecoveryJournalBlock *) node;
}

/**
 * Check whether a recovery block is dirty, indicating it has any uncommitted
 * entries, which includes both entries not written and entries written but
 * not yet acknowledged.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the block has any uncommitted entries
 **/
__attribute__((warn_unused_result))
static inline bool isRecoveryBlockDirty(const RecoveryJournalBlock *block)
{
  return (block->uncommittedEntryCount > 0);
}

/**
 * Check whether a journal block is empty.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the block has no entries
 **/
__attribute__((warn_unused_result))
static inline bool isRecoveryBlockEmpty(const RecoveryJournalBlock *block)
{
  return (block->entryCount == 0);
}

/**
 * Check whether a journal block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the the block is full
 **/
__attribute__((warn_unused_result))
static inline bool isRecoveryBlockFull(const RecoveryJournalBlock *block)
{
  return (block->journal->entriesPerBlock == block->entryCount);
}

/**
 * Construct a journal block.
 *
 * @param [in]  journal   The journal to which the block will belong
 * @param [out] blockPtr  A pointer to receive the new block
 *
 * @return VDO_SUCCESS or an error
 **/
int makeRecoveryBlock(RecoveryJournal       *journal,
                      RecoveryJournalBlock **blockPtr)
  __attribute__((warn_unused_result));

/**
 * Free a tail block and null out the reference to it.
 *
 * @param blockPtr  The reference to the tail block to free
 **/
void freeRecoveryBlock(RecoveryJournalBlock **blockPtr);

/**
 * Initialize the next active recovery journal block.
 *
 * @param block  The journal block to initialize
 **/
void initializeRecoveryBlock(RecoveryJournalBlock *block);

/**
 * Enqueue a DataVIO to asynchronously encode and commit its next recovery
 * journal entry in this block. The DataVIO will not be continued until the
 * entry is committed to the on-disk journal. The caller is responsible for
 * ensuring the block is not already full.
 *
 * @param block    The journal block in which to make an entry
 * @param dataVIO  The DataVIO to enqueue
 *
 * @return VDO_SUCCESS or an error code if the DataVIO could not be enqueued
 **/
int enqueueRecoveryBlockEntry(RecoveryJournalBlock *block, DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Attempt to commit a block. If the block is not the oldest block with
 * uncommitted entries or if it is already being committed, nothing will be
 * done.
 *
 * @param block         The block to write
 * @param callback      The function to call when the write completes
 * @param errorHandler  The handler for flush or write errors
 *
 * @return VDO_SUCCESS, or an error if the write could not be launched
 **/
int commitRecoveryBlock(RecoveryJournalBlock *block,
                        VDOAction            *callback,
                        VDOAction            *errorHandler)
  __attribute__((warn_unused_result));

/**
 * Dump the contents of the recovery block to the log.
 *
 * @param block  The block to dump
 **/
void dumpRecoveryBlock(const RecoveryJournalBlock *block);

#endif // RECOVERY_JOURNAL_BLOCK_H
