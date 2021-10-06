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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/block-allocator.c#2 $
 */

#include "block-allocator.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "action-manager.h"
#include "completion.h"
#include "heap.h"
#include "num-utils.h"
#include "priority-table.h"
#include "read-only-notifier.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-iterator.h"
#include "slab-journal-eraser.h"
#include "slab-journal.h"
#include "slab-scrubber.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vdo-recovery.h"
#include "vio.h"
#include "vio-pool.h"

/**
 * Assert that a block allocator function was called from the correct thread.
 *
 * @param thread_id      The allocator's thread id
 * @param function_name  The name of the function
 **/
static inline void assert_on_allocator_thread(thread_id_t thread_id,
					      const char *function_name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == thread_id),
			"%s called on correct thread",
			function_name);
}

/**
 * Get the priority for a slab in the allocator's slab queue. Slabs are
 * essentially prioritized by an approximation of the number of free blocks in
 * the slab so slabs with lots of free blocks with be opened for allocation
 * before slabs that have few free blocks.
 *
 * @param slab  The slab whose queue priority is desired
 *
 * @return the queue priority of the slab
 **/
static unsigned int calculate_slab_priority(struct vdo_slab *slab)
{
	block_count_t free_blocks = get_slab_free_block_count(slab);
	unsigned int unopened_slab_priority =
		slab->allocator->unopened_slab_priority;
	unsigned int priority;

	// Slabs that are completely full must be the only ones with the lowest
	// priority: zero.
	if (free_blocks == 0) {
		return 0;
	}

	/*
	 * Slabs that have never been opened (empty, newly initialized, never
	 * been written to) have lower priority than previously opened slabs
	 * that have a signficant number of free blocks. This ranking causes
	 * VDO to avoid writing physical blocks for the first time until there
	 * are very few free blocks that have been previously written to. That
	 * policy makes VDO a better client of any underlying storage that is
	 * thinly-provisioned [VDOSTORY-123].
	 */
	if (is_vdo_slab_journal_blank(slab->journal)) {
		return unopened_slab_priority;
	}

	/*
	 * For all other slabs, the priority is derived from the logarithm of
	 * the number of free blocks. Slabs with the same order of magnitude of
	 * free blocks have the same priority. With 2^23 blocks, the priority
	 * will range from 1 to 25. The reserved unopened_slab_priority divides
	 * the range and is skipped by the logarithmic mapping.
	 */
	priority = (1 + log_base_two(free_blocks));
	return ((priority < unopened_slab_priority) ? priority : priority + 1);
}

/**
 * Add a slab to the priority queue of slabs available for allocation.
 *
 * @param slab  The slab to prioritize
 **/
static void prioritize_slab(struct vdo_slab *slab)
{
	ASSERT_LOG_ONLY(list_empty(&slab->allocq_entry),
			"a slab must not already be on a ring when prioritizing");
	slab->priority = calculate_slab_priority(slab);
	priority_table_enqueue(slab->allocator->prioritized_slabs,
			       slab->priority,
			       &slab->allocq_entry);
}

/**********************************************************************/
void register_vdo_slab_with_allocator(struct block_allocator *allocator,
				      struct vdo_slab *slab)
{
	allocator->slab_count++;
	allocator->last_slab = slab->slab_number;
}

/**
 * Get an iterator over all the slabs in the allocator.
 *
 * @param allocator  The allocator
 *
 * @return An iterator over the allocator's slabs
 **/
static struct slab_iterator
get_slab_iterator(const struct block_allocator *allocator)
{
	return vdo_iterate_slabs(allocator->depot->slabs,
				 allocator->last_slab,
				 allocator->zone_number,
				 allocator->depot->zone_count);
}

/**
 * Notify a block allocator that the VDO has entered read-only mode.
 *
 * Implements vdo_read_only_notification.
 *
 * @param listener  The block allocator
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
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

		abort_vdo_slab_journal_waiters(slab->journal);
	}

	complete_vdo_completion(parent);
}

/**
 * Construct allocator metadata vios.
 *
 * Implements vio_constructor
 **/
static int __must_check
make_vdo_block_allocator_pool_vios(struct vdo *vdo,
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

/**
 * Allocate those component of the block allocator which are needed only at
 * load time, not at format time.
 *
 * @param allocator             The allocator
 * @param vdo                   The VDO
 * @param vio_pool_size         The vio pool size
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_components(struct block_allocator *allocator,
			       struct vdo *vdo,
			       block_count_t vio_pool_size)
{
	struct slab_depot *depot = allocator->depot;
	// The number of data blocks is the maximum number of free blocks that
	// could be used in calculate_slab_priority().
	block_count_t slab_journal_size =
		depot->slab_config.slab_journal_blocks;
	block_count_t max_free_blocks = depot->slab_config.data_blocks;
	unsigned int max_priority = (2 + log_base_two(max_free_blocks));
	int result;

	result = register_vdo_read_only_listener(allocator->read_only_notifier,
						 allocator,
						 notify_block_allocator_of_read_only_mode,
						 allocator->thread_id);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&allocator->completion, vdo,
				  VDO_BLOCK_ALLOCATOR_COMPLETION);
	allocator->summary =
		get_vdo_slab_summary_for_zone(depot, allocator->zone_number);

	result = make_vio_pool(vdo,
			       vio_pool_size,
			       allocator->thread_id,
			       make_vdo_block_allocator_pool_vios,
			       NULL,
			       &allocator->vio_pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_vdo_slab_scrubber(vdo,
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
	 * VDOSTORY-123 requires that we try to open slabs that already have
	 * allocated blocks in preference to slabs that have never been opened.
	 * For reasons we have not been able to fully understand, performance
	 * tests on SSD harvards have been very sensitive (50% reduction in
	 * test throughput) to very slight differences in the timing and
	 * locality of block allocation. Assigning a low priority to unopened
	 * slabs (max_priority/2, say) would be ideal for the story, but
	 * anything less than a very high threshold (max_priority - 1) hurts
	 * PMI results.
	 *
	 * This sets the free block threshold for preferring to open an
	 * unopened slab to the binary floor of 3/4ths the total number of
	 * datablocks in a slab, which will generally evaluate to about half
	 * the slab size, but avoids degenerate behavior in unit tests where
	 * the number of data blocks is artificially constrained to a power of
	 * two.
	 */
	allocator->unopened_slab_priority =
		(1 + log_base_two((max_free_blocks * 3) / 4));

	return VDO_SUCCESS;
}

/**********************************************************************/
int make_vdo_block_allocator(struct slab_depot *depot,
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
	set_vdo_admin_state_code(&allocator->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	result = allocate_components(allocator, vdo, vio_pool_size);
	if (result != VDO_SUCCESS) {
		free_vdo_block_allocator(allocator);
		return result;
	}

	*allocator_ptr = allocator;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_block_allocator(struct block_allocator *allocator)
{
	if (allocator == NULL) {
		return;
	}

	free_vdo_slab_scrubber(UDS_FORGET(allocator->slab_scrubber));
	free_vio_pool(UDS_FORGET(allocator->vio_pool));
	free_priority_table(UDS_FORGET(allocator->prioritized_slabs));
	UDS_FREE(allocator);
}


/**
 * Get the maximum number of data blocks that can be allocated.
 *
 * @param allocator  The block allocator to query
 *
 * @return The number of data blocks that can be allocated
 **/
static inline block_count_t __must_check
get_data_block_count(const struct block_allocator *allocator)
{
	return (allocator->slab_count *
		allocator->depot->slab_config.data_blocks);
}

/**********************************************************************/
block_count_t get_vdo_allocated_blocks(const struct block_allocator *allocator)
{
	return READ_ONCE(allocator->allocated_blocks);
}

/**********************************************************************/
block_count_t
get_vdo_unrecovered_slab_count(const struct block_allocator *allocator)
{
	return get_scrubber_vdo_slab_count(allocator->slab_scrubber);
}

/**********************************************************************/
void queue_vdo_slab(struct vdo_slab *slab)
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

	if (is_unrecovered_vdo_slab(slab)) {
		vdo_register_slab_for_scrubbing(allocator->slab_scrubber,
						slab, false);
		return;
	}

	if (!is_vdo_slab_resuming(slab)) {
		// If the slab is resuming, we've already accounted for it
		// here, so don't do it again.
		WRITE_ONCE(allocator->allocated_blocks,
			   allocator->allocated_blocks - free_blocks);
		if (!is_vdo_slab_journal_blank(slab->journal)) {
			WRITE_ONCE(allocator->statistics.slabs_opened,
				   allocator->statistics.slabs_opened + 1);
		}
	}

	// All slabs are kept in a priority queue for allocation.
	prioritize_slab(slab);
}

/**********************************************************************/
void adjust_vdo_free_block_count(struct vdo_slab *slab, bool increment)
{
	struct block_allocator *allocator = slab->allocator;
	// The sense of increment is reversed since allocations are being
	// counted.
	WRITE_ONCE(allocator->allocated_blocks,
		   allocator->allocated_blocks + (increment ? -1 : 1));

	// The open slab doesn't need to be reprioritized until it is closed.
	if (slab == allocator->open_slab) {
		return;
	}

	// The slab priority rarely changes; if no change, then don't requeue
	// it.
	if (slab->priority == calculate_slab_priority(slab)) {
		return;
	}

	// Reprioritize the slab to reflect the new free block count by
	// removing it from the table and re-enqueuing it with the new
	// priority.
	priority_table_remove(allocator->prioritized_slabs,
			      &slab->allocq_entry);
	prioritize_slab(slab);
}

/**
 * Allocate the next free physical block in a slab.
 *
 * The block allocated will have a provisional reference and the
 * reference must be either confirmed with a subsequent increment
 * or vacated with a subsequent decrement of the reference count.
 *
 * @param [in]  slab              The slab
 * @param [out] block_number_ptr  A pointer to receive the allocated block
 *                                number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int allocate_slab_block(struct vdo_slab *slab,
			       physical_block_number_t *block_number_ptr)
{
	physical_block_number_t pbn;
	int result =
		vdo_allocate_unreferenced_block(slab->reference_counts, &pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	adjust_vdo_free_block_count(slab, false);

	*block_number_ptr = pbn;
	return VDO_SUCCESS;
}

/**********************************************************************/
int allocate_vdo_block(struct block_allocator *allocator,
		       physical_block_number_t *block_number_ptr)
{
	if (allocator->open_slab != NULL) {
		// Try to allocate the next block in the currently open slab.
		int result =
			allocate_slab_block(allocator->open_slab, block_number_ptr);
		if ((result == VDO_SUCCESS) || (result != VDO_NO_SPACE)) {
			return result;
		}

		// Put the exhausted open slab back into the priority table.
		prioritize_slab(allocator->open_slab);
	}

	// Remove the highest priority slab from the priority table and make it
	// the open slab.
	allocator->open_slab =
		vdo_slab_from_list_entry(priority_table_dequeue(allocator->prioritized_slabs));
	open_vdo_slab(allocator->open_slab);

	// Try allocating again. If we're out of space immediately after
	// opening a slab, then every slab must be fully allocated.
	return allocate_slab_block(allocator->open_slab, block_number_ptr);
}

/**********************************************************************/
void release_vdo_block_reference(struct block_allocator *allocator,
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

	slab = get_vdo_slab(allocator->depot, pbn);
	result = modify_vdo_slab_reference_count(slab, NULL, operation);
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result,
				       "Failed to release reference to %s physical block %llu",
				       why,
				       (unsigned long long) pbn);
	}
}

/**
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
 **/
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
	return ((info1->slab_number < info2->slab_number) ? 1 : -1);
}

/**
 * Swap two slab_status structures. Implements heap_swapper.
 **/
static void swap_slab_statuses(void *item1, void *item2)
{
	struct slab_status *info1 = item1;
	struct slab_status *info2 = item2;
	struct slab_status temp = *info1;
	*info1 = *info2;
	*info2 = temp;
}

/**
 * Convert a generic vdo_completion to the block_allocator containing it.
 *
 * @param completion  The completion to convert
 *
 * @return The block allocator containing the completion
 **/
static struct block_allocator *
as_block_allocator(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type,
				   VDO_BLOCK_ALLOCATOR_COMPLETION);
	return container_of(completion, struct block_allocator, completion);
}

/**
 * Inform the allocator that a slab action has finished on some slab. This
 * callback is registered in apply_to_slabs().
 *
 * @param completion  The allocator completion
 **/
static void slab_action_callback(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);
	struct slab_actor *actor = &allocator->slab_actor;

	if (--actor->slab_action_count == 0) {
		actor->callback(completion);
		return;
	}

	reset_vdo_completion(completion);
}

/**
 * Preserve the error from part of an administrative action and continue.
 *
 * @param completion  The allocator completion
 **/
static void handle_operation_error(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	set_vdo_operation_result(&allocator->state, completion->result);
	completion->callback(completion);
}

/**
 * Perform an administrative action on each of an allocator's slabs in
 * parallel.
 *
 * @param allocator   The allocator
 * @param callback    The method to call when the action is complete on every
 *                    slab
 **/
static void apply_to_slabs(struct block_allocator *allocator,
			   vdo_action *callback)
{
	struct slab_iterator iterator;

	prepare_vdo_completion(&allocator->completion,
			       slab_action_callback,
			       handle_operation_error,
			       allocator->thread_id,
			       NULL);
	allocator->completion.requeue = false;

	// Since we are going to dequeue all of the slabs, the open slab will
	// become invalid, so clear it.
	allocator->open_slab = NULL;

	// Ensure that we don't finish before we're done starting.
	allocator->slab_actor = (struct slab_actor) {
		.slab_action_count = 1,
		.callback = callback,
	};

	iterator = get_slab_iterator(allocator);
	while (vdo_has_next_slab(&iterator)) {
		const struct admin_state_code *operation =
			get_vdo_admin_state_code(&allocator->state);
		struct vdo_slab *slab = vdo_next_slab(&iterator);

		list_del_init(&slab->allocq_entry);
		allocator->slab_actor.slab_action_count++;
		start_vdo_slab_action(slab, operation, &allocator->completion);
	}

	slab_action_callback(&allocator->completion);
}

/**
 * Inform the allocator that all load I/O has finished.
 *
 * @param completion  The allocator completion
 **/
static void finish_loading_allocator(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);
	const struct admin_state_code *operation =
		get_vdo_admin_state_code(&allocator->state);

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_RECOVERY) {
		void *context =
			get_current_vdo_action_context(allocator->depot->action_manager);
		vdo_replay_into_slab_journals(allocator, completion, context);
		return;
	}

	finish_vdo_loading(&allocator->state);
}

/**
 * Initiate a load.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_load(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	const struct admin_state_code *operation = get_vdo_admin_state_code(state);

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_REBUILD) {
		prepare_vdo_completion(&allocator->completion,
				       finish_loading_allocator,
				       handle_operation_error,
				       allocator->thread_id,
				       NULL);
		erase_vdo_slab_journals(allocator->depot,
					get_slab_iterator(allocator),
					&allocator->completion);
		return;
	}

	apply_to_slabs(allocator, finish_loading_allocator);
}

/**********************************************************************/
void load_vdo_block_allocator(void *context,
			      zone_count_t zone_number,
			      struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	start_vdo_loading(
		&allocator->state,
		get_current_vdo_manager_operation(allocator->depot->action_manager),
		parent,
		initiate_load);
}

/**********************************************************************/
void notify_vdo_slab_journals_are_recovered(struct block_allocator *allocator,
					    int result)
{
	finish_vdo_loading_with_result(&allocator->state, result);
}

/**
 * Prepare slabs for allocation or scrubbing.
 *
 * @param allocator  The allocator to prepare
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
prepare_vdo_slabs_for_allocation(struct block_allocator *allocator)
{
	struct slab_status current_slab_status;
	struct heap heap;
	int result;
	struct slab_status *slab_statuses;
	struct slab_depot *depot = allocator->depot;
	slab_count_t slab_count = depot->slab_count;

	WRITE_ONCE(allocator->allocated_blocks,
		   get_data_block_count(allocator));

	result = UDS_ALLOCATE(slab_count, struct slab_status, __func__,
			      &slab_statuses);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_get_summarized_slab_statuses(allocator->summary, slab_count,
					 slab_statuses);

	// Sort the slabs by cleanliness, then by emptiness hint.
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
			queue_vdo_slab(slab);
			continue;
		}

		mark_vdo_slab_unrecovered(slab);
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

/**********************************************************************/
void prepare_vdo_block_allocator_to_allocate(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	int result = prepare_vdo_slabs_for_allocation(allocator);

	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	scrub_high_priority_vdo_slabs(allocator->slab_scrubber,
				      is_priority_table_empty(allocator->prioritized_slabs),
				      parent,
				      finish_vdo_completion_parent_callback,
				      finish_vdo_completion_parent_callback);
}

/**********************************************************************/
void register_new_vdo_slabs_for_allocator(void *context,
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
			register_vdo_slab_with_allocator(allocator, slab);
		}
	}
	complete_vdo_completion(parent);
}

/**
 * Perform a step in draining the allocator. This method is its own callback.
 *
 * @param completion  The allocator's completion
 **/
static void do_drain_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	prepare_vdo_completion_for_requeue(&allocator->completion,
					   do_drain_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (++allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		stop_vdo_slab_scrubbing(allocator->slab_scrubber, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_drain_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		drain_vdo_slab_summary_zone(
			allocator->summary,
			get_vdo_admin_state_code(&allocator->state),
			completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_FINISHED:
		ASSERT_LOG_ONLY(!is_vio_pool_busy(allocator->vio_pool),
				"vio pool not busy");
		finish_vdo_draining_with_result(&allocator->state,
						completion->result);
		return;

	default:
		finish_vdo_draining_with_result(&allocator->state,
						UDS_BAD_STATE);
	}
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = VDO_DRAIN_ALLOCATOR_START;
	do_drain_step(&allocator->completion);
}

/**********************************************************************/
void drain_vdo_block_allocator(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	start_vdo_draining(
		&allocator->state,
		get_current_vdo_manager_operation(allocator->depot->action_manager),
		parent,
		initiate_drain);
}

/**
 * Perform a step in resuming a quiescent allocator. This method is its own
 * callback.
 *
 * @param completion  The allocator's completion
 **/
static void do_resume_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = as_block_allocator(completion);

	prepare_vdo_completion_for_requeue(&allocator->completion,
					   do_resume_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (--allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		resume_vdo_slab_summary_zone(allocator->summary, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_resume_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		resume_vdo_slab_scrubbing(allocator->slab_scrubber, completion);
		return;

	case VDO_DRAIN_ALLOCATOR_START:
		finish_vdo_resuming_with_result(&allocator->state,
						completion->result);
		return;

	default:
		finish_vdo_resuming_with_result(&allocator->state,
						UDS_BAD_STATE);
	}
}

/**
 * Initiate a resume.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_resume(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = VDO_DRAIN_ALLOCATOR_STEP_FINISHED;
	do_resume_step(&allocator->completion);
}

/**********************************************************************/
void resume_vdo_block_allocator(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	start_vdo_resuming(&allocator->state,
			   get_current_vdo_manager_operation(allocator->depot->action_manager),
			   parent,
			   initiate_resume);
}

/**********************************************************************/
void release_vdo_tail_block_locks(void *context,
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
	complete_vdo_completion(parent);
}

/**********************************************************************/
struct slab_summary_zone *
get_vdo_slab_summary_zone(const struct block_allocator *allocator)
{
	return allocator->summary;
}

/**********************************************************************/
int acquire_vdo_block_allocator_vio(struct block_allocator *allocator,
				    struct waiter *waiter)
{
	return acquire_vio_from_pool(allocator->vio_pool, waiter);
}

/**********************************************************************/
void return_vdo_block_allocator_vio(struct block_allocator *allocator,
				    struct vio_pool_entry *entry)
{
	return_vio_to_pool(allocator->vio_pool, entry);
}

/**********************************************************************/
void scrub_all_unrecovered_vdo_slabs_in_zone(void *context,
					     zone_count_t zone_number,
					     struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		vdo_get_block_allocator_for_zone(context, zone_number);
	scrub_vdo_slabs(allocator->slab_scrubber,
			allocator->depot,
			vdo_notify_zone_finished_scrubbing,
			noop_vdo_completion_callback);
	complete_vdo_completion(parent);
}

/**********************************************************************/
int enqueue_for_clean_vdo_slab(struct block_allocator *allocator,
			       struct waiter *waiter)
{
	return enqueue_clean_vdo_slab_waiter(allocator->slab_scrubber, waiter);
}

/**********************************************************************/
void increase_vdo_slab_scrubbing_priority(struct vdo_slab *slab)
{
	vdo_register_slab_for_scrubbing(slab->allocator->slab_scrubber, slab, true);
}


/**********************************************************************/
struct block_allocator_statistics
get_vdo_block_allocator_statistics(const struct block_allocator *allocator)
{
	const struct block_allocator_statistics *stats =
		&allocator->statistics;
	return (struct block_allocator_statistics) {
		.slab_count = allocator->slab_count,
		.slabs_opened = READ_ONCE(stats->slabs_opened),
		.slabs_reopened = READ_ONCE(stats->slabs_reopened),
	};
}

/**********************************************************************/
struct slab_journal_statistics
get_vdo_slab_journal_statistics(const struct block_allocator *allocator)
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

/**********************************************************************/
struct ref_counts_statistics
get_vdo_ref_counts_statistics(const struct block_allocator *allocator)
{
	const struct ref_counts_statistics *stats =
		&allocator->ref_counts_statistics;
	return (struct ref_counts_statistics) {
		.blocks_written = READ_ONCE(stats->blocks_written),
	};
}

/**********************************************************************/
void dump_vdo_block_allocator(const struct block_allocator *allocator)
{
	unsigned int pause_counter = 0;
	struct slab_iterator iterator = get_slab_iterator(allocator);

	uds_log_info("block_allocator zone %u", allocator->zone_number);
	while (vdo_has_next_slab(&iterator)) {
		dump_vdo_slab(vdo_next_slab(&iterator));

		// Wait for a while after each batch of 32 slabs dumped,
		// allowing the kernel log a chance to be flushed instead of
		// being overrun.
		if (pause_counter++ == 31) {
			pause_counter = 0;
			uds_pause_for_logger();
		}
	}

	dump_vdo_slab_scrubber(allocator->slab_scrubber);
}
