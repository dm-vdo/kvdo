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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournal.h#1 $
 */

#ifndef RECOVERY_JOURNAL_H
#define RECOVERY_JOURNAL_H

#include "buffer.h"
#include "completion.h"
#include "fixedLayout.h"
#include "flush.h"
#include "readOnlyModeContext.h"
#include "statistics.h"
#include "trace.h"
#include "types.h"

/**
 * The RecoveryJournal provides a log of all block mapping changes
 * which have not yet been stably written to the block map. It exists
 * to help provide resiliency guarantees by allowing synchronous
 * writes to be acknowledged as soon as the corresponding journal
 * entry is committed instead of having to wait for the block map
 * update. For asynchronous writes, the journal aids in meeting the
 * five second data loss window by ensuring that writes will not be
 * lost as long as they are committed to the journal before the window
 * expires. This should be less work than committing all of the
 * required block map pages.
 *
 * The journal consists of a set of on-disk blocks arranged as a
 * circular log with monotonically increasing sequence numbers. Three
 * sequence numbers serve to define the active extent of the
 * journal. The 'head' is the oldest active block in the journal. The
 * 'tail' is the end of the half-open interval containing the active
 * blocks. 'active' is the number of the block actively receiving
 * entries. In an empty journal, head == active == tail. Once any
 * entries are added, tail = active + 1, and head may be any value in
 * the interval [tail - size, active].
 *
 * The journal also contains a set of in-memory blocks which are used
 * to buffer up entries until they can be committed. In general the
 * number of in-memory blocks ('tailBufferCount') will be less than
 * the on-disk size. Each in-memory block is also a VDOCompletion.
 * Each in-memory block has a VDOExtent which is used to commit that
 * block to disk. The extent's data is a PackedJournalBlock (which is a
 * formatted journal block). In addition each in-memory block has a
 * buffer which is used to accumulate entries while a partial commit
 * of the block is in progress. In-memory blocks are kept on two
 * rings. Free blocks live on the 'freeTailBlocks' ring. When a block
 * becomes active (see below) it is moved to the 'activeTailBlocks'
 * ring. When a block is fully committed, it is moved back to the
 * 'freeTailBlocks' ring.
 *
 * When entries are added to the journal, they are added to the active
 * in-memory block, as indicated by the 'activeBlock' field. If the
 * caller wishes to wait for the entry to be committed, the requesting
 * VIO will be attached to the in-memory block to which the caller's
 * entry was added. If the caller does wish to wait, or if the entry
 * filled the active block, an attempt will be made to commit that
 * block to disk. If there is already another commit in progress, the
 * attempt will be ignored and then automatically retried when the
 * in-progress commit completes. If there is no commit in progress,
 * any VIOs waiting on the block are transferred to the extent. The
 * extent is then written, automatically waking all of the waiters
 * when it completes. When the extent completes, any entries which
 * accumulated in the block are copied to the extent's data buffer.
 *
 * Finally, the journal maintains a set of counters, one for each on
 * disk journal block. These counters are used as locks to prevent
 * premature reaping of journal blocks. Each time a new sequence
 * number is used, the counter for the corresponding block is
 * incremented. The counter is subsequently decremented when that
 * block is filled and then committed for the last time. This prevents
 * blocks from being reaped while they are still being updated. The
 * counter is also incremented once for each entry added to a block,
 * and decremented once each time the block map is updated in memory
 * for that request. This prevents blocks from being reaped while
 * their VIOs are still active. Finally, each in-memory block map page
 * tracks the oldest journal block that contains entries corresponding to
 * uncommitted updates to that block map page. Each time an in-memory block
 * map page is updated, it checks if the journal block for the VIO
 * is earlier than the one it references, in which case it increments
 * the count on the earlier journal block and decrements the count on the
 * later journal block, maintaining a lock on the oldest journal block
 * containing entries for that page. When a block map page has been flushed
 * from the cache, the counter for the journal block it references is
 * decremented. Whenever the counter for the head block goes to 0, the
 * head is advanced until it comes to a block whose counter is not 0
 * or until it reaches the active block. This is the mechanism for
 * reclaiming journal space on disk.
 *
 * If there is no in-memory space when a VIO attempts to add an entry,
 * the VIO will be attached to the 'commitCompletion' and will be
 * woken the next time a full block has committed. If there is no
 * on-disk space when a VIO attempts to add an entry, the VIO will be
 * attached to the 'reapCompletion', and will be woken the next time a
 * journal block is reaped.
 **/

/**
 * Return whether a given JournalOperation is an increment type.
 *
 * @param operation  The operation in question
 *
 * @return true if the type is an increment type
 **/
static inline bool isIncrementOperation(JournalOperation operation)
{
  return ((operation == DATA_INCREMENT) || (operation == BLOCK_MAP_INCREMENT));
}

/**
 * Get the name of a journal operation.
 *
 * @param operation  The operation to name
 *
 * @return The name of the operation
 **/
const char *getJournalOperationName(JournalOperation operation)
  __attribute__((warn_unused_result));

/**
 * Inform a recovery journal that the VDO has entered read-only mode.
 *
 * @param journal  The journal to notify
 **/
void notifyRecoveryJournalOfReadOnlyMode(RecoveryJournal *journal);

/**
 * Create a recovery journal.
 *
 * @param [in]  nonce            the nonce of the VDO
 * @param [in]  layer            the physical layer for the journal
 * @param [in]  partition        the partition for the journal
 * @param [in]  recoveryCount    The VDO's number of completed recoveries
 * @param [in]  journalSize      the number of blocks in the journal on disk
 * @param [in]  tailBufferSize   the number of blocks for tail buffer
 * @param [in]  readOnlyContext  the read-only mode context
 * @param [in]  threadConfig     the thread configuration of the VDO
 * @param [out] journalPtr       the pointer to hold the new recovery journal
 *
 * @return a success or error code
 **/
int makeRecoveryJournal(Nonce                 nonce,
                        PhysicalLayer        *layer,
                        Partition            *partition,
                        uint64_t              recoveryCount,
                        BlockCount            journalSize,
                        BlockCount            tailBufferSize,
                        ReadOnlyModeContext  *readOnlyContext,
                        const ThreadConfig   *threadConfig,
                        RecoveryJournal     **journalPtr)
  __attribute__((warn_unused_result));

/**
 * Free a recovery journal and null out the reference to it.
 *
 * @param [in,out] journalPtr  The reference to the recovery journal to free
 **/
void freeRecoveryJournal(RecoveryJournal **journalPtr);

/**
 * Move the backing partition pointer of the recovery journal.
 * Assumes that the data in the old and the new partitions is identical.
 *
 * @param journal   the journal being moved
 * @param partition the new journal partition
 **/
void setRecoveryJournalPartition(RecoveryJournal *journal,
                                 Partition       *partition);

/**
 * Initialize the journal after a recovery.
 *
 * @param journal        The journal in question
 * @param recoveryCount  The number of completed recoveries
 * @param tail           The new tail block sequence number
 **/
void initializeRecoveryJournalPostRecovery(RecoveryJournal *journal,
                                           uint64_t         recoveryCount,
                                           SequenceNumber   tail);

/**
 * Initialize the journal after a rebuild.
 *
 * @param journal            The journal in question
 * @param recoveryCount      The number of completed recoveries
 * @param tail               The new tail block sequence number
 * @param logicalBlocksUsed  The new number of logical blocks used
 * @param blockMapDataBlocks The new number of block map data blocks
 **/
void initializeRecoveryJournalPostRebuild(RecoveryJournal *journal,
                                          uint64_t         recoveryCount,
                                          SequenceNumber   tail,
                                          BlockCount       logicalBlocksUsed,
                                          BlockCount       blockMapDataBlocks);

/**
 * Get the number of block map pages, allocated from data blocks, currently
 * in use.
 *
 * @param journal   The journal in question
 *
 * @return  The number of block map pages allocated from slabs
 **/
BlockCount getJournalBlockMapDataBlocksUsed(RecoveryJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Set the number of block map pages, allocated from data blocks, currently
 * in use.
 *
 * @param journal   The journal in question
 * @param pages     The number of block map pages allocated from slabs
 **/
void setJournalBlockMapDataBlocksUsed(RecoveryJournal *journal,
                                      BlockCount       pages);

/**
 * Prepare the journal for new entries.
 *
 * @param journal   The journal in question
 * @param depot     The slab depot for this VDO
 * @param blockMap  The block map for this VDO
 **/
void openRecoveryJournal(RecoveryJournal *journal,
                         SlabDepot       *depot,
                         BlockMap        *blockMap);

/**
 * Obtain the recovery journal's current sequence number. Exposed only so
 * the block map can be initialized therefrom.
 *
 * @param journal  The journal in question
 *
 * @return the sequence number of the tail block
 **/
SequenceNumber getCurrentJournalSequenceNumber(RecoveryJournal *journal);

/**
 * Get the number of usable recovery journal blocks.
 *
 * @param journalSize  The size of the recovery journal in blocks
 *
 * @return the number of recovery journal blocks usable for entries
 **/
BlockCount getRecoveryJournalLength(BlockCount journalSize)
  __attribute__((warn_unused_result));

/**
 * Get the size of the encoded state of a recovery journal.
 *
 * @return the encoded size of the journal's state
 **/
size_t getRecoveryJournalEncodedSize(void)
  __attribute__((warn_unused_result));

/**
 * Encode the state of a recovery journal.
 *
 * @param journal  the recovery journal
 * @param buffer   the buffer to encode into
 *
 * @return VDO_SUCCESS or an error code
 **/
int encodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode the state of a recovery journal saved in a buffer.
 *
 * @param journal  the recovery journal
 * @param buffer   the buffer containing the saved state
 *
 * @return VDO_SUCCESS or an error code
 **/
int decodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode the state of a Sodium recovery journal saved in a buffer.
 *
 * @param journal  the recovery journal
 * @param buffer   the buffer containing the saved state
 *
 * @return VDO_SUCCESS or an error code
 **/
int decodeSodiumRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Add an entry to a recovery journal. This method is asynchronous. The DataVIO
 * will not be called back until the entry is committed to the on-disk journal.
 *
 * @param journal  The journal in which to make an entry
 * @param dataVIO  The DataVIO for which to add the entry. The entry will be
 *                 taken from the logical and newMapped fields of the
 *                 DataVIO. The DataVIO's recoverySequenceNumber field will
 *                 be set to the sequence number of the journal block in
 *                 which the entry was made.
 **/
void addRecoveryJournalEntry(RecoveryJournal *journal, DataVIO *dataVIO);

/**
 * Acquire a reference to a recovery journal block from somewhere other than
 * the journal itself.
 *
 * @param journal         The recovery journal
 * @param sequenceNumber  The journal sequence number of the referenced block
 * @param zoneType        The type of the zone making the adjustment
 * @param zoneID          The ID of the zone making the adjustment
 **/
void acquireRecoveryJournalBlockReference(RecoveryJournal *journal,
                                          SequenceNumber   sequenceNumber,
                                          ZoneType         zoneType,
                                          ZoneCount        zoneID);


/**
 * Release a reference to a recovery journal block from somewhere other than
 * the journal itself. If this is the last reference for a given zone type,
 * an attempt will be made to reap the journal.
 *
 * @param journal         The recovery journal
 * @param sequenceNumber  The journal sequence number of the referenced block
 * @param zoneType        The type of the zone making the adjustment
 * @param zoneID          The ID of the zone making the adjustment
 **/
void releaseRecoveryJournalBlockReference(RecoveryJournal *journal,
                                          SequenceNumber   sequenceNumber,
                                          ZoneType         zoneType,
                                          ZoneCount        zoneID);

/**
 * Release a single per-entry reference count for a recovery journal block. This
 * method may be called from any zone (but shouldn't be called from the journal
 * zone as it would be inefficient).
 *
 * @param journal         The recovery journal
 * @param sequenceNumber  The journal sequence number of the referenced block
 **/
void releasePerEntryLockFromOtherZone(RecoveryJournal *journal,
                                      SequenceNumber   sequenceNumber);

/**
 * Close the journal in preparation for shutdown. All uncommitted entries will
 * be committed.
 *
 * @param journal  The journal to close
 * @param parent   The completion to finish once the journal is closed
 **/
void closeRecoveryJournal(RecoveryJournal *journal, VDOCompletion *parent);

/**
 * Get the number of logical blocks in use by the VDO
 *
 * @param journal   the journal
 *
 * @return the number of logical blocks in use by the VDO
 **/
BlockCount getJournalLogicalBlocksUsed(const RecoveryJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Get the current statistics from the recovery journal.
 *
 * @param journal   The recovery journal to query
 *
 * @return a copy of the current statistics for the journal
 **/
RecoveryJournalStatistics
getRecoveryJournalStatistics(const RecoveryJournal *journal)
  __attribute__((warn_unused_result));

/**
 * Dump some current statistics and other debug info from the recovery
 * journal.
 *
 * @param journal   The recovery journal to dump
 **/
void dumpRecoveryJournalStatistics(const RecoveryJournal *journal);

#endif // RECOVERY_JOURNAL_H
