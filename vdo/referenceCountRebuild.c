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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/referenceCountRebuild.c#52 $
 */

#include "referenceCountRebuild.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "forest.h"
#include "constants.h"
#include "numUtils.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

/**
 * A reference count rebuild completion.
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the rebuild frees them.
 **/
struct rebuild_completion {
	/** completion header */
	struct vdo_completion completion;
	/** the completion for flushing the block map */
	struct vdo_completion sub_task_completion;
	/** the thread on which all block map operations must be done */
	thread_id_t logical_thread_id;
	/** the admin thread */
	thread_id_t admin_thread_id;
	/** the block map */
	struct block_map *block_map;
	/** the slab depot */
	struct slab_depot *depot;
	/** whether this recovery has been aborted */
	bool aborted;
	/** whether we are currently launching the initial round of requests */
	bool launching;
	/** The number of logical blocks observed used */
	block_count_t *logical_blocks_used;
	/** The number of block map data blocks */
	block_count_t *block_map_data_blocks;
	/** the next page to fetch */
	page_count_t page_to_fetch;
	/** the number of leaf pages in the block map */
	page_count_t leaf_pages;
	/** the last slot of the block map */
	struct block_map_slot last_slot;
	/** number of pending (non-ready) requests*/
	page_count_t outstanding;
	/** number of page completions */
	page_count_t page_count;
	/** array of requested, potentially ready page completions */
	struct vdo_page_completion page_completions[];
};

/**
 * Convert a vdo_completion to a rebuild_completion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a rebuild_completion
 **/
static inline struct rebuild_completion * __must_check
as_rebuild_completion(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type,
				   REFERENCE_COUNT_REBUILD_COMPLETION);
	return container_of(completion, struct rebuild_completion, completion);
}

/**
 * Free the rebuild_completion and notify the parent that the block map
 * rebuild is done. This callback is registered in make_rebuild_completion().
 *
 * @param completion  The rebuild_completion
 **/
static void finish_rebuild(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;
	FREE(FORGET(completion));
	finish_vdo_completion(parent, result);
}

/**
 * Make a new rebuild completion.
 *
 * @param [in]  vdo                    The vdo
 * @param [in]  logical_blocks_used    A pointer to hold the logical blocks used
 * @param [in]  block_map_data_blocks  A pointer to hold the number of block map
 *                                     data blocks
 * @param [in]  parent                 The parent of the rebuild completion
 * @param [out] rebuild_ptr            The new block map rebuild completion
 *
 * @return a success or error code
 **/
static int make_rebuild_completion(struct vdo *vdo,
				   block_count_t *logical_blocks_used,
				   block_count_t *block_map_data_blocks,
				   struct vdo_completion *parent,
				   struct rebuild_completion **rebuild_ptr)
{
	const struct thread_config *thread_config = get_thread_config(vdo);
	struct block_map *block_map = get_block_map(vdo);
	page_count_t page_count =
		min(get_configured_cache_size(vdo) >> 1,
		    (page_count_t) MAXIMUM_SIMULTANEOUS_VDO_BLOCK_MAP_RESTORATION_READS);

	struct rebuild_completion *rebuild;
	int result = ALLOCATE_EXTENDED(struct rebuild_completion, page_count,
				       struct vdo_page_completion, __func__,
				       &rebuild);
	if (result != UDS_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&rebuild->completion, vdo,
				  REFERENCE_COUNT_REBUILD_COMPLETION);
	initialize_vdo_completion(&rebuild->sub_task_completion, vdo,
				  SUB_TASK_COMPLETION);
	if (result != VDO_SUCCESS) {
		FREE(FORGET(rebuild));
		return result;
	}

	rebuild->block_map = block_map;
	rebuild->depot = vdo->depot;
	rebuild->logical_blocks_used = logical_blocks_used;
	rebuild->block_map_data_blocks = block_map_data_blocks;
	rebuild->page_count = page_count;
	rebuild->leaf_pages =
		compute_block_map_page_count(block_map->entry_count);

	rebuild->logical_thread_id = get_logical_zone_thread(thread_config, 0);
	rebuild->admin_thread_id = get_admin_thread(thread_config);

	ASSERT_LOG_ONLY((get_callback_thread_id() ==
			 rebuild->logical_thread_id),
			"%s must be called on logical thread %u (not %u)",
			__func__, rebuild->logical_thread_id,
			get_callback_thread_id());
	prepare_vdo_completion(&rebuild->completion, finish_rebuild,
			       finish_rebuild, rebuild->logical_thread_id,
			       parent);

	*rebuild_ptr = rebuild;
	return VDO_SUCCESS;
}

/**
 * Flush the block map now that all the reference counts are rebuilt. This
 * callback is registered in finish_if_done().
 *
 * @param completion  The sub-task completion
 **/
static void flush_block_map_updates(struct vdo_completion *completion)
{
	log_info("Flushing block map changes");
	prepare_vdo_completion_to_finish_parent(completion, completion->parent);
	drain_block_map(as_rebuild_completion(completion->parent)->block_map,
			ADMIN_STATE_RECOVERING, completion);
}

/**
 * Check whether the rebuild is done. If it succeeded, continue by flushing the
 * block map.
 *
 * @param rebuild  The rebuild completion
 *
 * @return <code>true</code> if the rebuild is complete
 **/
static bool finish_if_done(struct rebuild_completion *rebuild)
{
	if (rebuild->launching || (rebuild->outstanding > 0)) {
		return false;
	}

	if (rebuild->aborted) {
		complete_vdo_completion(&rebuild->completion);
		return true;
	}

	if (rebuild->page_to_fetch < rebuild->leaf_pages) {
		return false;
	}

	prepare_vdo_completion(&rebuild->sub_task_completion,
			       flush_block_map_updates,
			       finish_vdo_completion_parent_callback,
			       rebuild->admin_thread_id,
			       &rebuild->completion);
	invoke_vdo_completion_callback(&rebuild->sub_task_completion);
	return true;
}

/**
 * Record that there has been an error during the rebuild.
 *
 * @param rebuild  The rebuild completion
 * @param result   The error result to use, if one is not already saved
 **/
static void abort_rebuild(struct rebuild_completion *rebuild, int result)
{
	rebuild->aborted = true;
	set_vdo_completion_result(&rebuild->completion, result);
}

/**
 * Handle an error loading a page.
 *
 * @param completion  The vdo_page_completion
 **/
static void handle_page_load_error(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	rebuild->outstanding--;
	abort_rebuild(rebuild, completion->result);
	release_vdo_page_completion(completion);
	finish_if_done(rebuild);
}

/**
 * Rebuild reference counts from a block map page.
 *
 * @param rebuild     The rebuild completion
 * @param completion  The page completion holding the page
 *
 * @return VDO_SUCCESS or an error
 **/
static int
rebuild_reference_counts_from_page(struct rebuild_completion *rebuild,
				   struct vdo_completion *completion)
{
	slot_number_t slot;
	struct block_map_page *page = dereference_writable_vdo_page(completion);
	int result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (!is_block_map_page_initialized(page)) {
		return VDO_SUCCESS;
	}

	// Remove any bogus entries which exist beyond the end of the logical
	// space.
	if (get_block_map_page_pbn(page) == rebuild->last_slot.pbn) {
		slot_number_t slot;
		for (slot = rebuild->last_slot.slot;
		     slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
			struct data_location mapping =
				unpack_block_map_entry(&page->entries[slot]);
			if (is_mapped_location(&mapping)) {
				page->entries[slot] =
					pack_pbn(VDO_ZERO_BLOCK,
						 MAPPING_STATE_UNMAPPED);
				request_vdo_page_write(completion);
			}
		}
	}

	// Inform the slab depot of all entries on this page.
	for (slot = 0; slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
		struct vdo_slab *slab;
		int result;
		struct data_location mapping =
			unpack_block_map_entry(&page->entries[slot]);
		if (!is_valid_location(&mapping)) {
			// This entry is invalid, so remove it from the page.
			page->entries[slot] = pack_pbn(VDO_ZERO_BLOCK,
						       MAPPING_STATE_UNMAPPED);
			request_vdo_page_write(completion);
			continue;
		}

		if (!is_mapped_location(&mapping)) {
			continue;
		}

		(*rebuild->logical_blocks_used)++;
		if (mapping.pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		if (!is_physical_data_block(rebuild->depot, mapping.pbn)) {
			// This is a nonsense mapping. Remove it from the map so
			// we're at least consistent and mark the page dirty.
			page->entries[slot] = pack_pbn(VDO_ZERO_BLOCK,
						       MAPPING_STATE_UNMAPPED);
			request_vdo_page_write(completion);
			continue;
		}

		slab = get_slab(rebuild->depot, mapping.pbn);
		result = adjust_reference_count_for_rebuild(
			slab->reference_counts, mapping.pbn, DATA_INCREMENT);
		if (result != VDO_SUCCESS) {
			log_error_strerror(result,
					   "Could not adjust reference count for PBN %llu, slot %u mapped to PBN %llu",
					   get_block_map_page_pbn(page),
					   slot,
					   mapping.pbn);
			page->entries[slot] = pack_pbn(VDO_ZERO_BLOCK,
						       MAPPING_STATE_UNMAPPED);
			request_vdo_page_write(completion);
		}
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
static void fetch_page(struct rebuild_completion *rebuild,
		       struct vdo_completion *completion);

/**
 * Process a page which has just been loaded. This callback is registered by
 * fetch_page().
 *
 * @param completion  The vdo_page_completion for the fetched page
 **/
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

	release_vdo_page_completion(completion);
	if (finish_if_done(rebuild)) {
		return;
	}

	// Advance progress to the next page, and fetch the next page we
	// haven't yet requested.
	fetch_page(rebuild, completion);
}

/**
 * Fetch a page from the block map.
 *
 * @param rebuild     the rebuild_completion
 * @param completion  the page completion to use
 **/
static void fetch_page(struct rebuild_completion *rebuild,
		       struct vdo_completion *completion)
{
	while (rebuild->page_to_fetch < rebuild->leaf_pages) {
		physical_block_number_t pbn =
			find_block_map_page_pbn(rebuild->block_map,
						rebuild->page_to_fetch++);
		if (pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		if (!is_physical_data_block(rebuild->depot, pbn)) {
			abort_rebuild(rebuild, VDO_BAD_MAPPING);
			if (finish_if_done(rebuild)) {
				return;
			}
			continue;
		}

		init_vdo_page_completion(((struct vdo_page_completion *) completion),
					 rebuild->block_map->zones[0].page_cache,
					 pbn, true,
					 &rebuild->completion, page_loaded,
					 handle_page_load_error);
		rebuild->outstanding++;
		get_vdo_page(completion);
		return;
	}
}

/**
 * Rebuild reference counts from the leaf block map pages now that reference
 * counts have been rebuilt from the interior tree pages (which have been
 * loaded in the process). This callback is registered in
 * rebuild_reference_counts().
 *
 * @param completion  The sub-task completion
 **/
static void rebuild_from_leaves(struct vdo_completion *completion)
{
	page_count_t i;
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	*rebuild->logical_blocks_used = 0;

	// The PBN calculation doesn't work until the tree pages have been
	// loaded, so we can't set this value at the start of rebuild.
	rebuild->last_slot = (struct block_map_slot){
		.slot = rebuild->block_map->entry_count
			% VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		.pbn = find_block_map_page_pbn(rebuild->block_map,
					       rebuild->leaf_pages - 1),
	};

	// Prevent any page from being processed until all pages have been
	// launched.
	rebuild->launching = true;
	for (i = 0; i < rebuild->page_count; i++) {
		fetch_page(rebuild, &rebuild->page_completions[i].completion);
	}
	rebuild->launching = false;
	finish_if_done(rebuild);
}

/**
 * Process a single entry from the block map tree.
 *
 * <p>Implements vdo_entry_callback.
 *
 * @param pbn         A pbn which holds a block map tree page
 * @param completion  The parent completion of the traversal
 *
 * @return VDO_SUCCESS or an error
 **/
static int process_entry(physical_block_number_t pbn,
			struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild =
		as_rebuild_completion(completion->parent);
	struct vdo_slab *slab;
	int result;

	if ((pbn == VDO_ZERO_BLOCK)
	    || !is_physical_data_block(rebuild->depot, pbn)) {
		return log_error_strerror(VDO_BAD_CONFIGURATION,
					  "PBN %llu out of range",
					  pbn);
	}

	slab = get_slab(rebuild->depot, pbn);
	result = adjust_reference_count_for_rebuild(
		slab->reference_counts, pbn, BLOCK_MAP_INCREMENT);
	if (result != VDO_SUCCESS) {
		return log_error_strerror(result,
					  "Could not adjust reference count for block map tree PBN %llu",
					  pbn);
	}

	(*rebuild->block_map_data_blocks)++;
	return VDO_SUCCESS;
}

/**********************************************************************/
void rebuild_reference_counts(struct vdo *vdo,
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
		finish_vdo_completion(parent, result);
		return;
	}

	// Completion chaining from page cache hits can lead to stack overflow
	// during the rebuild, so clear out the cache before this rebuild phase.
	result =
		invalidate_vdo_page_cache(rebuild->block_map->zones[0].page_cache);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	// First traverse the block map trees.
	*rebuild->block_map_data_blocks = 0;
	completion = &rebuild->sub_task_completion;
	prepare_vdo_completion(completion, rebuild_from_leaves,
			       finish_vdo_completion_parent_callback,
			       rebuild->logical_thread_id,
			       &rebuild->completion);
	traverse_vdo_forest(rebuild->block_map, process_entry, completion);
}
