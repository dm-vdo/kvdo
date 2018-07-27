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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournalInternals.h#7 $
 */

#ifndef RECOVERY_JOURNAL_INTERNALS_H
#define RECOVERY_JOURNAL_INTERNALS_H

#include "numeric.h"

#include "fixedLayout.h"
#include "journalPoint.h"
#include "lockCounter.h"
#include "readOnlyModeContext.h"
#include "recoveryJournal.h"
#include "ringNode.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

typedef struct recoveryJournalBlock RecoveryJournalBlock;

struct recoveryJournal {
  /** The completion for shutdown */
  VDOCompletion              completion;
  /** The thread ID of the journal zone */
  ThreadID                   threadID;
  /** The slab depot which can hold locks on this journal */
  SlabDepot                 *depot;
  /** The block map which can hold locks on this journal */
  BlockMap                  *blockMap;
  /** The queue of VIOs waiting to make increment entries */
  WaitQueue                  incrementWaiters;
  /** The queue of VIOs waiting to make decrement entries */
  WaitQueue                  decrementWaiters;
  /** The number of free entries in the journal */
  uint64_t                   availableSpace;
  /** The number of decrement entries which need to be made */
  VIOCount                   pendingDecrementCount;
  /**
   * Whether the journal is adding entries from the increment or
   * decrement waiters queues
   **/
  bool                       addingEntries;
  /** The context for tracking read-only mode */
  ReadOnlyModeContext       *readOnlyContext;
  /** Whether a request has been made to close the journal */
  bool                       closeRequested;
  /** Whether a reap is in progress */
  bool                       reaping;
  /** The partition which holds the journal on disk */
  Partition                 *partition;
  /** The oldest active block in the journal on disk for block map rebuild */
  SequenceNumber             blockMapHead;
  /** The oldest active block in the journal on disk for slab journal replay */
  SequenceNumber             slabJournalHead;
  /** The newest block in the journal on disk to which a write has finished */
  SequenceNumber             lastWriteAcknowledged;
  /** The end of the half-open interval of the active journal */
  SequenceNumber             tail;
  /** The point at which the last entry will have been added */
  JournalPoint               appendPoint;
  /** The journal point of the VIO most recently released from the journal */
  JournalPoint               commitPoint;
  /** The nonce of the VDO */
  Nonce                      nonce;
  /** The number of recoveries completed by the VDO */
  uint8_t                    recoveryCount;
  /** The number of entries which fit in a single block */
  JournalEntryCount          entriesPerBlock;
  /** Unused in-memory journal blocks */
  RingNode                   freeTailBlocks;
  /** In-memory journal blocks with records */
  RingNode                   activeTailBlocks;
  /** A pointer to the active block (the one we are writing to now) */
  RecoveryJournalBlock      *activeBlock;
  /** The new block map reap head after reaping */
  SequenceNumber             blockMapReapHead;
  /** The head block number for the block map rebuild range */
  BlockCount                 blockMapHeadBlockNumber;
  /** The new slab journal reap head after reaping */
  SequenceNumber             slabJournalReapHead;
  /** The head block number for the slab journal replay range */
  BlockCount                 slabJournalHeadBlockNumber;
  /** The VIO on which we can call flush (less ick, but still ick) */
  VIO                       *flushVIO;
  /** The data block which must live in the VIO in the flush extent */
  char                      *unusedFlushVIOData;
  /** The number of blocks in the on-disk journal */
  BlockCount                 size;
  /** The number of logical blocks that are in-use */
  BlockCount                 logicalBlocksUsed;
  /** The number of block map pages that are allocated */
  BlockCount                 blockMapDataBlocks;
  /** The number of journal blocks written but not yet acknowledged */
  BlockCount                 pendingWriteCount;
  /** The threshold at which slab journal tail blocks will be written out */
  BlockCount                 slabJournalCommitThreshold;
  /** Counters for events in the journal that are reported as statistics */
  RecoveryJournalStatistics  events;
  /** The locks for each on-disk block */
  LockCounter               *lockCounter;
};

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return The block number corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline PhysicalBlockNumber
getRecoveryJournalBlockNumber(const RecoveryJournal *journal,
                              SequenceNumber         sequence)
{
  // Since journal size is a power of two, the block number modulus can just
  // be extracted from the low-order bits of the sequence.
  return (sequence & (journal->size - 1));
}

/**
 * Compute the checkByte for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number
 *
 * @return The check byte corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline uint8_t computeRecoveryCheckByte(const RecoveryJournal *journal,
                                               SequenceNumber         sequence)
{
  // The check byte must change with each trip around the journal.
  return (((sequence / journal->size) & 0x7F) | 0x80);
}

#endif // RECOVERY_JOURNAL_INTERNALS_H
