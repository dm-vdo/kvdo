/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BLOCK_ALLOCATOR_H
#define BLOCK_ALLOCATOR_H

#include <linux/dm-kcopyd.h>

#include "admin-state.h"
#include "priority-table.h"
#include "slab-scrubber.h"
#include "slab-iterator.h"
#include "statistics.h"
#include "types.h"
#include "vio-pool.h"
#include "wait-queue.h"

enum {
	/*
	 * The number of vios in the vio pool is proportional to the throughput
	 * of the VDO.
	 */
	VIO_POOL_SIZE = 128,
};

enum block_allocator_drain_step {
	VDO_DRAIN_ALLOCATOR_START,
	VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER,
	VDO_DRAIN_ALLOCATOR_STEP_SLABS,
	VDO_DRAIN_ALLOCATOR_STEP_SUMMARY,
	VDO_DRAIN_ALLOCATOR_STEP_FINISHED,
};

/*
 * A sub-structure for applying actions in parallel to all an allocator's
 * slabs.
 */
struct slab_actor {
	/* The number of slabs performing a slab action */
	slab_count_t slab_action_count;
	/* The method to call when a slab action has been completed by all
	 * slabs
	 */
	vdo_action *callback;
};

struct block_allocator {
	struct vdo_completion completion;
	/* The slab depot for this allocator */
	struct slab_depot *depot;
	/* The slab summary zone for this allocator */
	struct slab_summary_zone *summary;
	/* The notifier for entering read-only mode */
	struct read_only_notifier *read_only_notifier;
	/* The nonce of the VDO */
	nonce_t nonce;
	/* The physical zone number of this allocator */
	zone_count_t zone_number;
	/* The thread ID for this allocator's physical zone */
	thread_id_t thread_id;
	/* The number of slabs in this allocator */
	slab_count_t slab_count;
	/* The number of the last slab owned by this allocator */
	slab_count_t last_slab;
	/* The reduced priority level used to preserve unopened slabs */
	unsigned int unopened_slab_priority;
	/* The state of this allocator */
	struct admin_state state;
	/* The actor for applying an action to all slabs */
	struct slab_actor slab_actor;

	/* The slab from which blocks are currently being allocated */
	struct vdo_slab *open_slab;
	/* A priority queue containing all slabs available for allocation */
	struct priority_table *prioritized_slabs;
	/* The slab scrubber */
	struct slab_scrubber *slab_scrubber;
	/* What phase of the close operation the allocator is to perform */
	enum block_allocator_drain_step drain_step;

	/*
	 * These statistics are all mutated only by the physical zone thread,
	 * but are read by other threads when gathering statistics for the
	 * entire depot.
	 */
	/*
	 * The count of allocated blocks in this zone. Not in
	 * block_allocator_statistics for historical reasons.
	 */
	uint64_t allocated_blocks;
	/* Statistics for this block allocator */
	struct block_allocator_statistics statistics;
	/* Cumulative statistics for the slab journals in this zone */
	struct slab_journal_statistics slab_journal_statistics;
	/* Cumulative statistics for the ref_counts in this zone */
	struct ref_counts_statistics ref_counts_statistics;

	/*
	 * This is the head of a queue of slab journals which have entries in
	 * their tail blocks which have not yet started to commit. When the
	 * recovery journal is under space pressure, slab journals which have
	 * uncommitted entries holding a lock on the recovery journal head are
	 * forced to commit their blocks early. This list is kept in order,
	 * with the tail containing the slab journal holding the most recent
	 * recovery journal lock.
	 */
	struct list_head dirty_slab_journals;

	/* The vio pool for reading and writing block allocator metadata */
	struct vio_pool *vio_pool;
	/* The dm_kcopyd client for erasing slab journals */
	struct dm_kcopyd_client *eraser;
	/* Iterator over the slabs to be erased */
	struct slab_iterator slabs_to_erase;
};

int __must_check
vdo_make_block_allocator(struct slab_depot *depot,
			 zone_count_t zone_number,
			 thread_id_t thread_id,
			 nonce_t nonce,
			 block_count_t vio_pool_size,
			 struct vdo *vdo,
			 struct read_only_notifier *read_only_notifier,
			 struct block_allocator **allocator_ptr);

void vdo_free_block_allocator(struct block_allocator *allocator);

void vdo_queue_slab(struct vdo_slab *slab);

void vdo_adjust_free_block_count(struct vdo_slab *slab, bool increment);

int __must_check vdo_allocate_block(struct block_allocator *allocator,
				    physical_block_number_t *block_number_ptr);

void vdo_release_block_reference(struct block_allocator *allocator,
				 physical_block_number_t pbn,
				 const char *why);

void vdo_load_block_allocator(void *context,
			      zone_count_t zone_number,
			      struct vdo_completion *parent);

void vdo_notify_slab_journals_are_recovered(struct block_allocator *allocator,
					    int result);

void vdo_prepare_block_allocator_to_allocate(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent);

void vdo_register_slab_with_allocator(struct block_allocator *allocator,
				      struct vdo_slab *slab);

void vdo_register_new_slabs_for_allocator(void *context,
					  zone_count_t zone_number,
					  struct vdo_completion *parent);

void vdo_drain_block_allocator(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent);

void vdo_resume_block_allocator(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent);

void vdo_release_tail_block_locks(void *context,
				  zone_count_t zone_number,
				  struct vdo_completion *parent);

int __must_check
vdo_acquire_block_allocator_vio(struct block_allocator *allocator,
				struct waiter *waiter);

void vdo_return_block_allocator_vio(struct block_allocator *allocator,
				    struct vio_pool_entry *entry);

void vdo_scrub_all_unrecovered_slabs_in_zone(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent);

struct block_allocator_statistics __must_check
vdo_get_block_allocator_statistics(const struct block_allocator *allocator);

struct slab_journal_statistics __must_check
vdo_get_slab_journal_statistics(const struct block_allocator *allocator);

struct ref_counts_statistics __must_check
vdo_get_ref_counts_statistics(const struct block_allocator *allocator);

void vdo_dump_block_allocator(const struct block_allocator *allocator);

#endif /* BLOCK_ALLOCATOR_H */
