/* * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/ref-counts.h#1 $
 */

#ifndef REF_COUNTS_H
#define REF_COUNTS_H

#include "block-allocator.h"
#include "completion.h"
#include "journal-point.h"
#include "packed-reference-block.h"
#include "slab.h"
#include "types.h"
#include "wait-queue.h"

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

/**
 * Create a reference counting object.
 *
 * <p>A reference counting object can keep a reference count for every physical
 * block in the VDO configuration. Since we expect the vast majority of the
 * blocks to have 0 or 1 reference counts, the structure is optimized for that
 * situation.
 *
 * @param [in]  block_count         The number of physical blocks that can be
 *                                  referenced
 * @param [in]  slab                The slab of the ref counts object
 * @param [in]  origin              The layer PBN at which to save ref_counts
 * @param [in]  read_only_notifier  The context for tracking read-only mode
 * @param [out] ref_counts_ptr      The pointer to hold the new ref counts object
 *
 * @return a success or error code
 **/
int __must_check
make_vdo_ref_counts(block_count_t block_count,
		    struct vdo_slab *slab,
		    physical_block_number_t origin,
		    struct read_only_notifier *read_only_notifier,
		    struct ref_counts **ref_counts_ptr);

/**
 * Free a reference counting object.
 *
 * @param ref_counts  The object to free
 **/
void free_vdo_ref_counts(struct ref_counts *ref_counts);

/**
 * Check whether a ref_counts is active.
 *
 * @param ref_counts  The ref_counts to check
 **/
bool __must_check are_vdo_ref_counts_active(struct ref_counts *ref_counts);

/**
 * Reset the free block search back to the first reference counter
 * in the first reference block.
 *
 * @param ref_counts  The ref_counts object containing the search cursor
 **/
void vdo_reset_search_cursor(struct ref_counts *ref_counts);

/**
 * Get the stored count of the number of blocks that are currently free.
 *
 * @param  ref_counts  The ref_counts object
 *
 * @return the number of blocks with a reference count of zero
 **/
block_count_t __must_check
vdo_get_unreferenced_block_count(struct ref_counts *ref_counts);

/**
 * Determine how many times a reference count can be incremented without
 * overflowing.
 *
 * @param  ref_counts  The ref_counts object
 * @param  pbn         The physical block number
 *
 * @return the number of increments that can be performed
 **/
uint8_t __must_check
vdo_get_available_references(struct ref_counts *ref_counts,
			     physical_block_number_t pbn);

/**
 * Adjust the reference count of a block.
 *
 * @param [in]  ref_counts           The refcounts object
 * @param [in]  operation            The operation to perform
 * @param [in]  slab_journal_point   The slab journal entry for this adjustment
 * @param [out] free_status_changed  A pointer which will be set to true if the
 *                                   free status of the block changed
 *
 *
 * @return A success or error code, specifically:
 *           VDO_REF_COUNT_INVALID   if a decrement would result in a negative
 *                                   reference count, or an increment in a
 *                                   count greater than MAXIMUM_REFS
 *
 **/
int __must_check
vdo_adjust_reference_count(struct ref_counts *ref_counts,
			   struct reference_operation operation,
			   const struct journal_point *slab_journal_point,
			   bool *free_status_changed);

/**
 * Adjust the reference count of a block during rebuild.
 *
 * @param ref_counts  The refcounts object
 * @param pbn         The number of the block to adjust
 * @param operation   The operation to perform on the count
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_adjust_reference_count_for_rebuild(struct ref_counts *ref_counts,
				       physical_block_number_t pbn,
				       enum journal_operation operation);

/**
 * Replay the reference count adjustment from a slab journal entry into the
 * reference count for a block. The adjustment will be ignored if it was
 * already recorded in the reference count.
 *
 * @param ref_counts   The refcounts object
 * @param entry_point  The slab journal point for the entry
 * @param entry        The slab journal entry being replayed
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
vdo_replay_reference_count_change(struct ref_counts *ref_counts,
				  const struct journal_point *entry_point,
				  struct slab_journal_entry entry);

/**
 * Find a block with a reference count of zero in the range of physical block
 * numbers tracked by the reference counter. If a free block is found, that
 * block is allocated by marking it as provisionally referenced, and the
 * allocated block number is returned.
 *
 * @param [in]  ref_counts     The reference counters to scan
 * @param [out] allocated_ptr  A pointer to hold the physical block number of
 *                             the block that was found and allocated
 *
 * @return VDO_SUCCESS if a free block was found and allocated;
 *         VDO_NO_SPACE if there are no unreferenced blocks;
 *         otherwise an error code
 **/
int __must_check
vdo_allocate_unreferenced_block(struct ref_counts *ref_counts,
				physical_block_number_t *allocated_ptr);

/**
 * Provisionally reference a block if it is unreferenced.
 *
 * @param ref_counts  The reference counters
 * @param pbn         The PBN to reference
 * @param lock        The pbn_lock on the block (may be NULL)
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_provisionally_reference_block(struct ref_counts *ref_counts,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock);

/**
 * Count all unreferenced blocks in a range [start_block, end_block) of
 * physical block numbers.
 *
 * @param ref_counts  The reference counters to scan
 * @param start_pbn   The physical block number at which to start
 *                    scanning (included in the scan)
 * @param end_pbn     The physical block number at which to stop
 *                    scanning (excluded from the scan)
 *
 * @return The number of unreferenced blocks
 **/
block_count_t __must_check
vdo_count_unreferenced_blocks(struct ref_counts *ref_counts,
			      physical_block_number_t start_pbn,
			      physical_block_number_t end_pbn);

/**
 * Request a ref_counts object save several dirty blocks asynchronously. This
 * function currently writes 1 / flush_divisor of the dirty blocks.
 *
 * @param ref_counts       The ref_counts object to notify
 * @param flush_divisor    The inverse fraction of the dirty blocks to write
 **/
void vdo_save_several_reference_blocks(struct ref_counts *ref_counts,
				       size_t flush_divisor);

/**
 * Ask a ref_counts object to save all its dirty blocks asynchronously.
 *
 * @param ref_counts     The ref_counts object to notify
 **/
void vdo_save_dirty_reference_blocks(struct ref_counts *ref_counts);

/**
 * Mark all reference count blocks as dirty.
 *
 * @param ref_counts  The ref_counts of the reference blocks
 **/
void vdo_dirty_all_reference_blocks(struct ref_counts *ref_counts);

/**
 * Drain all reference count I/O. Depending upon the type of drain being
 * performed (as recorded in the ref_count's vdo_slab), the reference blocks
 * may be loaded from disk or dirty reference blocks may be written out.
 *
 * @param ref_counts  The reference counts to drain
 **/
void drain_vdo_ref_counts(struct ref_counts *ref_counts);

/**
 * Mark all reference count blocks dirty and cause them to hold locks on slab
 * journal block 1.
 *
 * @param ref_counts  The ref_counts of the reference blocks
 **/
void vdo_acquire_dirty_block_locks(struct ref_counts *ref_counts);

/**
 * Dump information about this ref_counts structure.
 *
 * @param ref_counts     The ref_counts to dump
 **/
void dump_vdo_ref_counts(const struct ref_counts *ref_counts);


#endif // REF_COUNTS_H
