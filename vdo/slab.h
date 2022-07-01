/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_SLAB_H
#define VDO_SLAB_H

#include <linux/list.h>

#include "permassert.h"

#include "admin-state.h"
#include "journal-point.h"
#include "reference-operation.h"
#include "types.h"

enum slab_rebuild_status {
	VDO_SLAB_REBUILT,
	VDO_SLAB_REPLAYING,
	VDO_SLAB_REQUIRES_SCRUBBING,
	VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING,
	VDO_SLAB_REBUILDING,
};

/**
 * This is the type declaration for the vdo_slab type. A vdo_slab currently
 * consists of a run of 2^23 data blocks, but that will soon change to
 * dedicate a small number of those blocks for metadata storage for the
 * reference counts and slab journal for the slab.
 **/
struct vdo_slab {
	/** A list entry to queue this slab in a block_allocator list */
	struct list_head allocq_entry;

	/** The struct block_allocator that owns this slab */
	struct block_allocator *allocator;

	/** The reference counts for the data blocks in this slab */
	struct ref_counts *reference_counts;
	/** The journal for this slab */
	struct slab_journal *journal;

	/** The slab number of this slab */
	slab_count_t slab_number;
	/**
	 * The offset in the allocator partition of the first block in this
	 * slab
	 */
	physical_block_number_t start;
	/** The offset of the first block past the end of this slab */
	physical_block_number_t end;
	/** The starting translated PBN of the slab journal */
	physical_block_number_t journal_origin;
	/** The starting translated PBN of the reference counts */
	physical_block_number_t ref_counts_origin;

	/** The administrative state of the slab */
	struct admin_state state;
	/** The status of the slab */
	enum slab_rebuild_status status;
	/** Whether the slab was ever queued for scrubbing */
	bool was_queued_for_scrubbing;

	/** The priority at which this slab has been queued for allocation */
	uint8_t priority;
};

/**
 * Convert a vdo_slab's list entry back to the vdo_slab.
 *
 * @param entry  The list entry to convert
 *
 * @return  The list entry as a vdo_slab
 **/
static inline struct vdo_slab *vdo_slab_from_list_entry(struct list_head *entry)
{
	return list_entry(entry, struct vdo_slab, allocq_entry);
}

int __must_check vdo_make_slab(physical_block_number_t slab_origin,
			       struct block_allocator *allocator,
			       physical_block_number_t translation,
			       struct recovery_journal *recovery_journal,
			       slab_count_t slab_number,
			       bool is_new,
			       struct vdo_slab **slab_ptr);

int __must_check vdo_allocate_ref_counts_for_slab(struct vdo_slab *slab);

void vdo_free_slab(struct vdo_slab *slab);

zone_count_t __must_check vdo_get_slab_zone_number(struct vdo_slab *slab);

/**
 * Check whether a slab is unrecovered.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is unrecovered
 **/
static inline bool vdo_is_unrecovered_slab(const struct vdo_slab *slab)
{
	return (slab->status != VDO_SLAB_REBUILT);
}

/**
 * Check whether a slab is being replayed into.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is replaying
 **/
static inline bool vdo_is_replaying_slab(const struct vdo_slab *slab)
{
	return (slab->status == VDO_SLAB_REPLAYING);
}

/**
 * Check whether a slab is being rebuilt.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is being rebuilt
 **/
static inline bool vdo_is_slab_rebuilding(const struct vdo_slab *slab)
{
	return (slab->status == VDO_SLAB_REBUILDING);
}

void vdo_mark_slab_replaying(struct vdo_slab *slab);

void vdo_mark_slab_unrecovered(struct vdo_slab *slab);

void vdo_open_slab(struct vdo_slab *slab);

block_count_t __must_check
get_slab_free_block_count(const struct vdo_slab *slab);

int __must_check
vdo_modify_slab_reference_count(struct vdo_slab *slab,
				const struct journal_point *journal_point,
				struct reference_operation operation);

int __must_check
vdo_acquire_provisional_reference(struct vdo_slab *slab,
				  physical_block_number_t pbn,
				  struct pbn_lock *lock);

int __must_check
vdo_slab_block_number_from_pbn(struct vdo_slab *slab,
			       physical_block_number_t physical_block_number,
			       slab_block_number *slab_block_number_ptr);

bool __must_check vdo_should_save_fully_built_slab(const struct vdo_slab *slab);

void vdo_start_slab_action(struct vdo_slab *slab,
			   const struct admin_state_code *operation,
			   struct vdo_completion *parent);

void vdo_notify_slab_journal_is_loaded(struct vdo_slab *slab, int result);

bool __must_check vdo_is_slab_open(struct vdo_slab *slab);

bool __must_check vdo_is_slab_draining(struct vdo_slab *slab);

void vdo_check_if_slab_drained(struct vdo_slab *slab);

void vdo_notify_slab_ref_counts_are_drained(struct vdo_slab *slab, int result);

bool __must_check vdo_is_slab_resuming(struct vdo_slab *slab);

void vdo_finish_scrubbing_slab(struct vdo_slab *slab);

void vdo_dump_slab(const struct vdo_slab *slab);

#endif /* VDO_SLAB_H */
