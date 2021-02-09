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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapRecovery.c#38 $
 */

#include "blockMapRecovery.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "heap.h"
#include "numUtils.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

/**
 * A completion to manage recovering the block map from the recovery journal.
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the recovery frees them.
 **/
struct block_map_recovery_completion {
	/** completion header */
	struct vdo_completion completion;
	/** the completion for flushing the block map */
	struct vdo_completion sub_task_completion;
	/** the thread from which the block map may be flushed */
	thread_id_t admin_thread;
	/** the thread on which all block map operations must be done */
	thread_id_t logical_thread_id;
	/** the block map */
	struct block_map *block_map;
	/** whether this recovery has been aborted */
	bool aborted;
	/** whether we are currently launching the initial round of requests */
	bool launching;

	// Fields for the journal entries.
	/** the journal entries to apply */
	struct numbered_block_mapping *journal_entries;
	/**
	 * a heap wrapping journal_entries. It re-orders and sorts journal
	 * entries in ascending LBN order, then original journal order. This
	 * permits efficient iteration over the journal entries in order.
	 **/
	struct heap replay_heap;

	// Fields tracking progress through the journal entries.
	/** a pointer to the next journal entry to apply */
	struct numbered_block_mapping *current_entry;
	/** next entry for which the block map page has not been requested */
	struct numbered_block_mapping *current_unfetched_entry;

	// Fields tracking requested pages.
	/** the absolute PBN of the current page being processed */
	physical_block_number_t pbn;
	/** number of pending (non-ready) requests */
	page_count_t outstanding;
	/** number of page completions */
	page_count_t page_count;
	/** array of requested, potentially ready page completions */
	struct vdo_page_completion page_completions[];
};

/**
 * This is a heap_comparator function that orders numbered_block_mappings using
 * the 'block_map_slot' field as the primary key and the mapping 'number' field
 * as the secondary key. Using the mapping number preserves the journal order
 * of entries for the same slot, allowing us to sort by slot while still
 * ensuring we replay all entries with the same slot in the exact order as they
 * appeared in the journal.
 *
 * <p>The comparator order is reversed from the usual sense since the
 * heap structure is a max-heap, returning larger elements before
 * smaller ones, but we want to pop entries off the heap in ascending
 * LBN order.
 **/
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

/**
 * Swap two numbered_block_mapping structures. Implements heap_swapper.
 **/
static void swap_mappings(void *item1, void *item2)
{
	struct numbered_block_mapping *mapping1 = item1;
	struct numbered_block_mapping *mapping2 = item2;
	struct numbered_block_mapping temp = *mapping1;
	*mapping1 = *mapping2;
	*mapping2 = temp;
}

/**
 * Convert a vdo_completion to a block_map_recovery_completion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a block_map_recovery_completion
 **/
static inline struct block_map_recovery_completion * __must_check
as_block_map_recovery_completion(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, BLOCK_MAP_RECOVERY_COMPLETION);
	return container_of(completion,
			    struct block_map_recovery_completion,
			    completion);
}

/**
 * Free a block_map_recovery_completion and null out the reference to it.
 *
 * @param recovery_ptr  a pointer to the completion to free
 **/
static void
free_recovery_completion(struct block_map_recovery_completion **recovery_ptr)
{
	struct block_map_recovery_completion *recovery = *recovery_ptr;
	if (recovery == NULL) {
		return;
	}

	FREE(recovery);
	*recovery_ptr = NULL;
}

/**
 * Free the block_map_recovery_completion and notify the parent that the
 * block map recovery is done. This callback is registered in
 * make_recovery_completion().
 *
 * @param completion  The block_map_recovery_completion
 **/
static void finish_block_map_recovery(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion);
	free_recovery_completion(&recovery);
	finish_completion(parent, result);
}

/**
 * Make a new block map recovery completion.
 *
 * @param [in]  vdo              The vdo
 * @param [in]  entry_count      The number of journal entries
 * @param [in]  journal_entries  An array of journal entries to process
 * @param [in]  parent           The parent of the recovery completion
 * @param [out] recovery_ptr     The new block map recovery completion
 *
 * @return a success or error code
 **/
static int
make_recovery_completion(struct vdo *vdo,
			 block_count_t entry_count,
			 struct numbered_block_mapping *journal_entries,
			 struct vdo_completion *parent,
			 struct block_map_recovery_completion **recovery_ptr)
{
	const struct thread_config *thread_config = get_thread_config(vdo);
	struct block_map *block_map = get_block_map(vdo);
	page_count_t page_count =
		min(get_configured_cache_size(vdo) >> 1,
		    (page_count_t) MAXIMUM_SIMULTANEOUS_BLOCK_MAP_RESTORATION_READS);

	struct block_map_recovery_completion *recovery;
	int result = ALLOCATE_EXTENDED(struct block_map_recovery_completion,
				       page_count,
				       struct vdo_page_completion,
				       __func__,
				       &recovery);
	if (result != UDS_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&recovery->completion, vdo,
				  BLOCK_MAP_RECOVERY_COMPLETION);
	initialize_vdo_completion(&recovery->sub_task_completion, vdo,
				  SUB_TASK_COMPLETION);
	recovery->block_map = block_map;
	recovery->journal_entries = journal_entries;
	recovery->page_count = page_count;
	recovery->current_entry = &recovery->journal_entries[entry_count - 1];

	recovery->admin_thread = get_admin_thread(thread_config);
	recovery->logical_thread_id = get_logical_zone_thread(thread_config, 0);

	// Organize the journal entries into a binary heap so we can iterate
	// over them in sorted order incrementally, avoiding an expensive sort
	// call.
	initialize_heap(&recovery->replay_heap,
			compare_mappings,
			swap_mappings,
			journal_entries,
			entry_count,
			sizeof(struct numbered_block_mapping));
	build_heap(&recovery->replay_heap, entry_count);

	ASSERT_LOG_ONLY((get_callback_thread_id() ==
			 recovery->logical_thread_id),
			"%s must be called on logical thread %u (not %u)",
			__func__,
			recovery->logical_thread_id,
			get_callback_thread_id());
	prepare_completion(&recovery->completion,
			   finish_block_map_recovery,
			   finish_block_map_recovery,
			   recovery->logical_thread_id,
			   parent);

	// This message must be recognizable by VDOTest::RebuildBase.
	log_info("Replaying %zu recovery entries into block map",
		 recovery->replay_heap.count);

	*recovery_ptr = recovery;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void flush_block_map(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	log_info("Flushing block map changes");
	ASSERT_LOG_ONLY((completion->callback_thread_id ==
			 recovery->admin_thread),
			"flush_block_map() called on admin thread");

	prepare_to_finish_parent(completion, completion->parent);
	drain_block_map(recovery->block_map,
			ADMIN_STATE_RECOVERING,
			completion);
}

/**
 * Check whether the recovery is done. If so, finish it by either flushing the
 * block map (if the recovery was successful), or by cleaning up (if it
 * wasn't).
 *
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if the recovery or recovery is complete
 **/
static bool finish_if_done(struct block_map_recovery_completion *recovery)
{
	// Pages are still being launched or there is still work to do
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
				release_vdo_page_completion(&page_completion->completion);
			}
		}
		complete_completion(&recovery->completion);
	} else {
		launch_callback_with_parent(&recovery->sub_task_completion,
					    flush_block_map,
					    recovery->admin_thread,
					    &recovery->completion);
	}

	return true;
}

/**
 * Note that there has been an error during the recovery and finish it if there
 * is nothing else outstanding.
 *
 * @param recovery  The block_map_recovery_completion
 * @param result    The error result to use, if one is not already saved
 **/
static void abort_recovery(struct block_map_recovery_completion *recovery,
			   int result)
{
	recovery->aborted = true;
	set_completion_result(&recovery->completion, result);
	finish_if_done(recovery);
}

/**
 * Find the first journal entry after a given entry which is not on the same
 * block map page.
 *
 * @param recovery       the block_map_recovery_completion
 * @param current_entry  the entry to search from
 * @param needs_sort     Whether sorting is needed to proceed
 *
 * @return Pointer to the first later journal entry on a different block map
 *         page, or a pointer to just before the journal entries if no
 *         subsequent entry is on a different block map page.
 **/
static struct numbered_block_mapping *
find_entry_starting_next_page(struct block_map_recovery_completion *recovery,
			      struct numbered_block_mapping *current_entry,
			      bool needs_sort)
{
	size_t current_page;
	// If current_entry is invalid, return immediately.
	if (current_entry < recovery->journal_entries) {
		return current_entry;
	}
	current_page = current_entry->block_map_slot.pbn;

	// Decrement current_entry until it's out of bounds or on a different
	// page.
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

/**
 * Apply a range of journal entries to a block map page.
 *
 * @param page            The block map page being modified
 * @param starting_entry  The first journal entry to apply
 * @param ending_entry    The entry just past the last journal entry to apply
 **/
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

/**********************************************************************/
static void recover_ready_pages(struct block_map_recovery_completion *recovery,
				struct vdo_completion *completion);

/**
 * Note that a page is now ready and attempt to process pages. This callback is
 * registered in fetch_page().
 *
 * @param completion  The vdo_page_completion for the fetched page
 **/
static void page_loaded(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	recovery->outstanding--;
	if (!recovery->launching) {
		recover_ready_pages(recovery, completion);
	}
}

/**
 * Handle an error loading a page.
 *
 * @param completion  The vdo_page_completion
 **/
static void handle_page_load_error(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);
	recovery->outstanding--;
	abort_recovery(recovery, completion->result);
}

/**
 * Fetch a page from the block map.
 *
 * @param recovery    the block_map_recovery_completion
 * @param completion  the page completion to use
 **/
static void fetch_page(struct block_map_recovery_completion *recovery,
		       struct vdo_completion *completion)
{
	physical_block_number_t new_pbn;
	if (recovery->current_unfetched_entry < recovery->journal_entries) {
		// Nothing left to fetch.
		return;
	}

	// Fetch the next page we haven't yet requested.
	new_pbn = recovery->current_unfetched_entry->block_map_slot.pbn;
	recovery->current_unfetched_entry =
		find_entry_starting_next_page(recovery,
					      recovery->current_unfetched_entry,
					      true);
	init_vdo_page_completion(((struct vdo_page_completion *) completion),
				 recovery->block_map->zones[0].page_cache,
				 new_pbn,
				 true,
				 &recovery->completion,
				 page_loaded,
				 handle_page_load_error);
	recovery->outstanding++;
	get_vdo_page(completion);
}

/**
 * Get the next page completion to process. If it isn't ready, we'll try again
 * when it is.
 *
 * @param recovery    The recovery completion
 * @param completion  The current page completion
 *
 * @return The next page completion to process
 **/
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

/**
 * Recover from as many pages as possible.
 *
 * @param recovery    The recovery completion
 * @param completion  The first page completion to process
 **/
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
			dereference_writable_vdo_page(completion);
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
		request_vdo_page_write(completion);
		release_vdo_page_completion(completion);

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

/**********************************************************************/
void recover_block_map(struct vdo *vdo,
		       block_count_t entry_count,
		       struct numbered_block_mapping *journal_entries,
		       struct vdo_completion *parent)
{
	struct numbered_block_mapping *first_sorted_entry;
	page_count_t i;
	struct block_map_recovery_completion *recovery;

	int result = make_recovery_completion(vdo, entry_count,
					      journal_entries, parent,
					      &recovery);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	if (is_heap_empty(&recovery->replay_heap)) {
		finish_completion(&recovery->completion, VDO_SUCCESS);
		return;
	}

	first_sorted_entry = sort_next_heap_element(&recovery->replay_heap);
	ASSERT_LOG_ONLY(first_sorted_entry == recovery->current_entry,
			"heap is returning elements in an unexpected order");

	// Prevent any page from being processed until all pages have been
	// launched.
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

	// Process any ready pages.
	recover_ready_pages(recovery, &recovery->page_completions[0].completion);
}
