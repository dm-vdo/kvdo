/* SPDX-License-Identifier: GPL-2.0-only */
/* * Copyright Red Hat
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

/*
 * Represents the possible status of a block.
 */
enum reference_status {
	RS_FREE, /* this block is free */
	RS_SINGLE, /* this block is singly-referenced */
	RS_SHARED, /* this block is shared */
	RS_PROVISIONAL /* this block is provisionally allocated */
};

/*
 * Reference_block structure
 *
 * Blocks are used as a proxy, permitting saves of partial refcounts.
 */
struct reference_block {
	/* This block waits on the ref_counts to tell it to write */
	struct waiter waiter;
	/* The parent ref_count structure */
	struct ref_counts *ref_counts;
	/* The number of references in this block that represent allocations */
	block_size_t allocated_count;
	/* The slab journal block on which this block must hold a lock */
	sequence_number_t slab_journal_lock;
	/*
	 * The slab journal block which should be released when this block
	 * is committed
	 */
	sequence_number_t slab_journal_lock_to_release;
	/* The point up to which each sector is accurate on disk */
	struct journal_point commit_points[VDO_SECTORS_PER_BLOCK];
	/* Whether this block has been modified since it was written to disk */
	bool is_dirty;
	/* Whether this block is currently writing */
	bool is_writing;
};

/*
 * The search_cursor represents the saved position of a free block search.
 */
struct search_cursor {
	/* The reference block containing the current search index */
	struct reference_block *block;
	/*
	 * The position at which to start searching for the next free counter
	 */
	slab_block_number index;
	/*
	 * The position just past the last valid counter in the current block
	 */
	slab_block_number end_index;

	/* A pointer to the first reference block in the slab */
	struct reference_block *first_block;
	/* A pointer to the last reference block in the slab */
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
	/* The slab of this reference block */
	struct vdo_slab *slab;

	/* The size of the counters array */
	uint32_t block_count;
	/* The number of free blocks */
	uint32_t free_blocks;
	/* The array of reference counts */
	vdo_refcount_t *counters; /* use UDS_ALLOCATE to align data ptr */

	/*
	 * The saved block pointer and array indexes for the free block search
	 */
	struct search_cursor search_cursor;

	/* A list of the dirty blocks waiting to be written out */
	struct wait_queue dirty_blocks;
	/* The number of blocks which are currently writing */
	size_t active_count;

	/* A waiter object for updating the slab summary */
	struct waiter slab_summary_waiter;
	/* Whether slab summary update is in progress */
	bool updating_slab_summary;

	/* The notifier for read-only mode */
	struct read_only_notifier *read_only_notifier;
	/*
	 * The refcount statistics, shared by all refcounts in our physical
	 * zone
	 */
	struct ref_counts_statistics *statistics;
	/* The layer PBN for the first struct reference_block */
	physical_block_number_t origin;
	/*
	 * The latest slab journal entry this ref_counts has been updated with
	 */
	struct journal_point slab_journal_point;

	/* The number of reference count blocks */
	uint32_t reference_block_count;
	/* reference count block array */
	struct reference_block blocks[];
};

int __must_check
vdo_make_ref_counts(block_count_t block_count,
		    struct vdo_slab *slab,
		    physical_block_number_t origin,
		    struct read_only_notifier *read_only_notifier,
		    struct ref_counts **ref_counts_ptr);

void vdo_free_ref_counts(struct ref_counts *ref_counts);

bool __must_check vdo_are_ref_counts_active(struct ref_counts *ref_counts);

void vdo_reset_search_cursor(struct ref_counts *ref_counts);

block_count_t __must_check
vdo_get_unreferenced_block_count(struct ref_counts *ref_counts);

uint8_t __must_check
vdo_get_available_references(struct ref_counts *ref_counts,
			     physical_block_number_t pbn);

int __must_check
vdo_adjust_reference_count(struct ref_counts *ref_counts,
			   struct reference_operation operation,
			   const struct journal_point *slab_journal_point,
			   bool *free_status_changed);

int __must_check
vdo_adjust_reference_count_for_rebuild(struct ref_counts *ref_counts,
				       physical_block_number_t pbn,
				       enum journal_operation operation);

int __must_check
vdo_replay_reference_count_change(struct ref_counts *ref_counts,
				  const struct journal_point *entry_point,
				  struct slab_journal_entry entry);

int __must_check
vdo_allocate_unreferenced_block(struct ref_counts *ref_counts,
				physical_block_number_t *allocated_ptr);

int __must_check
vdo_provisionally_reference_block(struct ref_counts *ref_counts,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock);

block_count_t __must_check
vdo_count_unreferenced_blocks(struct ref_counts *ref_counts,
			      physical_block_number_t start_pbn,
			      physical_block_number_t end_pbn);

void vdo_save_several_reference_blocks(struct ref_counts *ref_counts,
				       size_t flush_divisor);

void vdo_save_dirty_reference_blocks(struct ref_counts *ref_counts);

void vdo_dirty_all_reference_blocks(struct ref_counts *ref_counts);

void vdo_drain_ref_counts(struct ref_counts *ref_counts);

void vdo_acquire_dirty_block_locks(struct ref_counts *ref_counts);

void vdo_dump_ref_counts(const struct ref_counts *ref_counts);


#endif /* REF_COUNTS_H */
