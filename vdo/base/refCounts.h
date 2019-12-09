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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCounts.h#9 $
 */

#ifndef REF_COUNTS_H
#define REF_COUNTS_H

#include "completion.h"
#include "journalPoint.h"
#include "slab.h"
#include "types.h"

/**
 * Create a reference counting object.
 *
 * <p>A reference counting object can keep a reference count for every physical
 * block in the VDO configuration. Since we expect the vast majority of the
 * blocks to have 0 or 1 reference counts, the structure is optimized for that
 * situation.
 *
 * @param [in]  blockCount        The number of physical blocks that can be
 *                                referenced
 * @param [in]  slab              The slab of the ref counts object
 * @param [in]  origin            The layer PBN at which to save RefCounts
 * @param [in]  readOnlyNotifier  The context for tracking read-only mode
 * @param [out] refCountsPtr      The pointer to hold the new ref counts object
 *
 * @return a success or error code
 **/
int makeRefCounts(BlockCount            blockCount,
                  Slab                 *slab,
                  PhysicalBlockNumber   origin,
                  ReadOnlyNotifier     *readOnlyNotifier,
                  RefCounts           **refCountsPtr)
  __attribute__((warn_unused_result));

/**
 * Free a reference counting object and null out the reference to it.
 *
 * @param refCountsPtr  The reference to the reference counting object to free
 **/
void freeRefCounts(RefCounts **refCountsPtr);

/**
 * Get the stored count of the number of blocks that are currently free.
 *
 * @param  refCounts  The RefCounts object
 *
 * @return the number of blocks with a reference count of zero
 **/
BlockCount getUnreferencedBlockCount(RefCounts *refCounts)
  __attribute__((warn_unused_result));

/**
 * Determine how many times a reference count can be incremented without
 * overflowing.
 *
 * @param  refCounts  The RefCounts object
 * @param  pbn        The physical block number
 *
 * @return the number of increments that can be performed
 **/
uint8_t getAvailableReferences(RefCounts *refCounts, PhysicalBlockNumber pbn)
  __attribute__((warn_unused_result));

/**
 * Adjust the reference count of a block.
 *
 * @param [in]  refCounts          The refcounts object
 * @param [in]  operation          The operation to perform
 * @param [in]  slabJournalPoint   The slab journal entry for this adjustment
 * @param [out] freeStatusChanged  A pointer which will be set to true if the
 *                                 free status of the block changed
 *
 *
 * @return A success or error code, specifically:
 *           VDO_REF_COUNT_INVALID   if a decrement would result in a negative
 *                                   reference count, or an increment in a
 *                                   count greater than MAXIMUM_REFS
 *
 **/
int adjustReferenceCount(RefCounts                  *refCounts,
                         struct reference_operation  operation,
                         const struct journal_point *slabJournalPoint,
                         bool                       *freeStatusChanged)
  __attribute__((warn_unused_result));

/**
 * Adjust the reference count of a block during rebuild.
 *
 * @param refCounts  The refcounts object
 * @param pbn        The number of the block to adjust
 * @param operation  The operation to perform on the count
 *
 * @return VDO_SUCCESS or an error
 **/
int adjustReferenceCountForRebuild(RefCounts           *refCounts,
                                   PhysicalBlockNumber  pbn,
                                   JournalOperation     operation)
  __attribute__((warn_unused_result));

/**
 * Replay the reference count adjustment from a slab journal entry into the
 * reference count for a block. The adjustment will be ignored if it was already
 * recorded in the reference count.
 *
 * @param refCounts   The refcounts object
 * @param entryPoint  The slab journal point for the entry
 * @param entry       The slab journal entry being replayed
 *
 * @return VDO_SUCCESS or an error code
 **/
int replayReferenceCountChange(RefCounts                  *refCounts,
                               const struct journal_point *entryPoint,
                               SlabJournalEntry            entry)
  __attribute__((warn_unused_result));

/**
 * Check whether two reference counters are equivalent. This method is
 * used for unit testing.
 *
 * @param counterA The first counter to compare
 * @param counterB The second counter to compare
 *
 * @return <code>true</code> if the two counters are equivalent
 **/
bool areEquivalentReferenceCounters(RefCounts *counterA, RefCounts *counterB)
  __attribute__((warn_unused_result));

/**
 * Find a block with a reference count of zero in the range of physical block
 * numbers tracked by the reference counter. If a free block is found, that
 * block is allocated by marking it as provisionally referenced, and the
 * allocated block number is returned.
 *
 * @param [in]  refCounts     The reference counters to scan
 * @param [out] allocatedPtr  A pointer to hold the physical block number of
 *                            the block that was found and allocated
 *
 * @return VDO_SUCCESS if a free block was found and allocated;
 *         VDO_NO_SPACE if there are no unreferenced blocks;
 *         otherwise an error code
 **/
int allocateUnreferencedBlock(RefCounts           *refCounts,
                              PhysicalBlockNumber *allocatedPtr)
  __attribute__((warn_unused_result));

/**
 * Provisionally reference a block if it is unreferenced.
 *
 * @param refCounts  The reference counters
 * @param pbn        The PBN to reference
 * @param lock       The pbn_lock on the block (may be NULL)
 *
 * @return VDO_SUCCESS or an error
 **/
int provisionallyReferenceBlock(RefCounts           *refCounts,
                                PhysicalBlockNumber  pbn,
                                struct pbn_lock     *lock)
  __attribute__((warn_unused_result));

/**
 * Count all unreferenced blocks in a range [startBlock, endBlock) of physical
 * block numbers.
 *
 * @param refCounts  The reference counters to scan
 * @param startPBN   The physical block number at which to start
 *                   scanning (included in the scan)
 * @param endPBN     The physical block number at which to stop
 *                   scanning (excluded from the scan)
 *
 * @return The number of unreferenced blocks
 **/
BlockCount countUnreferencedBlocks(RefCounts           *refCounts,
                                   PhysicalBlockNumber  startPBN,
                                   PhysicalBlockNumber  endPBN)
  __attribute__((warn_unused_result));

/**
 * Get the number of blocks required to save a reference counts state covering
 * the specified number of data blocks.
 *
 * @param blockCount  The number of physical data blocks that can be referenced
 *
 * @return The number of blocks required to save reference counts with the
 *         given block count
 **/
BlockCount getSavedReferenceCountSize(BlockCount blockCount)
  __attribute__((warn_unused_result));

/**
 * Request a RefCounts save several dirty blocks asynchronously. This function
 * currently writes 1 / flushDivisor of the dirty blocks.
 *
 * @param refCounts       The RefCounts object to notify
 * @param flushDivisor    The inverse fraction of the dirty blocks to write
 **/
void saveSeveralReferenceBlocks(RefCounts *refCounts, size_t flushDivisor);

/**
 * Ask a RefCounts to save all its dirty blocks asynchronously.
 *
 * @param refCounts     The RefCounts object to notify
 **/
void saveDirtyReferenceBlocks(RefCounts *refCounts);

/**
 * Mark all reference count blocks as dirty.
 *
 * @param refCounts  The RefCounts of the reference blocks
 **/
void dirtyAllReferenceBlocks(RefCounts *refCounts);

/**
 * Drain all reference count I/O. Depending upon the type of drain being
 * performed (as recorded in the RefCount's Slab), the reference blocks may
 * be loaded from disk or dirty reference blocks may be written out.
 *
 * @param refCounts  The reference counts to drain
 **/
void drainRefCounts(RefCounts *refCounts);

/**
 * Mark all reference count blocks dirty and cause them to hold locks on slab
 * journal block 1.
 *
 * @param refCounts  The RefCounts of the reference blocks
 **/
void acquireDirtyBlockLocks(RefCounts *refCounts);

/**
 * Dump information about this RefCounts structure.
 *
 * @param refCounts     The RefCounts to dump
 **/
void dumpRefCounts(const RefCounts *refCounts);

#endif // REF_COUNTS_H
