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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/refCountsInternals.h#1 $
 */

#ifndef REF_COUNTS_INTERNALS_H
#define REF_COUNTS_INTERNALS_H

#include "refCounts.h"

#include "journalPoint.h"
#include "referenceBlock.h"
#include "slab.h"
#include "blockAllocatorInternals.h"
#include "waitQueue.h"

/**
 * Represents the possible status of a block.
 **/
typedef enum referenceStatus {
  RS_FREE,        // this block is free
  RS_SINGLE,      // this block is singly-referenced
  RS_SHARED,      // this block is shared
  RS_PROVISIONAL  // this block is provisionally allocated
} ReferenceStatus;

/**
 * The SearchCursor represents the saved position of a free block search.
 **/
typedef struct searchCursor {
  /** The reference block containing the current search index */
  ReferenceBlock      *block;
  /** The position at which to start searching for the next free counter */
  SlabBlockNumber      index;
  /** The position just past the last valid counter in the current block */
  SlabBlockNumber      endIndex;

  /** A pointer to the first reference block in the slab */
  ReferenceBlock      *firstBlock;
  /** A pointer to the last reference block in the slab */
  ReferenceBlock      *lastBlock;
} SearchCursor;

/*
 * RefCounts structure
 *
 * A reference count is maintained for each PhysicalBlockNumber.  The vast
 * majority of blocks have a very small reference count (usually 0 or 1).
 * For references less than or equal to MAXIMUM_REFS (254) the reference count
 * is stored in counters[pbn].
 *
 */
struct refCounts {
  VDOCompletion             completion;

  /** The slab of this reference block */
  Slab                     *slab;

  /** The size of the counters array */
  uint32_t                  blockCount;
  /** The number of free blocks */
  uint32_t                  freeBlocks;
  /** The array of reference counts */
  ReferenceCount           *counters; // use ALLOCATE to align data ptr

  /** The saved block pointer and array indexes for the free block search */
  SearchCursor              searchCursor;

  /** A list of the dirty blocks waiting to be written out */
  WaitQueue                 dirtyBlocks;
  /** The number of blocks which are currently writing */
  size_t                    activeCount;

  /** A waiter object for updating the slab summary */
  Waiter                    slabSummaryWaiter;
  /** Whether slab summary update is in progress */
  bool                      updatingSlabSummary;
  /** Whether something is waiting for I/O to complete */
  bool                      hasIOWaiter;
  /** Whether a close has been requested */
  bool                      closeRequested;

  /** The context for tracking read-only mode */
  ReadOnlyModeContext      *readOnlyContext;
  /** The refcount statistics, shared by all refcounts in our physical zone */
  AtomicRefCountStatistics *statistics;
  /** The layer PBN for the first ReferenceBlock */
  PhysicalBlockNumber       origin;
  /** The latest slab journal entry this RefCounts has been updated with */
  JournalPoint              slabJournalPoint;

  /** The number of reference count blocks */
  uint32_t                  referenceBlockCount;
  /** reference count block array */
  ReferenceBlock            blocks[];
};

/**
 * Convert a reference count to a reference status.
 *
 * @param count The count to convert
 *
 * @return  The appropriate reference status
 **/
__attribute__((warn_unused_result))
ReferenceStatus referenceCountToStatus(ReferenceCount count);

/**
 * Convert a generic VDOCompletion to a RefCounts.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a RefCounts
 **/
RefCounts *asRefCounts(VDOCompletion *completion)
  __attribute__((warn_unused_result));

/**
 * Get the reference block that covers the given block index (exposed for
 * testing).
 *
 * @param refCounts  The refcounts object
 * @param index      The block index
 **/
ReferenceBlock *getReferenceBlock(RefCounts *refCounts, SlabBlockNumber index)
  __attribute__((warn_unused_result));

/**
 * Find the reference counters for a given block (exposed for testing).
 *
 * @param block  The ReferenceBlock in question
 *
 * @return A pointer to the reference counters for this block
 **/
ReferenceCount *getReferenceCountersForBlock(ReferenceBlock *block)
  __attribute__((warn_unused_result));

/**
 * Copy data from a reference block to a buffer ready to be written out
 * (exposed for testing).
 *
 * @param block   The block to copy
 * @param buffer  The char buffer to fill with the packed block
 **/
void packReferenceBlock(ReferenceBlock *block, void *buffer);

/**
 * Get the reference status of a block. Exposed only for unit testing.
 *
 * @param [in]  refCounts   The refcounts object
 * @param [in]  pbn         The physical block number
 * @param [out] statusPtr   Where to put the status of the block
 *
 * @return                  A success or error code, specifically:
 *                          VDO_OUT_OF_RANGE if the pbn is out of range.
 **/
int getReferenceStatus(RefCounts           *refCounts,
                       PhysicalBlockNumber  pbn,
                       ReferenceStatus     *statusPtr)
  __attribute__((warn_unused_result));

/**
 * Find the first block with a reference count of zero in the specified range
 * of reference counter indexes. Exposed for unit testing.
 *
 * @param [in]  refCounts   The reference counters to scan
 * @param [in]  startIndex  The array index at which to start scanning
 *                          (included in the scan)
 * @param [in]  endIndex    The array index at which to stop scanning
 *                          (excluded from the scan)
 * @param [out] indexPtr    A pointer to hold the array index of the free block
 *
 * @return true if a free block was found in the specified range
 **/
bool findFreeBlock(const RefCounts *refCounts,
                   SlabBlockNumber  startIndex,
                   SlabBlockNumber  endIndex,
                   SlabBlockNumber *indexPtr)
  __attribute__((warn_unused_result));

/**
 * Request a RefCounts save its oldest dirty block asynchronously.
 *
 * @param refCounts  The RefCounts object to notify
 **/
void saveOldestReferenceBlock(RefCounts *refCounts);

/**
 * Reset all reference counts back to RS_FREE.
 *
 * @param refCounts   The reference counters to reset
 **/
void resetReferenceCounts(RefCounts *refCounts);

#endif // REF_COUNTS_INTERNALS_H
