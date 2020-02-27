/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockAllocator.c#48 $
 */

#include "blockAllocatorInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "heap.h"
#include "numUtils.h"
#include "priorityTable.h"
#include "readOnlyNotifier.h"
#include "refCounts.h"
#include "slab.h"
#include "slabDepotInternals.h"
#include "slabIterator.h"
#include "slabJournalEraser.h"
#include "slabJournalInternals.h"
#include "slabScrubber.h"
#include "slabSummary.h"
#include "vdoRecovery.h"
#include "vio.h"
#include "vioPool.h"

/**
 * Assert that a block allocator function was called from the correct thread.
 *
 * @param thread_id      The allocator's thread id
 * @param function_name  The name of the function
 **/
static inline void assert_on_allocator_thread(ThreadID thread_id,
					      const char *function_name)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() == thread_id),
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
static unsigned int calculateSlabPriority(struct vdo_slab *slab)
{
	BlockCount free_blocks = get_slab_free_block_count(slab);

	// Slabs that are completely full must be the only ones with the lowest
	// priority: zero.
	if (free_blocks == 0) {
		return 0;
	}

	/*
	 * Slabs that have never been opened (empty, newly initialized, never
	 * been written to) have lower priority than previously opened slabs
	 * that have a signficant number of free blocks. This ranking causes VDO
	 * to avoid writing physical blocks for the first time until there are
	 * very few free blocks that have been previously written to. That
	 * policy makes VDO a better client of any underlying storage that is
	 * thinly-provisioned [VDOSTORY-123].
	 */
	unsigned int unopened_slab_priority =
		slab->allocator->unopened_slab_priority;
	if (isSlabJournalBlank(slab->journal)) {
		return unopened_slab_priority;
	}

	/*
	 * For all other slabs, the priority is derived from the logarithm of
	 * the number of free blocks. Slabs with the same order of magnitude of
	 * free blocks have the same priority. With 2^23 blocks, the priority
	 * will range from 1 to 25. The reserved unopened_slab_priority divides
	 * the range and is skipped by the logarithmic mapping.
	 */
	unsigned int priority = (1 + logBaseTwo(free_blocks));
	return ((priority < unopened_slab_priority) ? priority : priority + 1);
}

/**
 * Add a slab to the priority queue of slabs available for allocation.
 *
 * @param slab  The slab to prioritize
 **/
static void prioritize_slab(struct vdo_slab *slab)
{
	ASSERT_LOG_ONLY(isRingEmpty(&slab->ringNode),
			"a slab must not already be on a ring when prioritizing");
	slab->priority = calculateSlabPriority(slab);
	priority_table_enqueue(slab->allocator->prioritized_slabs,
			       slab->priority,
			       &slab->ringNode);
}

/**********************************************************************/
void register_slab_with_allocator(struct block_allocator *allocator,
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
	return iterate_slabs(allocator->depot->slabs,
			     allocator->last_slab,
			     allocator->zone_number,
			     allocator->depot->zone_count);
}

/**
 * Notify a block allocator that the VDO has entered read-only mode.
 *
 * Implements ReadOnlyNotification.
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
	assert_on_allocator_thread(allocator->thread_id, __func__);
	struct slab_iterator iterator = get_slab_iterator(allocator);
	while (has_next_slab(&iterator)) {
		struct vdo_slab *slab = next_slab(&iterator);
		abortSlabJournalWaiters(slab->journal);
	}

	completeCompletion(parent);
}

/**********************************************************************/
int make_allocator_pool_vios(PhysicalLayer *layer,
			     void *parent,
			     void *buffer,
			     struct vio **vio_ptr)
{
	return createVIO(layer,
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
 * @param layer                 The physical layer below this allocator
 * @param vio_pool_size         The vio pool size
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_components(struct block_allocator *allocator,
			       PhysicalLayer *layer,
			       BlockCount vio_pool_size)
{
	/*
	 * If createVIO is NULL, the block allocator is only being used to
	 * format or audit the VDO. These only require the SuperBlock component,
	 * so we can just skip allocating all the memory needed for runtime
	 * components.
	 */
	if (layer->createMetadataVIO == NULL) {
		return VDO_SUCCESS;
	}

	int result =
		register_read_only_listener(allocator->read_only_notifier,
					    allocator,
					    notify_block_allocator_of_read_only_mode,
					    allocator->thread_id);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct slab_depot *depot = allocator->depot;
	result = initializeEnqueueableCompletion(&allocator->completion,
						 BLOCK_ALLOCATOR_COMPLETION,
						 layer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	allocator->summary =
		get_slab_summary_for_zone(depot, allocator->zone_number);

	result = makeVIOPool(layer,
			     vio_pool_size,
			     allocator->thread_id,
			     make_allocator_pool_vios,
			     NULL,
			     &allocator->vio_pool);
	if (result != VDO_SUCCESS) {
		return result;
	}

	BlockCount slab_journal_size = depot->slab_config.slabJournalBlocks;
	result = make_slab_scrubber(layer,
				    slab_journal_size,
				    allocator->read_only_notifier,
				    &allocator->slab_scrubber);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// The number of data blocks is the maximum number of free blocks that
	// could be used in calculateSlabPriority().
	BlockCount max_free_blocks = depot->slab_config.dataBlocks;
	unsigned int max_priority = (2 + logBaseTwo(max_free_blocks));
	result = make_priority_table(max_priority,
				     &allocator->prioritized_slabs);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * VDOSTORY-123 requires that we try to open slabs that already have
	 * allocated blocks in preference to slabs that have never been opened.
	 * For reasons we have not been able to fully understand, performance
	 * tests on SSD harvards have been very sensitive (50% reduction in test
	 * throughput) to very slight differences in the timing and locality of
	 * block allocation. Assigning a low priority to unopened slabs
	 * (max_priority/2, say) would be ideal for the story, but anything less
	 * than a very high threshold (max_priority - 1) hurts PMI results.
	 *
	 * This sets the free block threshold for preferring to open an unopened
	 * slab to the binary floor of 3/4ths the total number of datablocks in
	 * a slab, which will generally evaluate to about half the slab size,
	 * but avoids degenerate behavior in unit tests where the number of data
	 * blocks is artificially constrained to a power of two.
	 */
	allocator->unopened_slab_priority =
		(1 + logBaseTwo((max_free_blocks * 3) / 4));

	return VDO_SUCCESS;
}

/**********************************************************************/
int make_block_allocator(struct slab_depot *depot,
			 ZoneCount zone_number,
			 ThreadID thread_id,
			 Nonce nonce,
			 BlockCount vio_pool_size,
			 PhysicalLayer *layer,
			 struct read_only_notifier *read_only_notifier,
			 struct block_allocator **allocator_ptr)
{
	struct block_allocator *allocator;
	int result = ALLOCATE(1, struct block_allocator, __func__, &allocator);
	if (result != VDO_SUCCESS) {
		return result;
	}

	allocator->depot = depot;
	allocator->zone_number = zone_number;
	allocator->thread_id = thread_id;
	allocator->nonce = nonce;
	allocator->read_only_notifier = read_only_notifier;
	initializeRing(&allocator->dirty_slab_journals);

	result = allocate_components(allocator, layer, vio_pool_size);
	if (result != VDO_SUCCESS) {
		free_block_allocator(&allocator);
		return result;
	}

	*allocator_ptr = allocator;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_block_allocator(struct block_allocator **block_allocator_ptr)
{
	struct block_allocator *allocator = *block_allocator_ptr;
	if (allocator == NULL) {
		return;
	}

	free_slab_scrubber(&allocator->slab_scrubber);
	freeVIOPool(&allocator->vio_pool);
	free_priority_table(&allocator->prioritized_slabs);
	destroyEnqueueable(&allocator->completion);
	FREE(allocator);
	*block_allocator_ptr = NULL;
}

/**********************************************************************/
int replace_vio_pool(struct block_allocator *allocator,
		     size_t size,
		     PhysicalLayer *layer)
{
	freeVIOPool(&allocator->vio_pool);
	return makeVIOPool(layer,
			   size,
			   allocator->thread_id,
			   make_allocator_pool_vios,
			   NULL,
			   &allocator->vio_pool);
}

/**
 * Get the maximum number of data blocks that can be allocated.
 *
 * @param allocator  The block allocator to query
 *
 * @return The number of data blocks that can be allocated
 **/
__attribute__((warn_unused_result)) static inline BlockCount
get_data_block_count(const struct block_allocator *allocator)
{
	return (allocator->slab_count * allocator->depot->slab_config.dataBlocks);
}

/**********************************************************************/
BlockCount get_allocated_blocks(const struct block_allocator *allocator)
{
	return relaxedLoad64(&allocator->statistics.allocatedBlocks);
}

/**********************************************************************/
BlockCount get_unrecovered_slab_count(const struct block_allocator *allocator)
{
	return get_scrubber_slab_count(allocator->slab_scrubber);
}

/**********************************************************************/
void queue_slab(struct vdo_slab *slab)
{
	ASSERT_LOG_ONLY(isRingEmpty(&slab->ringNode),
			"a requeued slab must not already be on a ring");
	struct block_allocator *allocator = slab->allocator;
	BlockCount free_blocks = get_slab_free_block_count(slab);
	int result =
		ASSERT((free_blocks <= allocator->depot->slab_config.dataBlocks),
		       "rebuilt slab %u must have a valid free block count"
		       " (has %llu, expected maximum %llu)",
		       slab->slab_number,
		       free_blocks,
		       allocator->depot->slab_config.dataBlocks);
	if (result != VDO_SUCCESS) {
		enter_read_only_mode(allocator->read_only_notifier, result);
		return;
	}

	if (is_unrecovered_slab(slab)) {
		register_slab_for_scrubbing(allocator->slab_scrubber,
					    slab, false);
		return;
	}

	if (!is_slab_resuming(slab)) {
		// If the slab is resuming, we've already accounted for it here,
		// so don't do it again.
		relaxedAdd64(&allocator->statistics.allocatedBlocks,
			     -free_blocks);
		if (!isSlabJournalBlank(slab->journal)) {
			relaxedAdd64(&allocator->statistics.slabsOpened, 1);
		}
	}

	// All slabs are kept in a priority queue for allocation.
	prioritize_slab(slab);
}

/**********************************************************************/
void adjust_free_block_count(struct vdo_slab *slab, bool increment)
{
	struct block_allocator *allocator = slab->allocator;
	// The sense of increment is reversed since allocations are being
	// counted.
	relaxedAdd64(&allocator->statistics.allocatedBlocks,
		     (increment ? -1 : 1));

	// The open slab doesn't need to be reprioritized until it is closed.
	if (slab == allocator->open_slab) {
		return;
	}

	// The slab priority rarely changes; if no change, then don't requeue
	// it.
	if (slab->priority == calculateSlabPriority(slab)) {
		return;
	}

	// Reprioritize the slab to reflect the new free block count by removing
	// it from the table and re-enqueuing it with the new priority.
	priority_table_remove(allocator->prioritized_slabs, &slab->ringNode);
	prioritize_slab(slab);
}

/**
 * Allocate the next free physical block in a slab.
 *
 * The block allocated will have a provisional reference and the
 * reference must be either confirmed with a subsequent call to
 * incrementReferenceCount() or vacated with a subsequent call to
 * decrementReferenceCount().
 *
 * @param [in]  slab              The slab
 * @param [out] block_number_ptr  A pointer to receive the allocated block number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int allocate_slab_block(struct vdo_slab *slab,
			       PhysicalBlockNumber *block_number_ptr)
{
	PhysicalBlockNumber pbn;
	int result = allocate_unreferenced_block(slab->reference_counts, &pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	adjust_free_block_count(slab, false);

	*block_number_ptr = pbn;
	return VDO_SUCCESS;
}

/**********************************************************************/
int allocate_block(struct block_allocator *allocator,
		   PhysicalBlockNumber *block_number_ptr)
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
		slabFromRingNode(priority_table_dequeue(allocator->prioritized_slabs));

	if (isSlabJournalBlank(allocator->open_slab->journal)) {
		relaxedAdd64(&allocator->statistics.slabsOpened, 1);
		dirty_all_reference_blocks(allocator->open_slab->reference_counts);
	} else {
		relaxedAdd64(&allocator->statistics.slabsReopened, 1);
	}

	// Try allocating again. If we're out of space immediately after opening
	// a slab, then every slab must be fully allocated.
	return allocate_slab_block(allocator->open_slab, block_number_ptr);
}

/**********************************************************************/
void release_block_reference(struct block_allocator *allocator,
			     PhysicalBlockNumber pbn,
			     const char *why)
{
	if (pbn == ZERO_BLOCK) {
		return;
	}

	struct vdo_slab *slab = get_slab(allocator->depot, pbn);
	struct reference_operation operation = {
		.type = DATA_DECREMENT,
		.pbn  = pbn,
	};
	int result = modify_slab_reference_count(slab, NULL, operation);
	if (result != VDO_SUCCESS) {
		logErrorWithStringError(result,
					"Failed to release reference to %s "
					"physical block %llu",
					why,
					pbn);
	}
}

/**
 * This is a HeapComparator function that orders slab_status
 * atructures using the 'isClean' field as the primary key and the
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

	if (info1->isClean != info2->isClean) {
		return (info1->isClean ? 1 : -1);
	}
	if (info1->emptiness != info2->emptiness) {
		return ((info1->emptiness > info2->emptiness) ? 1 : -1);
	}
	return ((info1->slabNumber < info2->slabNumber) ? 1 : -1);
}

/**
 * Swap two slab_status structures. Implements HeapSwapper.
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
 * Inform the allocator that a slab action has finished on some slab. This
 * callback is registered in applyToSlabs().
 *
 * @param completion  The allocator completion
 **/
static void slab_action_callback(struct vdo_completion *completion)
{
	struct block_allocator *allocator =
		container_of(completion, struct block_allocator, completion);
	struct slab_actor *actor = &allocator->slab_actor;
	if (--actor->slab_action_count == 0) {
		actor->callback(completion);
		return;
	}

	resetCompletion(completion);
}

/**
 * Preserve the error from part of an administrative action and continue.
 *
 * @param completion  The allocator completion
 **/
static void handle_operation_error(struct vdo_completion *completion)
{
	struct block_allocator *allocator =
		(struct block_allocator *) completion;
	set_operation_result(&allocator->state, completion->result);
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
			   VDOAction *callback)
{
	prepareCompletion(&allocator->completion,
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

	struct slab_iterator iterator = get_slab_iterator(allocator);
	while (has_next_slab(&iterator)) {
		struct vdo_slab *slab = next_slab(&iterator);
		unspliceRingNode(&slab->ringNode);
		allocator->slab_actor.slab_action_count++;
		start_slab_action(slab,
				  allocator->state.state,
				  &allocator->completion);
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
	struct block_allocator *allocator =
		(struct block_allocator *) completion;
	if (allocator->state.state == ADMIN_STATE_LOADING_FOR_RECOVERY) {
		void *context =
			get_current_action_context(allocator->depot->action_manager);
		replayIntoSlabJournals(allocator, completion, context);
		return;
	}

	finish_loading(&allocator->state);
}

/**
 * Initiate a load.
 *
 * Implements AdminInitiator.
 **/
static void initiate_load(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	if (state->state == ADMIN_STATE_LOADING_FOR_REBUILD) {
		prepareCompletion(&allocator->completion,
				  finish_loading_allocator,
				  handle_operation_error,
				  allocator->thread_id,
				  NULL);
		eraseSlabJournals(allocator->depot,
				  get_slab_iterator(allocator),
				  &allocator->completion);
		return;
	}

	apply_to_slabs(allocator, finish_loading_allocator);
}

/**********************************************************************/
void load_block_allocator(void *context,
			  ZoneCount zone_number,
			  struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	start_loading(
		&allocator->state,
		get_current_manager_operation(allocator->depot->action_manager),
		parent,
		initiate_load);
}

/**********************************************************************/
void notify_slab_journals_are_recovered(struct block_allocator *allocator,
					int result)
{
	finish_loading_with_result(&allocator->state, result);
}

/**********************************************************************/
int prepare_slabs_for_allocation(struct block_allocator *allocator)
{
	relaxedStore64(&allocator->statistics.allocatedBlocks,
		       get_data_block_count(allocator));

	struct slab_depot *depot = allocator->depot;
	SlabCount slab_count = depot->slab_count;

	struct slab_status *slab_statuses;
	int result = ALLOCATE(slab_count,
			      struct slab_status,
			      __func__,
			      &slab_statuses);
	if (result != VDO_SUCCESS) {
		return result;
	}

	get_summarized_slab_statuses(allocator->summary, slab_count,
				     slab_statuses);

	// Sort the slabs by cleanliness, then by emptiness hint.
	struct heap heap;
	initialize_heap(&heap,
			compare_slab_statuses,
			swap_slab_statuses,
			slab_statuses,
			slab_count,
			sizeof(struct slab_status));
	build_heap(&heap, slab_count);

	struct slab_status current_slab_status;
	while (pop_max_heap_element(&heap, &current_slab_status)) {
		struct vdo_slab *slab =
			depot->slabs[current_slab_status.slabNumber];
		if (slab->allocator != allocator) {
			continue;
		}

		if ((depot->load_type == REBUILD_LOAD) ||
		    (!must_load_ref_counts(allocator->summary,
					   slab->slab_number) &&
		     current_slab_status.isClean)) {
			queue_slab(slab);
			continue;
		}

		mark_slab_unrecovered(slab);
		bool high_priority = ((current_slab_status.isClean &&
				      (depot->load_type == NORMAL_LOAD)) ||
				     requiresScrubbing(slab->journal));
		register_slab_for_scrubbing(allocator->slab_scrubber,
					    slab,
					    high_priority);
	}
	FREE(slab_statuses);

	return VDO_SUCCESS;
}

/**********************************************************************/
void prepare_allocator_to_allocate(void *context,
				   ZoneCount zone_number,
				   struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	int result = prepare_slabs_for_allocation(allocator);
	if (result != VDO_SUCCESS) {
		finishCompletion(parent, result);
		return;
	}

	scrub_high_priority_slabs(allocator->slab_scrubber,
				  is_priority_table_empty(allocator->prioritized_slabs),
				  parent,
				  finishParentCallback,
				  finishParentCallback);
}

/**********************************************************************/
void register_new_slabs_for_allocator(void *context,
				      ZoneCount zone_number,
				      struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	struct slab_depot *depot = allocator->depot;
	SlabCount i;
	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];
		if (slab->allocator == allocator) {
			register_slab_with_allocator(allocator, slab);
		}
	}
	completeCompletion(parent);
}

/**
 * Perform a step in draining the allocator. This method is its own callback.
 *
 * @param completion  The allocator's completion
 **/
static void do_drain_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator =
		(struct block_allocator *) completion;
	prepareForRequeue(&allocator->completion,
			  do_drain_step,
			  handle_operation_error,
			  allocator->thread_id,
			  NULL);
	switch (++allocator->drain_step) {
	case DRAIN_ALLOCATOR_STEP_SCRUBBER:
		stop_scrubbing(allocator->slab_scrubber, completion);
		return;

	case DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_drain_step);
		return;

	case DRAIN_ALLOCATOR_STEP_SUMMARY:
		drain_slab_summary_zone(allocator->summary,
					allocator->state.state, completion);
		return;

	case DRAIN_ALLOCATOR_STEP_FINISHED:
		ASSERT_LOG_ONLY(!isVIOPoolBusy(allocator->vio_pool),
				"vio pool not busy");
		finish_draining_with_result(&allocator->state,
					    completion->result);
		return;

	default:
		finish_draining_with_result(&allocator->state, UDS_BAD_STATE);
	}
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = DRAIN_ALLOCATOR_START;
	do_drain_step(&allocator->completion);
}

/**********************************************************************/
void drain_block_allocator(void *context,
			   ZoneCount zone_number,
			   struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	start_draining(
		&allocator->state,
		get_current_manager_operation(allocator->depot->action_manager),
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
	struct block_allocator *allocator =
		(struct block_allocator *) completion;
	prepareForRequeue(&allocator->completion,
			  do_resume_step,
			  handle_operation_error,
			  allocator->thread_id,
			  NULL);
	switch (--allocator->drain_step) {
	case DRAIN_ALLOCATOR_STEP_SUMMARY:
		resume_slab_summary_zone(allocator->summary, completion);
		return;

	case DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_resume_step);
		return;

	case DRAIN_ALLOCATOR_STEP_SCRUBBER:
		resume_scrubbing(allocator->slab_scrubber, completion);
		return;

	case DRAIN_ALLOCATOR_START:
		finish_resuming_with_result(&allocator->state,
					    completion->result);
		return;

	default:
		finish_resuming_with_result(&allocator->state, UDS_BAD_STATE);
	}
}

/**
 * Initiate a resume.
 *
 * Implements AdminInitiator.
 **/
static void initiate_resume(struct admin_state *state)
{
	struct block_allocator *allocator =
		container_of(state, struct block_allocator, state);
	allocator->drain_step = DRAIN_ALLOCATOR_STEP_FINISHED;
	do_resume_step(&allocator->completion);
}

/**********************************************************************/
void resume_block_allocator(void *context,
			    ZoneCount zone_number,
			    struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	start_resuming(&allocator->state,
		       get_current_manager_operation(allocator->depot->action_manager),
		       parent,
		       initiate_resume);
}

/**********************************************************************/
void release_tail_block_locks(void *context,
			      ZoneCount zone_number,
			      struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	RingNode *ring = &allocator->dirty_slab_journals;
	while (!isRingEmpty(ring)) {
		if (!releaseRecoveryJournalLock(slabJournalFromDirtyNode(ring->next),
						allocator->depot->active_release_request)) {
			break;
		}
	}
	completeCompletion(parent);
}

/**********************************************************************/
struct slab_summary_zone *
get_slab_summary_zone(const struct block_allocator *allocator)
{
	return allocator->summary;
}

/**********************************************************************/
int acquire_vio(struct block_allocator *allocator, struct waiter *waiter)
{
	return acquireVIOFromPool(allocator->vio_pool, waiter);
}

/**********************************************************************/
void return_vio(struct block_allocator *allocator, struct vio_pool_entry *entry)
{
	returnVIOToPool(allocator->vio_pool, entry);
}

/**********************************************************************/
void scrub_all_unrecovered_slabs_in_zone(void *context,
					 ZoneCount zone_number,
					 struct vdo_completion *parent)
{
	struct block_allocator *allocator =
		get_block_allocator_for_zone(context, zone_number);
	scrub_slabs(allocator->slab_scrubber,
		    allocator->depot,
		    notify_zone_finished_scrubbing,
		    noopCallback);
	completeCompletion(parent);
}

/**********************************************************************/
int enqueue_for_clean_slab(struct block_allocator *allocator,
			   struct waiter *waiter)
{
	return enqueue_clean_slab_waiter(allocator->slab_scrubber, waiter);
}

/**********************************************************************/
void increase_scrubbing_priority(struct vdo_slab *slab)
{
	register_slab_for_scrubbing(slab->allocator->slab_scrubber, slab, true);
}

/**********************************************************************/
void allocate_from_allocator_last_slab(struct block_allocator *allocator)
{
	ASSERT_LOG_ONLY(allocator->open_slab == NULL,
			"mustn't have an open slab");
	struct vdo_slab *last_slab =
		allocator->depot->slabs[allocator->last_slab];
	priority_table_remove(allocator->prioritized_slabs,
			      &last_slab->ringNode);
	allocator->open_slab = last_slab;
}

/**********************************************************************/
BlockAllocatorStatistics
get_block_allocator_statistics(const struct block_allocator *allocator)
{
	const struct atomic_allocator_statistics *atoms =
		&allocator->statistics;
	return (BlockAllocatorStatistics) {
		.slabCount = allocator->slab_count,
		.slabsOpened = relaxedLoad64(&atoms->slabsOpened),
		.slabsReopened = relaxedLoad64(&atoms->slabsReopened),
	};
}

/**********************************************************************/
SlabJournalStatistics
get_slab_journal_statistics(const struct block_allocator *allocator)
{
	const struct atomic_slab_journal_statistics *atoms =
		&allocator->slab_journal_statistics;
	return (SlabJournalStatistics) {
		.diskFullCount = atomicLoad64(&atoms->diskFullCount),
		.flushCount = atomicLoad64(&atoms->flushCount),
		.blockedCount = atomicLoad64(&atoms->blockedCount),
		.blocksWritten = atomicLoad64(&atoms->blocksWritten),
		.tailBusyCount = atomicLoad64(&atoms->tailBusyCount),
	};
}

/**********************************************************************/
RefCountsStatistics
get_ref_counts_statistics(const struct block_allocator *allocator)
{
	const struct atomic_ref_count_statistics *atoms =
		&allocator->ref_count_statistics;
	return (RefCountsStatistics) {
		.blocksWritten = atomicLoad64(&atoms->blocksWritten),
	};
}

/**********************************************************************/
void dump_block_allocator(const struct block_allocator *allocator)
{
	unsigned int pause_counter = 0;
	logInfo("block_allocator zone %u", allocator->zone_number);
	struct slab_iterator iterator = get_slab_iterator(allocator);
	while (has_next_slab(&iterator)) {
		dump_slab(next_slab(&iterator));

		// Wait for a while after each batch of 32 slabs dumped,
		// allowing the kernel log a chance to be flushed instead of
		// being overrun.
		if (pause_counter++ == 31) {
			pause_counter = 0;
			pauseForLogger();
		}
	}

	dump_slab_scrubber(allocator->slab_scrubber);
}
