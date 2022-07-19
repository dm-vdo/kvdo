/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_DEPOT_H
#define SLAB_DEPOT_H

#include <linux/atomic.h>
#include "admin-state.h"
#include "slab-depot-format.h"
#include "statistics.h"
#include "types.h"
#include "vdo-layout.h"

/*
 * A slab_depot is responsible for managing all of the slabs and block
 * allocators of a VDO. It has a single array of slabs in order to eliminate
 * the need for additional math in order to compute which physical zone a PBN
 * is in. It also has a block_allocator per zone.
 *
 * Load operations are required to be performed on a single thread. Normal
 * operations are assumed to be performed in the appropriate zone. Allocations
 * and reference count updates must be done from the thread of their physical
 * zone. Requests to commit slab journal tail blocks from the recovery journal
 * must be done on the journal zone thread. Save operations are required to be
 * launched from the same thread as the original load operation.
 */

enum slab_depot_load_type {
	VDO_SLAB_DEPOT_NORMAL_LOAD,
	VDO_SLAB_DEPOT_RECOVERY_LOAD,
	VDO_SLAB_DEPOT_REBUILD_LOAD
};

struct slab_depot {
	zone_count_t zone_count;
	zone_count_t old_zone_count;
	struct vdo *vdo;
	struct slab_config slab_config;
	struct slab_summary *slab_summary;
	struct action_manager *action_manager;

	physical_block_number_t first_block;
	physical_block_number_t last_block;
	physical_block_number_t origin;

	/* slab_size == (1 << slab_size_shift) */
	unsigned int slab_size_shift;

	/* Determines how slabs should be queued during load */
	enum slab_depot_load_type load_type;

	/* The state for notifying slab journals to release recovery journal */
	sequence_number_t active_release_request;
	sequence_number_t new_release_request;

	/* State variables for scrubbing complete handling */
	atomic_t zones_to_scrub;

	/* Array of pointers to individually allocated slabs */
	struct vdo_slab **slabs;
	/* The number of slabs currently allocated and stored in 'slabs' */
	slab_count_t slab_count;

	/* Array of pointers to a larger set of slabs (used during resize) */
	struct vdo_slab **new_slabs;
	/* The number of slabs currently allocated and stored in 'new_slabs' */
	slab_count_t new_slab_count;
	/* The size that 'new_slabs' was allocated for */
	block_count_t new_size;

	/* The last block before resize, for rollback */
	physical_block_number_t old_last_block;
	/* The last block after resize, for resize */
	physical_block_number_t new_last_block;

	/* The block allocators for this depot */
	struct block_allocator *allocators[];
};

int __must_check
vdo_decode_slab_depot(struct slab_depot_state_2_0 state,
		      struct vdo *vdo,
		      struct partition *summary_partition,
		      struct slab_depot **depot_ptr);

void vdo_free_slab_depot(struct slab_depot *depot);

struct slab_depot_state_2_0 __must_check
vdo_record_slab_depot(const struct slab_depot *depot);

int __must_check vdo_allocate_slab_ref_counts(struct slab_depot *depot);

struct block_allocator * __must_check
vdo_get_block_allocator_for_zone(struct slab_depot *depot,
				 zone_count_t zone_number);

struct vdo_slab * __must_check
vdo_get_slab(const struct slab_depot *depot, physical_block_number_t pbn);

struct slab_journal * __must_check
vdo_get_slab_journal(const struct slab_depot *depot, physical_block_number_t pbn);

uint8_t __must_check
vdo_get_increment_limit(struct slab_depot *depot, physical_block_number_t pbn);

bool __must_check
vdo_is_physical_data_block(const struct slab_depot *depot,
			   physical_block_number_t pbn);

block_count_t __must_check
vdo_get_slab_depot_allocated_blocks(const struct slab_depot *depot);

block_count_t __must_check
vdo_get_slab_depot_data_blocks(const struct slab_depot *depot);

void vdo_get_slab_depot_statistics(const struct slab_depot *depot,
				   struct vdo_statistics *stats);

void vdo_load_slab_depot(struct slab_depot *depot,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent,
			 void *context);

void vdo_prepare_slab_depot_to_allocate(struct slab_depot *depot,
					enum slab_depot_load_type load_type,
					struct vdo_completion *parent);

void vdo_update_slab_depot_size(struct slab_depot *depot);

int __must_check
vdo_prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size);

void vdo_use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent);

void vdo_abandon_new_slabs(struct slab_depot *depot);

void vdo_drain_slab_depot(struct slab_depot *depot,
			  const struct admin_state_code *operation,
			  struct vdo_completion *parent);

void vdo_resume_slab_depot(struct slab_depot *depot, struct vdo_completion *parent);

void
vdo_commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
					   sequence_number_t recovery_block_number);

const struct slab_config * __must_check
vdo_get_slab_config(const struct slab_depot *depot);

struct slab_summary * __must_check
vdo_get_slab_summary(const struct slab_depot *depot);

void vdo_scrub_all_unrecovered_slabs(struct slab_depot *depot,
				     struct vdo_completion *parent);

block_count_t __must_check vdo_get_slab_depot_new_size(const struct slab_depot *depot);

void vdo_dump_slab_depot(const struct slab_depot *depot);

void vdo_notify_zone_finished_scrubbing(struct vdo_completion *completion);

#endif /* SLAB_DEPOT_H */
