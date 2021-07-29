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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCountsInternals.h#27 $
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
	struct journal_point commit_points[VDO_SECTORS_PER_BLOCK];
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
	vdo_refcount_t *counters; // use UDS_ALLOCATE to align data ptr

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
 * Convert a generic vdo_completion to a ref_counts object.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ref_counts object
 **/
struct ref_counts * __must_check
as_vdo_ref_counts(struct vdo_completion *completion);


#endif // REF_COUNTS_INTERNALS_H
