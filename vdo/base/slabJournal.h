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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabJournal.h#1 $
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
 * @param [in]  layer            The layer to which the journal will write
 * @param [in]  recoveryJournal  The recovery journal of the VDO
 * @param [out] journalPtr       The pointer to hold the new slab journal
 *
 * @return VDO_SUCCESS or error code
 **/
int makeSlabJournal(BlockAllocator  *allocator,
                    Slab            *slab,
                    PhysicalLayer   *layer,
                    RecoveryJournal *recoveryJournal,
                    SlabJournal    **journalPtr)
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
 * Check whether the slab journal can accept an entry of the specified type.
 *
 * @param journal    The journal to query
 * @param operation  The type of entry to make
 * @param completion The completion to notify when space is available if it
 *                   wasn't when this method was called
 *
 * @return <code>true</code> if the journal can make an entry of the
 *         specified type without blocking
 **/
bool mayAddSlabJournalEntry(SlabJournal      *journal,
                            JournalOperation  operation,
                            VDOCompletion    *completion);

/**
 * Add an entry to a slab journal during rebuild.
 *
 * @param journal        The slab journal to use
 * @param pbn            The PBN for the entry
 * @param operation      The type of entry to add
 * @param recoveryPoint  The recovery journal point corresponding to this entry
 **/
void addSlabJournalEntryForRebuild(SlabJournal         *journal,
                                   PhysicalBlockNumber  pbn,
                                   JournalOperation     operation,
                                   JournalPoint        *recoveryPoint);

/**
 * Add an entry to a slab journal.
 *
 * @param journal  The slab journal to use
 * @param dataVIO  The DataVIO for which to add the entry
 **/
void addSlabJournalEntry(SlabJournal *journal, DataVIO *dataVIO);

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
 * Close the slab journal.
 *
 * @param journal       The journal to close
 * @param parent        The completion which should be notified when the
 *                      journal is closed
 * @param callback      The callback to call when the journal is closed
 * @param errorHandler  The handler for close errors
 * @param threadID      The thread on which the callback should run
 **/
void closeSlabJournal(SlabJournal   *journal,
                      VDOCompletion *parent,
                      VDOAction     *callback,
                      VDOAction     *errorHandler,
                      ThreadID       threadID);

/**
 * Flush all uncommitted entries in the slab journal.
 *
 * <p>Implements slabJournal.c:SlabJournalPreparer.
 *
 * @param journal       The journal to flush
 * @param parent        The completion which should be notified when the
 *                      journal has been flushed
 * @param callback      The callback to use
 * @param errorHandler  The handler for flush errors
 * @param threadID      The thread on which the callback should run
 **/
void flushSlabJournal(SlabJournal   *journal,
                      VDOCompletion *parent,
                      VDOAction     *callback,
                      VDOAction     *errorHandler,
                      ThreadID       threadID);

/**
 * Decode the slab journal by reading its tail.
 *
 * <p>Implements slabJournal.c:SlabJournalPreparer.
 *
 * @param journal       The journal to decode
 * @param parent        The completion which should be notified when the
 *                      journal is decoded
 * @param callback      The callback to call once the tail is decoded
 * @param errorHandler  The handler for decode errors
 * @param threadID      The thread on which the callback should run
 **/
void decodeSlabJournal(SlabJournal   *journal,
                       VDOCompletion *parent,
                       VDOAction     *callback,
                       VDOAction     *errorHandler,
                       ThreadID       threadID);

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
