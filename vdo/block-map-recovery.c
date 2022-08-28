// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "block-map-recovery.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "block-map-page.h"
#include "heap.h"
#include "num-utils.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-page-cache.h"

/*
 * A structure to manage recovering the block map from the recovery journal.
 *
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the recovery frees them.
 **/
struct block_map_recovery_completion {
	struct vdo_completion completion;
	/* for flushing the block map */
	struct vdo_completion sub_task_completion;
	/* the thread from which the block map may be flushed */
	thread_id_t admin_thread;
	/* the thread on which all block map operations must be done */
	thread_id_t logical_thread_id;
	struct block_map *block_map;
	/* whether this recovery has been aborted */
	bool aborted;
	bool launching;

	/* Fields for the journal entries. */
	struct numbered_block_mapping *journal_entries;
	/*
	 * a heap wrapping journal_entries. It re-orders and sorts journal
	 * entries in ascending LBN order, then original journal order. This
	 * permits efficient iteration over the journal entries in order.
	 */
	struct heap replay_heap;

	/* Fields tracking progress through the journal entries. */

	struct numbered_block_mapping *current_entry;
	/* next entry for which the block map page has not been requested */
	struct numbered_block_mapping *current_unfetched_entry;

	/* Fields tracking requested pages. */
	/* current page's absolute PBN */
	physical_block_number_t pbn;
	page_count_t outstanding;
	page_count_t page_count;
	struct vdo_page_completion page_completions[];
};

/*
 * This is a heap_comparator function that orders numbered_block_mappings using
 * the 'block_map_slot' field as the primary key and the mapping 'number' field
 * as the secondary key. Using the mapping number preserves the journal order
 * of entries for the same slot, allowing us to sort by slot while still
 * ensuring we replay all entries with the same slot in the exact order as they
 * appeared in the journal.
 *
 * The comparator order is reversed from the usual sense since the
 * heap structure is a max-heap, returning larger elements before
 * smaller ones, but we want to pop entries off the heap in ascending
 * LBN order.
 */
static int compare_mappings(const void *item1, const void *item2)
{
	const struct numbered_block_mapping *mapping1 =
		(const struct numbered_block_mapping *) item1;
	const struct numbered_block_mapping *mapping2 =
		(const struct numbered_block_mapping *) item2;

	if (mapping1->block_map_slot.pbn != mapping2->block_map_slot.pbn) {
		return ((mapping1->block_map_slot.pbn <
			 mapping2->block_map_slot.pbn) ? 1 : -1);
	}

	if (mapping1->block_map_slot.slot != mapping2->block_map_slot.slot) {
		return ((mapping1->block_map_slot.slot <
			 mapping2->block_map_slot.slot) ? 1 : -1);
	}

	if (mapping1->number != mapping2->number) {
		return ((mapping1->number < mapping2->number) ? 1 : -1);
	}

	return 0;
}

/*
 * Implements heap_swapper.
 */
static void swap_mappings(void *item1, void *item2)
{
	struct numbered_block_mapping *mapping1 = item1;
	struct numbered_block_mapping *mapping2 = item2;
	struct numbered_block_mapping temp = *mapping1;
	*mapping1 = *mapping2;
	*mapping2 = temp;
}

static inline struct block_map_recovery_completion * __must_check
as_block_map_recovery_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_BLOCK_MAP_RECOVERY_COMPLETION);
	return container_of(completion,
			    struct block_map_recovery_completion,
			    completion);
}

static void finish_block_map_recovery(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;

	UDS_FREE(as_block_map_recovery_completion(UDS_FORGET(completion)));
	vdo_finish_completion(parent, result);
}

static int
vdo_make_recovery_completion(struct vdo *vdo,
			     block_count_t entry_count,
			     struct numbered_block_mapping *journal_entries,
			     struct vdo_completion *parent,
			     struct block_map_recovery_completion **recovery_ptr)
{
	page_count_t page_count =
		min(vdo->device_config->cache_size >> 1,
		    (page_count_t) MAXIMUM_SIMULTANEOUS_VDO_BLOCK_MAP_RESTORATION_READS);

	struct block_map_recovery_completion *recovery;
	int result = UDS_ALLOCATE_EXTENDED(struct block_map_recovery_completion,
					   page_count,
					   struct vdo_page_completion,
					   __func__,
					   &recovery);
	if (result != UDS_SUCCESS) {
		return result;
	}

	vdo_initialize_completion(&recovery->completion, vdo,
				  VDO_BLOCK_MAP_RECOVERY_COMPLETION);
	vdo_initialize_completion(&recovery->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);
	recovery->block_map = vdo->block_map;
	recovery->journal_entries = journal_entries;
	recovery->page_count = page_count;
	recovery->current_entry = &recovery->journal_entries[entry_count - 1];

	recovery->admin_thread = vdo->thread_config->admin_thread;
	recovery->logical_thread_id =
		vdo_get_logical_zone_thread(vdo->thread_config, 0);

	/*
	 * Organize the journal entries into a binary heap so we can iterate
	 * over them in sorted order incrementally, avoiding an expensive sort
	 * call.
	 */
	initialize_heap(&recovery->replay_heap,
			compare_mappings,
			swap_mappings,
			journal_entries,
			entry_count,
			sizeof(struct numbered_block_mapping));
	build_heap(&recovery->replay_heap, entry_count);

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 recovery->logical_thread_id),
			"%s must be called on logical thread %u (not %u)",
			__func__,
			recovery->logical_thread_id,
			vdo_get_callback_thread_id());
	vdo_prepare_completion(&recovery->completion,
			       finish_block_map_recovery,
			       finish_block_map_recovery,
			       recovery->logical_thread_id,
			       parent);

	uds_log_info("Replaying %zu recovery entries into block map",
		     recovery->replay_heap.count);

	*recovery_ptr = recovery;
	return VDO_SUCCESS;
}

static void flush_block_map(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	uds_log_info("Flushing block map changes");
	ASSERT_LOG_ONLY((completion->callback_thread_id ==
			 recovery->admin_thread),
			"flush_block_map() called on admin thread");

	vdo_prepare_completion_to_finish_parent(completion, completion->parent);
	vdo_drain_block_map(recovery->block_map,
			    VDO_ADMIN_STATE_RECOVERING,
			    completion);
}

/* @return true if recovery is done.  */
static bool finish_if_done(struct block_map_recovery_completion *recovery)
{
	/* Pages are still being launched or there is still work to do */
	if (recovery->launching || (recovery->outstanding > 0) ||
	    (!recovery->aborted &&
	     (recovery->current_entry >= recovery->journal_entries))) {
		return false;
	}

	if (recovery->aborted) {
		/*
		 * We need to be careful here to only free completions that
		 * exist. But since we know none are outstanding, we just go
		 * through the ready ones.
		 */
		size_t i;

		for (i = 0; i < recovery->page_count; i++) {
			struct vdo_page_completion *page_completion =
				&recovery->page_completions[i];
			if (recovery->page_completions[i].ready) {
				vdo_release_page_completion(&page_completion->completion);
			}
		}
		vdo_complete_completion(&recovery->completion);
	} else {
		vdo_launch_completion_callback_with_parent(&recovery->sub_task_completion,
							   flush_block_map,
							   recovery->admin_thread,
							   &recovery->completion);
	}

	return true;
}

static void abort_recovery(struct block_map_recovery_completion *recovery,
			   int result)
{
	recovery->aborted = true;
	vdo_set_completion_result(&recovery->completion, result);
	finish_if_done(recovery);
}

/*
 * Find the first journal entry after a given entry which is not on the same
 * block map page.
 *
 * @current_entry: the entry to search from
 * @needs_sort: Whether sorting is needed to proceed
 *
 * @return Pointer to the first later journal entry on a different block map
 *         page, or a pointer to just before the journal entries if no
 *         subsequent entry is on a different block map page.
 */
static struct numbered_block_mapping *
find_entry_starting_next_page(struct block_map_recovery_completion *recovery,
			      struct numbered_block_mapping *current_entry,
			      bool needs_sort)
{
	size_t current_page;
	/* If current_entry is invalid, return immediately. */
	if (current_entry < recovery->journal_entries) {
		return current_entry;
	}
	current_page = current_entry->block_map_slot.pbn;

	/*
	 * Decrement current_entry until it's out of bounds or on a different
	 * page.
	 */
	while ((current_entry >= recovery->journal_entries) &&
	       (current_entry->block_map_slot.pbn == current_page)) {
		if (needs_sort) {
			struct numbered_block_mapping *just_sorted_entry =
				sort_next_heap_element(&recovery->replay_heap);
			ASSERT_LOG_ONLY(just_sorted_entry < current_entry,
					"heap is returning elements in an unexpected order");
		}
		current_entry--;
	}
	return current_entry;
}

/*
 * Apply a range of journal entries [starting_entry, ending_entry) journal
 * entries to a block map page.
 */
static void
apply_journal_entries_to_page(struct block_map_page *page,
			      struct numbered_block_mapping *starting_entry,
			      struct numbered_block_mapping *ending_entry)
{
	struct numbered_block_mapping *current_entry = starting_entry;

	while (current_entry != ending_entry) {
		page->entries[current_entry->block_map_slot.slot] =
			current_entry->block_map_entry;
		current_entry--;
	}
}

static void recover_ready_pages(struct block_map_recovery_completion *recovery,
				struct vdo_completion *completion);

static void page_loaded(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	recovery->outstanding--;
	if (!recovery->launching) {
		recover_ready_pages(recovery, completion);
	}
}

static void handle_page_load_error(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	recovery->outstanding--;
	abort_recovery(recovery, completion->result);
}

static void fetch_page(struct block_map_recovery_completion *recovery,
		       struct vdo_completion *completion)
{
	physical_block_number_t new_pbn;

	if (recovery->current_unfetched_entry < recovery->journal_entries) {
		/* Nothing left to fetch. */
		return;
	}

	/* Fetch the next page we haven't yet requested. */
	new_pbn = recovery->current_unfetched_entry->block_map_slot.pbn;
	recovery->current_unfetched_entry =
		find_entry_starting_next_page(recovery,
					      recovery->current_unfetched_entry,
					      true);
	vdo_init_page_completion(((struct vdo_page_completion *) completion),
				 recovery->block_map->zones[0].page_cache,
				 new_pbn,
				 true,
				 &recovery->completion,
				 page_loaded,
				 handle_page_load_error);
	recovery->outstanding++;
	vdo_get_page(completion);
}

static struct vdo_page_completion *
get_next_page_completion(struct block_map_recovery_completion *recovery,
			 struct vdo_page_completion *completion)
{
	completion++;
	if (completion == (&recovery->page_completions[recovery->page_count])) {
		completion = &recovery->page_completions[0];
	}
	return completion;
}

static void recover_ready_pages(struct block_map_recovery_completion *recovery,
				struct vdo_completion *completion)
{
	struct vdo_page_completion *page_completion =
		(struct vdo_page_completion *) completion;

	if (finish_if_done(recovery)) {
		return;
	}

	if (recovery->pbn != page_completion->pbn) {
		return;
	}

	while (page_completion->ready) {
		struct numbered_block_mapping *start_of_next_page;
		struct block_map_page *page =
			vdo_dereference_writable_page(completion);
		int result = ASSERT(page != NULL, "page available");

		if (result != VDO_SUCCESS) {
			abort_recovery(recovery, result);
			return;
		}

		start_of_next_page =
			find_entry_starting_next_page(recovery,
						      recovery->current_entry,
						      false);
		apply_journal_entries_to_page(page,
					      recovery->current_entry,
					      start_of_next_page);
		recovery->current_entry = start_of_next_page;
		vdo_request_page_write(completion);
		vdo_release_page_completion(completion);

		if (finish_if_done(recovery)) {
			return;
		}

		recovery->pbn = recovery->current_entry->block_map_slot.pbn;
		fetch_page(recovery, completion);
		page_completion =
			get_next_page_completion(recovery, page_completion);
		completion = &page_completion->completion;
	}
}

/*
 * Recover the block map (normal rebuild).
 */
void vdo_recover_block_map(struct vdo *vdo,
			   block_count_t entry_count,
			   struct numbered_block_mapping *journal_entries,
			   struct vdo_completion *parent)
{
	struct numbered_block_mapping *first_sorted_entry;
	page_count_t i;
	struct block_map_recovery_completion *recovery;

	int result = vdo_make_recovery_completion(vdo, entry_count,
						  journal_entries, parent,
						  &recovery);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	if (is_heap_empty(&recovery->replay_heap)) {
		vdo_finish_completion(&recovery->completion, VDO_SUCCESS);
		return;
	}

	first_sorted_entry = sort_next_heap_element(&recovery->replay_heap);
	ASSERT_LOG_ONLY(first_sorted_entry == recovery->current_entry,
			"heap is returning elements in an unexpected order");

	/*
	 * Prevent any page from being processed until all pages have been
	 * launched.
	 */
	recovery->launching = true;
	recovery->pbn = recovery->current_entry->block_map_slot.pbn;
	recovery->current_unfetched_entry = recovery->current_entry;
	for (i = 0; i < recovery->page_count; i++) {
		if (recovery->current_unfetched_entry <
		    recovery->journal_entries) {
			break;
		}

		fetch_page(recovery, &recovery->page_completions[i].completion);
	}
	recovery->launching = false;

	/* Process any ready pages. */
	recover_ready_pages(recovery, &recovery->page_completions[0].completion);
}
