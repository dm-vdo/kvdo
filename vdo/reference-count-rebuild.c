// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "reference-count-rebuild.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "block-map-page.h"
#include "forest.h"
#include "constants.h"
#include "num-utils.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "thread-config.h"
#include "vdo.h"
#include "vdo-page-cache.h"

/*
 * A reference count rebuild completion.
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the rebuild frees them.
 */
struct rebuild_completion {
	/* completion header */
	struct vdo_completion completion;
	/* the completion for flushing the block map */
	struct vdo_completion sub_task_completion;
	/* the thread on which all block map operations must be done */
	thread_id_t logical_thread_id;
	/* the admin thread */
	thread_id_t admin_thread_id;
	/* the block map */
	struct block_map *block_map;
	/* the slab depot */
	struct slab_depot *depot;
	/* whether this recovery has been aborted */
	bool aborted;
	/* whether we are currently launching the initial round of requests */
	bool launching;
	/* The number of logical blocks observed used */
	block_count_t *logical_blocks_used;
	/* The number of block map data blocks */
	block_count_t *block_map_data_blocks;
	/* the next page to fetch */
	page_count_t page_to_fetch;
	/* the number of leaf pages in the block map */
	page_count_t leaf_pages;
	/* the last slot of the block map */
	struct block_map_slot last_slot;
	/* number of pending (non-ready) requests*/
	page_count_t outstanding;
	/* number of page completions */
	page_count_t page_count;
	/* array of requested, potentially ready page completions */
	struct vdo_page_completion page_completions[];
};

/**
 * as_rebuild_completion() - Convert a vdo_completion to a rebuild_completion.
 * @completion: The completion to convert.
 *
 * Return: The completion as a rebuild_completion.
 */
static inline struct rebuild_completion * __must_check
as_rebuild_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_REFERENCE_COUNT_REBUILD_COMPLETION);
	return container_of(completion, struct rebuild_completion, completion);
}

/**
 * finish_rebuild() - Free the rebuild_completion and notify the parent that
 *                    the block map rebuild is done.
 * @completion: The rebuild_completion.
 *
 * This callback is registered in make_rebuild_completion().
 */
static void finish_rebuild(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;

	UDS_FREE(UDS_FORGET(completion));
	vdo_finish_completion(parent, result);
}

/**
 * make_rebuild_completion() - Make a new rebuild completion.
 * @vdo: The vdo.
 * @logical_blocks_used: A pointer to hold the logical blocks used.
 * @block_map_data_blocks: A pointer to hold the number of block map data
 *                         blocks.
 * @parent: The parent of the rebuild completion.
 * @rebuild_ptr: The new block map rebuild completion.
 *
 * Return: A success or error code.
 */
static int make_rebuild_completion(struct vdo *vdo,
				   block_count_t *logical_blocks_used,
				   block_count_t *block_map_data_blocks,
				   struct vdo_completion *parent,
				   struct rebuild_completion **rebuild_ptr)
{
	page_count_t page_count =
		min(vdo->device_config->cache_size >> 1,
		    (page_count_t) MAXIMUM_SIMULTANEOUS_VDO_BLOCK_MAP_RESTORATION_READS);

	struct rebuild_completion *rebuild;
	int result = UDS_ALLOCATE_EXTENDED(struct rebuild_completion,
					   page_count,
					   struct vdo_page_completion,
					   __func__,
					   &rebuild);
	if (result != UDS_SUCCESS) {
		return result;
	}

	vdo_initialize_completion(&rebuild->completion, vdo,
				  VDO_REFERENCE_COUNT_REBUILD_COMPLETION);
	vdo_initialize_completion(&rebuild->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);

	rebuild->block_map = vdo->block_map;
	rebuild->depot = vdo->depot;
	rebuild->logical_blocks_used = logical_blocks_used;
	rebuild->block_map_data_blocks = block_map_data_blocks;
	rebuild->page_count = page_count;
	rebuild->leaf_pages =
		vdo_compute_block_map_page_count(vdo->block_map->entry_count);

	rebuild->logical_thread_id =
		vdo_get_logical_zone_thread(vdo->thread_config, 0);
	rebuild->admin_thread_id = vdo->thread_config->admin_thread;

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 rebuild->logical_thread_id),
			"%s must be called on logical thread %u (not %u)",
			__func__, rebuild->logical_thread_id,
			vdo_get_callback_thread_id());
	vdo_prepare_completion(&rebuild->completion, finish_rebuild,
			       finish_rebuild, rebuild->logical_thread_id,
			       parent);

	*rebuild_ptr = rebuild;
	return VDO_SUCCESS;
}

/**
 * flush_block_map_updates() - Flush the block map now that all the reference
 *                             counts are rebuilt.
 * @completion: The sub-task completion.
 *
 * This callback is registered in finish_if_done().
 */
static void flush_block_map_updates(struct vdo_completion *completion)
{
	uds_log_info("Flushing block map changes");
	vdo_prepare_completion_to_finish_parent(completion, completion->parent);
	vdo_drain_block_map(as_rebuild_completion(completion->parent)->block_map,
						  VDO_ADMIN_STATE_RECOVERING,
						  completion);
}

/**
 * finish_if_done() - Check whether the rebuild is done.
 * @rebuild: The rebuild completion.
 *
 * If it succeeded, continues by flushing the block map.
 *
 * Return: true if the rebuild is complete.
 */
static bool finish_if_done(struct rebuild_completion *rebuild)
{
	if (rebuild->launching || (rebuild->outstanding > 0)) {
		return false;
	}

	if (rebuild->aborted) {
		vdo_complete_completion(&rebuild->completion);
		return true;
	}

	if (rebuild->page_to_fetch < rebuild->leaf_pages) {
		return false;
	}

	vdo_prepare_completion(&rebuild->sub_task_completion,
			       flush_block_map_updates,
			       vdo_finish_completion_parent_callback,
			       rebuild->admin_thread_id,
			       &rebuild->completion);
	vdo_invoke_completion_callback(&rebuild->sub_task_completion);
	return true;
}

/**
 * abort_rebuild() - Record that there has been an error during the rebuild.
 * @rebuild: The rebuild completion.
 * @result: The error result to use, if one is not already saved.
 */
static void abort_rebuild(struct rebuild_completion *rebuild, int result)
{
	rebuild->aborted = true;
	vdo_set_completion_result(&rebuild->completion, result);
}

/**
 * handle_page_load_error() - Handle an error loading a page.
 * @completion: The vdo_page_completion.
 */
static void handle_page_load_error(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	rebuild->outstanding--;
	abort_rebuild(rebuild, completion->result);
	vdo_release_page_completion(completion);
	finish_if_done(rebuild);
}

/**
 * rebuild_reference_counts_from_page() - Rebuild reference counts from a
 *                                        block map page.
 * @rebuild: The rebuild completion.
 * @completion: The page completion holding the page.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int
rebuild_reference_counts_from_page(struct rebuild_completion *rebuild,
				   struct vdo_completion *completion)
{
	slot_number_t slot;
	struct block_map_page *page = vdo_dereference_writable_page(completion);
	int result = ASSERT(page != NULL, "page available");

	if (result != VDO_SUCCESS) {
		return result;
	}

	if (!vdo_is_block_map_page_initialized(page)) {
		return VDO_SUCCESS;
	}

	/*
	 * Remove any bogus entries which exist beyond the end of the logical
	 * space.
	 */
	if (vdo_get_block_map_page_pbn(page) == rebuild->last_slot.pbn) {
		slot_number_t slot;

		for (slot = rebuild->last_slot.slot;
		     slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
			struct data_location mapping =
				vdo_unpack_block_map_entry(&page->entries[slot]);
			if (vdo_is_mapped_location(&mapping)) {
				page->entries[slot] =
					vdo_pack_pbn(VDO_ZERO_BLOCK,
						     VDO_MAPPING_STATE_UNMAPPED);
				vdo_request_page_write(completion);
			}
		}
	}

	/* Inform the slab depot of all entries on this page. */
	for (slot = 0; slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
		struct vdo_slab *slab;
		int result;
		struct data_location mapping =
			vdo_unpack_block_map_entry(&page->entries[slot]);
		if (!vdo_is_valid_location(&mapping)) {
			/* This entry is invalid, so remove it from the page. */
			page->entries[slot] = vdo_pack_pbn(VDO_ZERO_BLOCK,
							   VDO_MAPPING_STATE_UNMAPPED);
			vdo_request_page_write(completion);
			continue;
		}

		if (!vdo_is_mapped_location(&mapping)) {
			continue;
		}

		(*rebuild->logical_blocks_used)++;
		if (mapping.pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		if (!vdo_is_physical_data_block(rebuild->depot, mapping.pbn)) {
			/*
			 * This is a nonsense mapping. Remove it from the map so
			 * we're at least consistent and mark the page dirty.
			 */
			page->entries[slot] = vdo_pack_pbn(VDO_ZERO_BLOCK,
							   VDO_MAPPING_STATE_UNMAPPED);
			vdo_request_page_write(completion);
			continue;
		}

		slab = vdo_get_slab(rebuild->depot, mapping.pbn);
		result = vdo_adjust_reference_count_for_rebuild(slab->reference_counts,
								mapping.pbn,
								VDO_JOURNAL_DATA_INCREMENT);
		if (result != VDO_SUCCESS) {
			uds_log_error_strerror(result,
					       "Could not adjust reference count for PBN %llu, slot %u mapped to PBN %llu",
					       (unsigned long long) vdo_get_block_map_page_pbn(page),
					       slot,
					       (unsigned long long) mapping.pbn);
			page->entries[slot] = vdo_pack_pbn(VDO_ZERO_BLOCK,
							   VDO_MAPPING_STATE_UNMAPPED);
			vdo_request_page_write(completion);
		}
	}
	return VDO_SUCCESS;
}

static void fetch_page(struct rebuild_completion *rebuild,
		       struct vdo_completion *completion);

/**
 * page_loaded() - Process a page which has just been loaded.
 * @completion: The vdo_page_completion for the fetched page.
 *
 * This callback is registered by fetch_page().
 */
static void page_loaded(struct vdo_completion *completion)
{
	int result;

	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	rebuild->outstanding--;

	result = rebuild_reference_counts_from_page(rebuild, completion);
	if (result != VDO_SUCCESS) {
		abort_rebuild(rebuild, result);
	}

	vdo_release_page_completion(completion);
	if (finish_if_done(rebuild)) {
		return;
	}

	/*
	 * Advance progress to the next page, and fetch the next page we
	 * haven't yet requested.
	 */
	fetch_page(rebuild, completion);
}

/**
 * fetch_page() - Fetch a page from the block map.
 * @rebuild: The rebuild_completion.
 * @completion: The page completion to use.
 */
static void fetch_page(struct rebuild_completion *rebuild,
		       struct vdo_completion *completion)
{
	while (rebuild->page_to_fetch < rebuild->leaf_pages) {
		physical_block_number_t pbn =
			vdo_find_block_map_page_pbn(rebuild->block_map,
						    rebuild->page_to_fetch++);
		if (pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		if (!vdo_is_physical_data_block(rebuild->depot, pbn)) {
			abort_rebuild(rebuild, VDO_BAD_MAPPING);
			if (finish_if_done(rebuild)) {
				return;
			}
			continue;
		}

		vdo_init_page_completion(((struct vdo_page_completion *) completion),
					 rebuild->block_map->zones[0].page_cache,
					 pbn, true,
					 &rebuild->completion, page_loaded,
					 handle_page_load_error);
		rebuild->outstanding++;
		vdo_get_page(completion);
		return;
	}
}

/**
 * rebuild_from_leaves() - Rebuild reference counts from the leaf block map
 *                         pages.
 * @completion: The sub-task completion.
 *
 * Rebuilds reference counts from the leaf block map pages now that reference
 * counts have been rebuilt from the interior tree pages (which have been
 * loaded in the process). This callback is registered in
 * vdo_rebuild_reference_counts().
 */
static void rebuild_from_leaves(struct vdo_completion *completion)
{
	page_count_t i;
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	*rebuild->logical_blocks_used = 0;

	/*
	 * The PBN calculation doesn't work until the tree pages have been
	 * loaded, so we can't set this value at the start of rebuild.
	 */
	rebuild->last_slot = (struct block_map_slot){
		.slot = rebuild->block_map->entry_count
			% VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		.pbn = vdo_find_block_map_page_pbn(rebuild->block_map,
						   rebuild->leaf_pages - 1),
	};

	/*
	 * Prevent any page from being processed until all pages have been
	 * launched.
	 */
	rebuild->launching = true;
	for (i = 0; i < rebuild->page_count; i++) {
		fetch_page(rebuild, &rebuild->page_completions[i].completion);
	}
	rebuild->launching = false;
	finish_if_done(rebuild);
}

/**
 * process_entry() - Process a single entry from the block map tree.
 * @pbn: A pbn which holds a block map tree page.
 * @completion: The parent completion of the traversal.
 *
 * Implements vdo_entry_callback.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int process_entry(physical_block_number_t pbn,
			struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	struct vdo_slab *slab;
	int result;

	if ((pbn == VDO_ZERO_BLOCK)
	    || !vdo_is_physical_data_block(rebuild->depot, pbn)) {
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "PBN %llu out of range",
					      (unsigned long long) pbn);
	}

	slab = vdo_get_slab(rebuild->depot, pbn);
	result = vdo_adjust_reference_count_for_rebuild(slab->reference_counts,
							pbn,
							VDO_JOURNAL_BLOCK_MAP_INCREMENT);
	if (result != VDO_SUCCESS) {
		return uds_log_error_strerror(result,
					      "Could not adjust reference count for block map tree PBN %llu",
					      (unsigned long long) pbn);
	}

	(*rebuild->block_map_data_blocks)++;
	return VDO_SUCCESS;
}

/**
 * vdo_rebuild_reference_counts() - Rebuild the reference counts from the
 *                                  block map (read-only rebuild).
 * @vdo: The vdo.
 * @parent: The completion to notify when the rebuild is complete
 * @logical_blocks_used: A pointer to hold the logical blocks used.
 * @block_map_data_blocks: A pointer to hold the number of block map
 *                         data blocks.
 */
void vdo_rebuild_reference_counts(struct vdo *vdo,
				  struct vdo_completion *parent,
				  block_count_t *logical_blocks_used,
				  block_count_t *block_map_data_blocks)
{
	struct rebuild_completion *rebuild;
	struct vdo_completion *completion;

	int result = make_rebuild_completion(vdo, logical_blocks_used,
					     block_map_data_blocks, parent,
					     &rebuild);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	/*
	 * Completion chaining from page cache hits can lead to stack overflow
	 * during the rebuild, so clear out the cache before this rebuild phase.
	 */
	result =
		vdo_invalidate_page_cache(rebuild->block_map->zones[0].page_cache);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	/* First traverse the block map trees. */
	*rebuild->block_map_data_blocks = 0;
	completion = &rebuild->sub_task_completion;
	vdo_prepare_completion(completion, rebuild_from_leaves,
			       vdo_finish_completion_parent_callback,
			       rebuild->logical_thread_id,
			       &rebuild->completion);
	vdo_traverse_forest(rebuild->block_map, process_entry, completion);
}
