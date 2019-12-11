/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournal.h#12 $
 */

#ifndef SLAB_JOURNAL_H
#define SLAB_JOURNAL_H

#include "completion.h"
#include "journalPoint.h"
#include "ringNode.h"
#include "types.h"

/**
 * Convert a completion to a SlabJournal.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a SlabJournal
 **/
SlabJournal *asSlabJournal(VDOCompletion *completion)
  __attribute__((warn_unused_result));

/**
 * Calculate the number of slab journal entries per block.
 *
 * @return The number of slab journal entries per block
 **/
size_t getSlabJournalEntriesPerBlock(void)
  __attribute__((warn_unused_result));

/**
 * Obtain a pointer to a SlabJournal structure from a pointer to the
 * dirtyRingNode field within it.
 *
 * @param node  The RingNode to convert
 *
 * @return The RingNode as a SlabJournal
 **/
SlabJournal *slabJournalFromDirtyNode(RingNode *node)
  __attribute__((warn_unused_result));

/**
 * Create a slab journal.
 *
 * @param [in]  allocator        The block allocator which owns this journal
 * @param [in]  slab             The parent slab of the journal
 * @param [in]  recoveryJournal  The recovery journal of the VDO
 * @param [out] journalPtr       The pointer to hold the new slab journal
 *
 * @return VDO_SUCCESS or error code
 **/
int makeSlabJournal(struct block_allocator   *allocator,
                    struct vdo_slab          *slab,
                    struct recovery_journal  *recoveryJournal,
                    SlabJournal             **journalPtr)
  __attribute__((warn_unused_result));

/**
 * Free a slab journal and null out the reference to it.
 *
 * @param journalPtr  The reference to the slab journal to free
 **/
void freeSlabJournal(SlabJournal **journalPtr);

/**
 * Check whether a slab journal is blank, meaning it has never had any entries
 * recorded in it.
 *
 * @param journal  The journal to query
 *
 * @return <code>true</code> if the slab journal has never been modified
 **/
bool isSlabJournalBlank(const SlabJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Check whether the slab journal is on the block allocator's ring of dirty
 * journals.
 *
 * @param journal  The journal to query
 *
 * @return <code>true</code> if the journal has been added to the dirty ring
 **/
bool isSlabJournalDirty(const SlabJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Abort any VIOs waiting to make slab journal entries.
 *
 * @param journal  The journal to abort
 **/
void abortSlabJournalWaiters(SlabJournal *journal);

/**
 * Reopen a slab journal by emptying it and then adding any pending entries.
 *
 * @param journal  The journal to reopen
 **/
void reopenSlabJournal(SlabJournal *journal);

/**
 * Attempt to replay a recovery journal entry into a slab journal.
 *
 * @param journal        The slab journal to use
 * @param pbn            The PBN for the entry
 * @param operation      The type of entry to add
 * @param recoveryPoint  The recovery journal point corresponding to this entry
 * @param parent         The completion to notify when there is space to add
 *                       the entry if the entry could not be added immediately
 *
 * @return <code>true</code> if the entry was added immediately
 **/
bool attemptReplayIntoSlabJournal(SlabJournal          *journal,
                                  PhysicalBlockNumber   pbn,
                                  JournalOperation      operation,
                                  struct journal_point *recoveryPoint,
                                  VDOCompletion        *parent)
  __attribute__((warn_unused_result));

/**
 * Add an entry to a slab journal.
 *
 * @param journal  The slab journal to use
 * @param dataVIO  The data_vio for which to add the entry
 **/
void addSlabJournalEntry(SlabJournal *journal, struct data_vio *dataVIO);

/**
 * Adjust the reference count for a slab journal block. Note that when the
 * adjustment is negative, the slab journal will be reaped.
 *
 * @param journal         The slab journal
 * @param sequenceNumber  The journal sequence number of the referenced block
 * @param adjustment      Amount to adjust the reference counter
 **/
void adjustSlabJournalBlockReference(SlabJournal    *journal,
                                     SequenceNumber  sequenceNumber,
                                     int             adjustment);

/**
 * Request the slab journal to release the recovery journal lock it may hold on
 * a specified recovery journal block.
 *
 * @param journal       The slab journal
 * @param recoveryLock  The sequence number of the recovery journal block
 *                      whose locks should be released
 *
 * @return <code>true</code> if the journal does hold a lock on the specified
 *         block (which it will release)
 **/
bool releaseRecoveryJournalLock(SlabJournal    *journal,
                                SequenceNumber  recoveryLock)
  __attribute__((warn_unused_result));

/**
 * Commit the tail block of a slab journal.
 *
 * @param journal  The journal whose tail block should be committed
 **/
void commitSlabJournalTail(SlabJournal *journal);

/**
 * Drain slab journal I/O. Depending upon the type of drain (as recorded in
 * the journal's slab), any dirty journal blocks may be written out.
 *
 * @param journal  The journal to drain
 **/
void drainSlabJournal(SlabJournal *journal);

/**
 * Decode the slab journal by reading its tail.
 *
 * @param journal  The journal to decode
 **/
void decodeSlabJournal(SlabJournal *journal);

/**
 * Check to see if the journal should be scrubbed.
 *
 * @param journal  The slab journal
 *
 * @return <code>true</code> if the journal requires scrubbing
 **/
bool requiresScrubbing(const SlabJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Dump the slab journal.
 *
 * @param journal       The slab journal to dump
 **/
void dumpSlabJournal(const SlabJournal *journal);

#endif // SLAB_JOURNAL_H
