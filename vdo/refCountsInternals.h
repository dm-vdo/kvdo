/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCountsInternals.h#22 $
 */

#ifndef REF_COUNTS_INTERNALS_H
#define REF_COUNTS_INTERNALS_H

#include "refCounts.h"

#include "journalPoint.h"
#include "packedReferenceBlock.h"
#include "slab.h"
#include "blockAllocatorInternals.h"
#include "waitQueue.h"

/**
 * Represents the possible status of a block.
 **/
enum reference_status {
	RS_FREE, // this block is free
	RS_SINGLE, // this block is singly-referenced
	RS_SHARED, // this block is shared
	RS_PROVISIONAL // this block is provisionally allocated
};

/*
 * Reference_block structure
 *
 * Blocks are used as a proxy, permitting saves of partial refcounts.
 **/
struct reference_block {
	/** This block waits on the ref_counts to tell it to write */
	struct waiter waiter;
	/** The parent ref_count structure */
	struct ref_counts *ref_counts;
	/** The number of references in this block that represent allocations */
	block_size_t allocated_count;
	/** The slab journal block on which this block must hold a lock */
	sequence_number_t slab_journal_lock;
	/**
	 * The slab journal block which should be released when this block
	 * is committed
	 **/
	sequence_number_t slab_journal_lock_to_release;
	/** The point up to which each sector is accurate on disk */
	struct journal_point commit_points[SECTORS_PER_BLOCK];
	/** Whether this block has been modified since it was written to disk */
	bool is_dirty;
	/** Whether this block is currently writing */
	bool is_writing;
};

/**
 * The search_cursor represents the saved position of a free block search.
 **/
struct search_cursor {
	/** The reference block containing the current search index */
	struct reference_block *block;
	/**
	 * The position at which to start searching for the next free counter
	 */
	slab_block_number index;
	/**
	 * The position just past the last valid counter in the current block
	 */
	slab_block_number end_index;

	/** A pointer to the first reference block in the slab */
	struct reference_block *first_block;
	/** A pointer to the last reference block in the slab */
	struct reference_block *last_block;
};

/*
 * ref_counts structure
 *
 * A reference count is maintained for each physical block number.  The vast
 * majority of blocks have a very small reference count (usually 0 or 1).
 * For references less than or equal to MAXIMUM_REFS (254) the reference count
 * is stored in counters[pbn].
 *
 */
struct ref_counts {
	/** The slab of this reference block */
	struct vdo_slab *slab;

	/** The size of the counters array */
	uint32_t block_count;
	/** The number of free blocks */
	uint32_t free_blocks;
	/** The array of reference counts */
	vdo_refcount_t *counters; // use ALLOCATE to align data ptr

	/**
	 * The saved block pointer and array indexes for the free block search
	 */
	struct search_cursor search_cursor;

	/** A list of the dirty blocks waiting to be written out */
	struct wait_queue dirty_blocks;
	/** The number of blocks which are currently writing */
	size_t active_count;

	/** A waiter object for updating the slab summary */
	struct waiter slab_summary_waiter;
	/** Whether slab summary update is in progress */
	bool updating_slab_summary;

	/** The notifier for read-only mode */
	struct read_only_notifier *read_only_notifier;
	/**
	 * The refcount statistics, shared by all refcounts in our physical
	 * zone
	 */
	struct ref_counts_statistics *statistics;
	/** The layer PBN for the first struct reference_block */
	physical_block_number_t origin;
	/**
	 * The latest slab journal entry this ref_counts has been updated with
	 */
	struct journal_point slab_journal_point;

	/** The number of reference count blocks */
	uint32_t reference_block_count;
	/** reference count block array */
	struct reference_block blocks[];
};

/**
 * Convert a reference count to a reference status.
 *
 * @param count The count to convert
 *
 * @return  The appropriate reference status
 **/
enum reference_status __must_check
reference_count_to_status(vdo_refcount_t count);

/**
 * Convert a generic vdo_completion to a ref_counts object.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ref_counts object
 **/
struct ref_counts * __must_check
as_ref_counts(struct vdo_completion *completion);

/**
 * Get the reference block that covers the given block index (exposed for
 * testing).
 *
 * @param ref_counts  The refcounts object
 * @param index       The block index
 **/
struct reference_block * __must_check
get_reference_block(struct ref_counts *ref_counts, slab_block_number index);

/**
 * Find the reference counters for a given block (exposed for testing).
 *
 * @param block  The reference_block in question
 *
 * @return A pointer to the reference counters for this block
 **/
vdo_refcount_t * __must_check
get_reference_counters_for_block(struct reference_block *block);

/**
 * Copy data from a reference block to a buffer ready to be written out
 * (exposed for testing).
 *
 * @param block   The block to copy
 * @param buffer  The char buffer to fill with the packed block
 **/
void pack_reference_block(struct reference_block *block, void *buffer);

/**
 * Get the reference status of a block. Exposed only for unit testing.
 *
 * @param [in]  ref_counts   The refcounts object
 * @param [in]  pbn          The physical block number
 * @param [out] status_ptr   Where to put the status of the block
 *
 * @return                  A success or error code, specifically:
 *                          VDO_OUT_OF_RANGE if the pbn is out of range.
 **/
int __must_check get_reference_status(struct ref_counts *ref_counts,
				      physical_block_number_t pbn,
				      enum reference_status *status_ptr);

/**
 * Find the first block with a reference count of zero in the specified range
 * of reference counter indexes. Exposed for unit testing.
 *
 * @param [in]  ref_counts   The reference counters to scan
 * @param [in]  start_index  The array index at which to start scanning
 *                           (included in the scan)
 * @param [in]  end_index    The array index at which to stop scanning
 *                           (excluded from the scan)
 * @param [out] index_ptr    A pointer to hold the array index of the free
 *                           block
 *
 * @return true if a free block was found in the specified range
 **/
bool __must_check find_free_block(const struct ref_counts *ref_counts,
				  slab_block_number start_index,
				  slab_block_number end_index,
				  slab_block_number *index_ptr);

/**
 * Request a ref_counts object save its oldest dirty block asynchronously.
 *
 * @param ref_counts  The ref_counts object to notify
 **/
void save_oldest_reference_block(struct ref_counts *ref_counts);

/**
 * Reset all reference counts back to RS_FREE.
 *
 * @param ref_counts   The reference counters to reset
 **/
void reset_reference_counts(struct ref_counts *ref_counts);

#endif // REF_COUNTS_INTERNALS_H
