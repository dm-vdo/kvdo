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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCountsInternals.h#12 $
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
 * The search_cursor represents the saved position of a free block search.
 **/
struct search_cursor {
  /** The reference block containing the current search index */
  struct reference_block *block;
  /** The position at which to start searching for the next free counter */
  SlabBlockNumber         index;
  /** The position just past the last valid counter in the current block */
  SlabBlockNumber         endIndex;

  /** A pointer to the first reference block in the slab */
  struct reference_block *firstBlock;
  /** A pointer to the last reference block in the slab */
  struct reference_block *lastBlock;
};

/*
 * ref_counts structure
 *
 * A reference count is maintained for each PhysicalBlockNumber.  The vast
 * majority of blocks have a very small reference count (usually 0 or 1).
 * For references less than or equal to MAXIMUM_REFS (254) the reference count
 * is stored in counters[pbn].
 *
 */
struct ref_counts {
  /** The slab of this reference block */
  struct vdo_slab                    *slab;

  /** The size of the counters array */
  uint32_t                            blockCount;
  /** The number of free blocks */
  uint32_t                            freeBlocks;
  /** The array of reference counts */
  ReferenceCount                     *counters; // use ALLOCATE to align data ptr

  /** The saved block pointer and array indexes for the free block search */
  struct search_cursor                searchCursor;

  /** A list of the dirty blocks waiting to be written out */
  struct wait_queue                   dirtyBlocks;
  /** The number of blocks which are currently writing */
  size_t                              activeCount;

  /** A waiter object for updating the slab summary */
  struct waiter                       slabSummaryWaiter;
  /** Whether slab summary update is in progress */
  bool                                updatingSlabSummary;

  /** The notifier for read-only mode */
  struct read_only_notifier          *readOnlyNotifier;
  /** The refcount statistics, shared by all refcounts in our physical zone */
  struct atomic_ref_count_statistics *statistics;
  /** The layer PBN for the first struct reference_block */
  PhysicalBlockNumber                 origin;
  /** The latest slab journal entry this ref_counts has been updated with */
  struct journal_point                slabJournalPoint;

  /** The number of reference count blocks */
  uint32_t                            referenceBlockCount;
  /** reference count block array */
  struct reference_block              blocks[];
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
 * Convert a generic VDOCompletion to a ref_counts object.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ref_counts object
 **/
struct ref_counts *asRefCounts(VDOCompletion *completion)
  __attribute__((warn_unused_result));

/**
 * Get the reference block that covers the given block index (exposed for
 * testing).
 *
 * @param refCounts  The refcounts object
 * @param index      The block index
 **/
struct reference_block *getReferenceBlock(struct ref_counts *refCounts,
                                          SlabBlockNumber    index)
  __attribute__((warn_unused_result));

/**
 * Find the reference counters for a given block (exposed for testing).
 *
 * @param block  The reference_block in question
 *
 * @return A pointer to the reference counters for this block
 **/
ReferenceCount *getReferenceCountersForBlock(struct reference_block *block)
  __attribute__((warn_unused_result));

/**
 * Copy data from a reference block to a buffer ready to be written out
 * (exposed for testing).
 *
 * @param block   The block to copy
 * @param buffer  The char buffer to fill with the packed block
 **/
void packReferenceBlock(struct reference_block *block, void *buffer);

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
int getReferenceStatus(struct ref_counts   *refCounts,
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
bool findFreeBlock(const struct ref_counts *refCounts,
                   SlabBlockNumber          startIndex,
                   SlabBlockNumber          endIndex,
                   SlabBlockNumber         *indexPtr)
  __attribute__((warn_unused_result));

/**
 * Request a ref_counts object save its oldest dirty block asynchronously.
 *
 * @param refCounts  The ref_counts object to notify
 **/
void saveOldestReferenceBlock(struct ref_counts *refCounts);

/**
 * Reset all reference counts back to RS_FREE.
 *
 * @param refCounts   The reference counters to reset
 **/
void resetReferenceCounts(struct ref_counts *refCounts);

#endif // REF_COUNTS_INTERNALS_H
