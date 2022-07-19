// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "block-allocator.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "action-manager.h"
#include "completion.h"
#include "constants.h"
#include "heap.h"
#include "num-utils.h"
#include "priority-table.h"
#include "read-only-notifier.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-iterator.h"
#include "slab-journal.h"
#include "slab-scrubber.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vdo-recovery.h"
#include "vio.h"
#include "vio-pool.h"

struct slab_journal_eraser {
	struct vdo_completion *parent;
	struct dm_kcopyd_client *client;
	block_count_t blocks;
	struct slab_iterator slabs;
};

static inline void assert_on_allocator_thread(thread_id_t thread_id,
					      const char *function_name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == thread_id),
			"%s called on correct thread",
			function_name);
}

/*
 * Slabs are essentially prioritized by an approximation of the number of free
 * blocks in the slab so slabs with lots of free blocks with be opened for
 * allocation before slabs that have few free blocks.
 */
static unsigned int calculate_slab_priority(struct vdo_slab *slab)
{
	block_count_t free_blocks = get_slab_free_block_count(slab);
	unsigned int unopened_slab_priority =
		slab->allocator->unopened_slab_priority;
	unsigned int priority;

	/*
	 * Wholly full slabs must be the only ones with lowest priority, 0.
	 *
	 * Slabs that have never been opened (empty, newly initialized, and
	 * never been written to) have lower priority than previously opened
	 * slabs that have a significant number of free blocks. This ranking
	 * causes VDO to avoid writing physical blocks for the first time
	 * unless there are very few free blocks that have been previously
	 * written to.
	 *
	 * Since VDO doesn't discard blocks currently, reusing previously
	 * written blocks makes VDO a better client of any underlying storage
	 * that is thinly-provisioned (though discarding would be better).
	 *
	 * For all other slabs, the priority is derived from the logarithm of
	 * the number of free blocks. Slabs with the same order of magnitude of
	 * free blocks have the same priority. With 2^23 blocks, the priority
	 * will range from 1 to 25. The reserved unopened_slab_priority divides
	 * the range and is skipped by the logarithmic mapping.
	 */

	if (free_blocks == 0) {
		return 0;
	}

	if (vdo_is_slab_journal_blank(slab->journal)) {
		return unopened_slab_priority;
	}

	priority = (1 + ilog2(free_blocks));
	return ((priority < unopened_slab_priority) ? priority : priority + 1);
}

static void prioritize_slab(struct vdo_slab *slab)
{
	ASSERT_LOG_ONLY(list_empty(&slab->allocq_entry),
			"a slab must not already be on a ring when prioritizing");
	slab->priority = calculate_slab_priority(slab);
	priority_table_enqueue(slab->allocator->prioritized_slabs,
			       slab->priority,
			       &slab->allocq_entry);
}

void vdo_register_slab_with_allocator(struct block_allocator *allocator,
				      struct vdo_slab *slab)
{
	allocator->slab_count++;
	allocator->last_slab = slab->slab_number;
}

static struct slab_iterator
get_slab_iterator(const struct block_allocator *allocator)
{
	return vdo_iterate_slabs(allocator->depot->slabs,
				 allocator->last_slab,
				 allocator->zone_number,
				 allocator->depot->zone_count);
}

/*
 * Implements vdo_read_only_notification.
 */
static void
notify_block_allocator_of_read_only_mode(void *listener,
					 struct vdo_completion *parent)
{
	struct block_allocator *allocator = listener;
	struct slab_iterator iterator;

	assert_on_allocator_thread(allocator->thread_id, __func__);
	iterator = get_slab_iterator(allocator);
	while (vdo_has_next_slab(&iterator)) {
		struct vdo_slab *slab = vdo_next_slab(&iterator);

		vdo_abort_slab_journal_waiters(slab->journal);
	}

	vdo_complete_completion(parent);
}

/*
 * Implements vio_constructor
 */
static int __must_check
vdo_make_block_allocator_pool_vios(struct vdo *vdo,
				   void *parent,
				   void *buffer,
				   struct vio **vio_ptr)
{
	return create_metadata_vio(vdo,
				   VIO_TYPE_SLAB_JOURNAL,
				   VIO_PRIORITY_METADATA,
				   parent,
				   buffer,
				   vio_ptr);
}

static int allocate_components(struct block_allocator *allocator,
			       struct vdo *vdo,
			       block_count_t vio_pool_size)
{
	struct slab_depot *depot = allocator->depot;
	block_count_t slab_journal_size =
		depot->slab_config.slab_journal_blocks;
	block_count_t max_free_blocks = depot->slab_config.data_blocks;
	unsigned int max_priority = (2 + ilog2(max_free_blocks));
	int result;

	result = vdo_register_read_only_listener(allocator->read_only_notifier,
						 allocator,
						 notify_block_allocator_of_read_only_mode,
						 allocator->thread_id);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_initialize_completion(&allocator->completion, vdo,
				  VDO_BLOCK_ALLOCATOR_COMPLETION);
	allocator->summary =
		vdo_get_slab_summary_for_zone(depot->slab_summary,
					      allocator->zone_number);

	result = make_vio_pool(vdo,
			       vio_pool_size,
			       allocator->thread_id,
			       vdo_make_block_allocator_pool_vios,
			       NULL,
			       &allocator->vio_pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_slab_scrubber(vdo,
					slab_journal_size,
					allocator->read_only_notifier,
					&allocator->slab_scrubber);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_priority_table(max_priority,
				     &allocator->prioritized_slabs);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * Performing well atop thin provisioned storage requires either that
	 * VDO discards freed blocks, or that the block allocator try to use
	 * slabs that already have allocated blocks in preference to slabs that
	 * have never been opened.  For reasons we have not been able to fully
	 * understand, some SSD machines have been have been very sensitive
	 * (50% reduction in test throughput) to very slight differences in the
	 * timing and locality of block allocation.  Assigning a low priority
	 * to unopened slabs (max_priority/2, say) would be ideal for the
	 * story, but anything less than a very high threshold (max_priority -
	 * 1) hurts on these machines.
	 *
	 * This sets the free block threshold for preferring to open an
	 * unopened slab to the binary floor of 3/4ths the total number of
	 * data blocks in a slab, which will generally evaluate to about half
	 * the slab size.
	 */
	allocator->unopened_slab_priority =
		(1 + ilog2((max_free_blocks * 3) / 4));

	return VDO_SUCCESS;
}

int vdo_make_block_allocator(struct slab_depot *depot,
			     zone_count_t zone_number,
			     thread_id_t thread_id,
			     nonce_t nonce,
			     block_count_t vio_pool_size,
			     struct vdo *vdo,
			     struct read_only_notifier *read_only_notifier,
			     struct block_allocator **allocator_ptr)
{
	struct block_allocator *allocator;
	int result = UDS_ALLOCATE(1, struct block_allocator, __func__, &allocator);

	if (result != VDO_SUCCESS) {
		return result;
	}

	allocator->depot = depot;
	allocator->zone_number = zone_number;
	allocator->thread_id = thread_id;
	allocator->nonce = nonce;
	allocator->read_only_notifier = read_only_notifier;
	INIT_LIST_HEAD(&allocator->dirty_slab_journals);
	vdo_set_admin_state_code(&allocator->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	result = allocate_components(allocator, vdo, vio_pool_size);
	if (result != VDO_SUCCESS) {
		vdo_free_block_allocator(allocator);
		return result;
	}

	*allocator_ptr = allocator;
	return VDO_SUCCESS;
}

void vdo_free_block_allocator(struct block_allocator *allocator)
{
	if (allocator == NULL) {
		return;
	}

	if (allocator->eraser != NULL) {
		dm_kcopyd_client_destroy(UDS_FORGET(allocator->eraser));
	}

	vdo_free_slab_scrubber(UDS_FORGET(allocator->slab_scrubber));
	free_vio_pool(UDS_FORGET(allocator->vio_pool));
	free_priority_table(UDS_FORGET(allocator->prioritized_slabs));
	UDS_FREE(allocator);
}

/*
 * Queue a slab for allocation or scrubbing.
 */
void vdo_queue_slab(struct vdo_slab *slab)
{
	struct block_allocator *allocator = slab->allocator;
	block_count_t free_blocks;
	int result;

	ASSERT_LOG_ONLY(list_empty(&slab->allocq_entry),
			"a requeued slab must not already be on a ring");
	free_blocks = get_slab_free_block_count(slab);
	result = ASSERT((free_blocks <=
			 allocator->depot->slab_config.data_blocks),
			"rebuilt slab %u must have a valid free block count (has %llu, expected maximum %llu)",
			slab->slab_number,
			(unsigned long long) free_blocks,
			(unsigned long long) allocator->depot->slab_config.data_blocks);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(allocator->read_only_notifier, result);
		return;
	}

	if (vdo_is_unrecovered_slab(slab)) {
		vdo_register_slab_for_scrubbing(allocator->slab_scrubber,
						slab, false);
		return;
	}

	if (!vdo_is_slab_resuming(slab)) {
		/*
		 * If the slab is resuming, we've already accounted for it
		 * here, so don't do it again.
		 * FIXME: under what situation would the slab be resuming here?
		 */
		WRITE_ONCE(allocator->allocated_blocks,
			   allocator->allocated_blocks - free_blocks);
		if (!vdo_is_slab_journal_blank(slab->journal)) {
			WRITE_ONCE(allocator->statistics.slabs_opened,
				   allocator->statistics.slabs_opened + 1);
		}
	}

	vdo_resume_slab_journal(slab->journal);
	prioritize_slab(slab);
}

/*
 * Adjust the free block count and (if needed) reprioritize the slab.
 * @increment should be true if the free block count went up.
 */
void vdo_adjust_free_block_count(struct vdo_slab *slab, bool increment)
{
	struct block_allocator *allocator = slab->allocator;

	WRITE_ONCE(allocator->allocated_blocks,
		   allocator->allocated_blocks + (increment ? -1 : 1));

	/* The open slab doesn't need to be reprioritized until it is closed. */
	if (slab == allocator->open_slab) {
		return;
	}

	/*
	 * Don't bother adjusting the priority table if unneeded.
	 */
	if (slab->priority == calculate_slab_priority(slab)) {
		return;
	}

	/*
	 * Reprioritize the slab to reflect the new free block count by
	 * removing it from the table and re-enqueuing it with the new
	 * priority.
	 */
	priority_table_remove(allocator->prioritized_slabs,
			      &slab->allocq_entry);
	prioritize_slab(slab);
}

static int allocate_slab_block(struct vdo_slab *slab,
			       physical_block_number_t *block_number_ptr)
{
	physical_block_number_t pbn;
	int result =
		vdo_allocate_unreferenced_block(slab->reference_counts, &pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_adjust_free_block_count(slab, false);

	*block_number_ptr = pbn;
	return VDO_SUCCESS;
}

/*
 * The block allocated will have a provisional reference and the reference
 * must be either confirmed with a subsequent increment or vacated with a
 * subsequent decrement via vdo_release_block_reference().
 */
int vdo_allocate_block(struct block_allocator *allocator,
		       physical_block_number_t *block_number_ptr)
{
	if (allocator->open_slab != NULL) {
		/* Try to allocate the next block in the currently open slab. */
		int result =
			allocate_slab_block(allocator->open_slab, block_number_ptr);
		if ((result == VDO_SUCCESS) || (result != VDO_NO_SPACE)) {
			return result;
		}

		/* Put the exhausted open slab back into the priority table. */
		prioritize_slab(allocator->open_slab);
	}

	/*
	 * Remove the highest priority slab from the priority table and make it
	 * the open slab.
	 */
	allocator->open_slab =
		vdo_slab_from_list_entry(priority_table_dequeue(allocator->prioritized_slabs));
	vdo_open_slab(allocator->open_slab);

	/*
	 * Try allocating again. If we're out of space immediately after
	 * opening a slab, then every slab must be fully allocated.
	 */
	return allocate_slab_block(allocator->open_slab, block_number_ptr);
}

/*
 * Release an unused provisional reference.
 */
void vdo_release_block_reference(struct block_allocator *allocator,
				 physical_block_number_t pbn,
				 const char *why)
{
	struct vdo_slab *slab;
	int result;
	struct reference_operation operation = {
		.type = VDO_JOURNAL_DATA_DECREMENT,
		.pbn = pbn,
	};

	if (pbn == VDO_ZERO_BLOCK) {
		return;
	}

	slab = vdo_get_slab(allocator->depot, pbn);
	result = vdo_modify_slab_reference_count(slab, NULL, operation);
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result,
				       "Failed to release reference to %s physical block %llu",
				       why,
				       (unsigned long long) pbn);
	}
}

/*
 * This is a heap_comparator function that orders slab_status
 * structures using the 'is_clean' field as the primary key and the
 * 'emptiness' field as the secondary key.
 *
 * Slabs need to be pushed onto the rings in the same order they are
 * to be popped off. Popping should always get the most empty first,
 * so pushing should be from most empty to least empty. Thus, the
 * comparator order is the usual sense since the heap structure
 * returns larger elements before smaller ones.
 *
 * @param item1  The first item to compare
 * @param item2  The second item to compare
 *
 * @return  1 if the first item is cleaner or emptier than the second;
 *          0 if the two items are equally clean and empty;
	   -1 otherwise
 */
static int compare_slab_statuses(const void *item1, const void *item2)
{
	const struct slab_status *info1 = (const struct slab_status *) item1;
	const struct slab_status *info2 = (const struct slab_status *) item2;

	if (info1->is_clean != info2->is_clean) {
		return (info1->is_clean ? 1 : -1);
	}
	if (info1->emptiness != info2->emptiness) {
		return ((info1->emptiness > info2->emptiness) ? 1 : -1);
	}
	return (info1->slab_number < info2->slab_number) ? 1 : -1;
}

/*
 * Implements heap_swapper.
 */
static void swap_slab_statuses(void *item1, void *item2)
{
	struct slab_status *info1 = item1;
	struct slab_status *info2 = item2;
	struct slab_status temp = *info1;
	*info1 = *info2;
	*info2 = temp;
}

static struct block_allocator *
as_block_allocator(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_BLOCK_ALLOCATOR_COMPLETION);
	return container_of(completion, struct block_allocator, completion);
}

/*
 * Inform the slab actor that a action has finished on some slab; used by
 * apply_to_slabs().
 */
static void slab_action_callback(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);
	struct slab_actor *actor = &allocator->slab_actor;

	if (--actor->slab_action_count == 0) {
		actor->callback(completion);
		return;
	}

	vdo_reset_completion(completion);
}

/*
 * Preserve the error from part of an action and continue.
 */
static void handle_operation_error(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	vdo_set_operation_result(&allocator->state, completion->result);
	completion->callback(completion);
}

/*
 * Perform an action on each of an allocator's slabs in parallel.
 */
static void apply_to_slabs(struct block_allocator *allocator,
			   vdo_action *callback)
{
	struct slab_iterator iterator;

	vdo_prepare_completion(&allocator->completion,
			       slab_action_callback,
			       handle_operation_error,
			       allocator->thread_id,
			       NULL);
	allocator->completion.requeue = false;

	/*
	 * Since we are going to dequeue all of the slabs, the open slab will
	 * become invalid, so clear it.
	 */
	allocator->open_slab = NULL;

	/* Ensure that we don't finish before we're done starting. */
	allocator->slab_actor = (struct slab_actor) {
		.slab_action_count = 1,
		.callback = callback,
	};

	iterator = get_slab_iterator(allocator);
	while (vdo_has_next_slab(&iterator)) {
		const struct admin_state_code *operation =
			vdo_get_admin_state_code(&allocator->state);
		struct vdo_slab *slab = vdo_next_slab(&iterator);

		list_del_init(&slab->allocq_entry);
		allocator->slab_actor.slab_action_count++;
		vdo_start_slab_action(slab, operation, &allocator->completion);
	}

	slab_action_callback(&allocator->completion);
}

static void finish_loading_allocator(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);
	const struct admin_state_code *operation =
		vdo_get_admin_state_code(&allocator->state);

	if (allocator->eraser != NULL) {
		dm_kcopyd_client_destroy(UDS_FORGET(allocator->eraser));
	}

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_RECOVERY) {
		void *context =
			vdo_get_current_action_context(allocator->depot->action_manager);
		vdo_replay_into_slab_journals(allocator, completion, context);
		return;
	}

	vdo_finish_loading(&allocator->state);
}

static void erase_next_slab_journal(struct block_allocator *allocator);

static void copy_callback(int read_err, unsigned long write_err, void *context)
{
	struct block_allocator *allocator = context;
	int result = (((read_err == 0) && (write_err == 0))
		      ? VDO_SUCCESS : -EIO);

	if (result != VDO_SUCCESS) {
		vdo_finish_completion(&allocator->completion, result);
		return;
	}

	erase_next_slab_journal(allocator);
}

/**
 * erase_next_slab_journal() - Erase the next slab journal.
 */
static void erase_next_slab_journal(struct block_allocator *allocator)
{
	struct vdo_slab *slab;
	physical_block_number_t pbn;
	struct dm_io_region regions[1];
	struct slab_depot *depot = allocator->depot;
	block_count_t blocks = depot->slab_config.slab_journal_blocks;

	if (!vdo_has_next_slab(&allocator->slabs_to_erase)) {
		vdo_finish_completion(&allocator->completion, VDO_SUCCESS);
		return;
	}

	slab = vdo_next_slab(&allocator->slabs_to_erase);
	pbn = slab->journal_origin - depot->vdo->geometry.bio_offset;
	regions[0] = (struct dm_io_region) {
		.bdev = vdo_get_backing_device(depot->vdo),
		.sector = pbn * VDO_SECTORS_PER_BLOCK,
		.count = blocks * VDO_SECTORS_PER_BLOCK,
	};
	dm_kcopyd_zero(allocator->eraser, 1, regions, 0, copy_callback,
		       allocator);
}

/*
 * Implements vdo_admin_initiator.
 */
static void initiate_load(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	const struct admin_state_code *operation = vdo_get_admin_state_code(state);

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_REBUILD) {
		/*
		 * Must requeue because the kcopyd client cannot be freed in
		 * the same stack frame as the kcopyd callback, lest it
		 * deadlock.
		 */
		vdo_prepare_completion_for_requeue(&allocator->completion,
						   finish_loading_allocator,
						   handle_operation_error,
						   allocator->thread_id,
						   NULL);
		allocator->eraser = dm_kcopyd_client_create(NULL);
		if (allocator->eraser == NULL) {
			vdo_finish_completion(&allocator->completion, -ENOMEM);
			return;
		}
		allocator->slabs_to_erase = get_slab_iterator(allocator);

		erase_next_slab_journal(allocator);
		return;
	}

	apply_to_slabs(allocator, finish_loading_allocator);
}

/*
 * Implements vdo_zone_action.
 */
void vdo_load_block_allocator(void *context,
			      zone_count_t zone_number,
			      struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	vdo_start_loading(
		&allocator->state,
		vdo_get_current_manager_operation(allocator->depot->action_manager),
		parent,
		initiate_load);
}

/*
 * Inform a block allocator that its slab journals have been recovered from the
 * recovery journal.
 */
void vdo_notify_slab_journals_are_recovered(struct block_allocator *allocator,
					    int result)
{
	vdo_finish_loading_with_result(&allocator->state, result);
}

/*
 * Prepare slabs for allocation or scrubbing.
 */
static int __must_check
vdo_prepare_slabs_for_allocation(struct block_allocator *allocator)
{
	struct slab_status current_slab_status;
	struct heap heap;
	int result;
	struct slab_status *slab_statuses;
	struct slab_depot *depot = allocator->depot;
	slab_count_t slab_count = depot->slab_count;

	block_count_t allocated_count
		= (allocator->slab_count * depot->slab_config.data_blocks);
	WRITE_ONCE(allocator->allocated_blocks, allocated_count);

	result = UDS_ALLOCATE(slab_count, struct slab_status, __func__,
			      &slab_statuses);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_get_summarized_slab_statuses(allocator->summary, slab_count,
					 slab_statuses);

	/* Sort the slabs by cleanliness, then by emptiness hint. */
	initialize_heap(&heap,
			compare_slab_statuses,
			swap_slab_statuses,
			slab_statuses,
			slab_count,
			sizeof(struct slab_status));
	build_heap(&heap, slab_count);

	while (pop_max_heap_element(&heap, &current_slab_status)) {
		bool high_priority;
		struct vdo_slab *slab =
			depot->slabs[current_slab_status.slab_number];
		if (slab->allocator != allocator) {
			continue;
		}

		if ((depot->load_type == VDO_SLAB_DEPOT_REBUILD_LOAD) ||
		    (!vdo_must_load_ref_counts(allocator->summary,
					       slab->slab_number) &&
		     current_slab_status.is_clean)) {
			vdo_queue_slab(slab);
			continue;
		}

		vdo_mark_slab_unrecovered(slab);
		high_priority = ((current_slab_status.is_clean &&
				 (depot->load_type == VDO_SLAB_DEPOT_NORMAL_LOAD)) ||
				 vdo_slab_journal_requires_scrubbing(slab->journal));
		vdo_register_slab_for_scrubbing(allocator->slab_scrubber,
						slab,
						high_priority);
	}
	UDS_FREE(slab_statuses);

	return VDO_SUCCESS;
}

/*
 * Implements vdo_zone_action.
 */
void vdo_prepare_block_allocator_to_allocate(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	int result = vdo_prepare_slabs_for_allocation(allocator);

	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	vdo_scrub_high_priority_slabs(allocator->slab_scrubber,
				      is_priority_table_empty(allocator->prioritized_slabs),
				      parent,
				      vdo_finish_completion_parent_callback,
				      vdo_finish_completion_parent_callback);
}

/*
 * Implements vdo_zone_action.
 */
void vdo_register_new_slabs_for_allocator(void *context,
					  zone_count_t zone_number,
					  struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	struct slab_depot *depot = allocator->depot;
	slab_count_t i;

	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];

		if (slab->allocator == allocator) {
			vdo_register_slab_with_allocator(allocator, slab);
		}
	}
	vdo_complete_completion(parent);
}

static void do_drain_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	vdo_prepare_completion_for_requeue(&allocator->completion,
					   do_drain_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (++allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		vdo_stop_slab_scrubbing(allocator->slab_scrubber, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_drain_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		vdo_drain_slab_summary_zone(
			allocator->summary,
			vdo_get_admin_state_code(&allocator->state),
			completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_FINISHED:
		ASSERT_LOG_ONLY(!is_vio_pool_busy(allocator->vio_pool),
				"vio pool not busy");
		vdo_finish_draining_with_result(&allocator->state,
						completion->result);
		return;

	default:
		vdo_finish_draining_with_result(&allocator->state,
						UDS_BAD_STATE);
	}
}

/*
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = VDO_DRAIN_ALLOCATOR_START;
	do_drain_step(&allocator->completion);
}

/*
 * Drain all allocator I/O. Depending upon the type of drain, some or all
 * dirty metadata may be written to disk. The type of drain will be determined
 * from the state of the allocator's depot.
 *
 * Implements vdo_zone_action.
 */
void vdo_drain_block_allocator(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	vdo_start_draining(
		&allocator->state,
		vdo_get_current_manager_operation(allocator->depot->action_manager),
		parent,
		initiate_drain);
}

static void do_resume_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	vdo_prepare_completion_for_requeue(&allocator->completion,
					   do_resume_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (--allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		vdo_resume_slab_summary_zone(allocator->summary, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_resume_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		vdo_resume_slab_scrubbing(allocator->slab_scrubber, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_START:
		vdo_finish_resuming_with_result(&allocator->state,
						completion->result);
		return;

	default:
		vdo_finish_resuming_with_result(&allocator->state,
						UDS_BAD_STATE);
	}
}

/*
 * Implements vdo_admin_initiator.
 */
static void initiate_resume(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = VDO_DRAIN_ALLOCATOR_STEP_FINISHED;
	do_resume_step(&allocator->completion);
}

/*
 * Implements vdo_zone_action.
 */
void vdo_resume_block_allocator(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	vdo_start_resuming(&allocator->state,
			   vdo_get_current_manager_operation(allocator->depot->action_manager),
			   parent,
			   initiate_resume);
}

/*
 * Request a commit of all dirty tail blocks which are locking the recovery
 * journal block the depot is seeking to release.
 *
 * Implements vdo_zone_action.
 */
void vdo_release_tail_block_locks(void *context,
				  zone_count_t zone_number,
				  struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	struct list_head *list = &allocator->dirty_slab_journals;

	while (!list_empty(list)) {
		if (!vdo_release_recovery_journal_lock(vdo_slab_journal_from_dirty_entry(list->next),
						       allocator->depot->active_release_request)) {
			break;
		}
	}
	vdo_complete_completion(parent);
}

int vdo_acquire_block_allocator_vio(struct block_allocator *allocator,
				    struct waiter *waiter)
{
	return acquire_vio_from_pool(allocator->vio_pool, waiter);
}

void vdo_return_block_allocator_vio(struct block_allocator *allocator,
				    struct vio_pool_entry *entry)
{
	return_vio_to_pool(allocator->vio_pool, entry);
}

/*
 * Implements vdo_zone_action.
 */
void vdo_scrub_all_unrecovered_slabs_in_zone(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	vdo_scrub_slabs(allocator->slab_scrubber,
			allocator->depot,
			vdo_notify_zone_finished_scrubbing,
			vdo_noop_completion_callback);
	vdo_complete_completion(parent);
}


struct block_allocator_statistics
vdo_get_block_allocator_statistics(const struct block_allocator *allocator)
{
	const struct block_allocator_statistics *stats =
		&allocator->statistics;
	return (struct block_allocator_statistics) {
		.slab_count = allocator->slab_count,
		.slabs_opened = READ_ONCE(stats->slabs_opened),
		.slabs_reopened = READ_ONCE(stats->slabs_reopened),
	};
}

struct slab_journal_statistics
vdo_get_slab_journal_statistics(const struct block_allocator *allocator)
{
	const struct slab_journal_statistics *stats =
		&allocator->slab_journal_statistics;
	return (struct slab_journal_statistics) {
		.disk_full_count = READ_ONCE(stats->disk_full_count),
		.flush_count = READ_ONCE(stats->flush_count),
		.blocked_count = READ_ONCE(stats->blocked_count),
		.blocks_written = READ_ONCE(stats->blocks_written),
		.tail_busy_count = READ_ONCE(stats->tail_busy_count),
	};
}

struct ref_counts_statistics
vdo_get_ref_counts_statistics(const struct block_allocator *allocator)
{
	const struct ref_counts_statistics *stats =
		&allocator->ref_counts_statistics;
	return (struct ref_counts_statistics) {
		.blocks_written = READ_ONCE(stats->blocks_written),
	};
}

void vdo_dump_block_allocator(const struct block_allocator *allocator)
{
	unsigned int pause_counter = 0;
	struct slab_iterator iterator = get_slab_iterator(allocator);

	uds_log_info("block_allocator zone %u", allocator->zone_number);
	while (vdo_has_next_slab(&iterator)) {
		vdo_dump_slab(vdo_next_slab(&iterator));

		/*
		 * Wait for a while after each batch of 32 slabs dumped, an
		 * arbitrary number, allowing the kernel log a chance to be
		 * flushed instead of being overrun.
		 */
		if (pause_counter++ == 31) {
			pause_counter = 0;
			uds_pause_for_logger();
		}
	}

	vdo_dump_slab_scrubber(allocator->slab_scrubber);
}
