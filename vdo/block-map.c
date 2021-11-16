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
 */

#include "block-map.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "action-manager.h"
#include "admin-state.h"
#include "block-map-format.h"
#include "block-map-page.h"
#include "block-map-tree.h"
#include "constants.h"
#include "data-vio.h"
#include "forest.h"
#include "num-utils.h"
#include "recovery-journal.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"
#include "vdo-page-cache.h"

/**
 * State associated which each block map page while it is in the VDO page
 * cache.
 **/
struct block_map_page_context {
	/**
	 * The earliest recovery journal block containing uncommitted updates
	 * to the block map page associated with this context. A reference
	 * (lock) is held on that block to prevent it from being reaped. When
	 * this value changes, the reference on the old value must be released
	 * and a reference on the new value must be acquired.
	 **/
	sequence_number_t recovery_lock;
};

/**
 * Implements vdo_page_read_function.
 **/
static int validate_page_on_read(void *buffer,
				 physical_block_number_t pbn,
				 struct block_map_zone *zone,
				 void *page_context)
{
	struct block_map_page *page = buffer;
	struct block_map_page_context *context = page_context;
	nonce_t nonce = zone->block_map->nonce;

	enum block_map_page_validity validity =
		vdo_validate_block_map_page(page, nonce, pbn);
	if (validity == VDO_BLOCK_MAP_PAGE_BAD) {
		return uds_log_error_strerror(VDO_BAD_PAGE,
					      "Expected page %llu but got page %llu instead",
					      (unsigned long long) pbn,
					      (unsigned long long) vdo_get_block_map_page_pbn(page));
	}

	if (validity == VDO_BLOCK_MAP_PAGE_INVALID) {
		vdo_format_block_map_page(page, nonce, pbn, false);
	}

	context->recovery_lock = 0;
	return VDO_SUCCESS;
}

/**
 * Handle journal updates and torn write protection.
 *
 * Implements vdo_page_write_function.
 **/
static bool handle_page_write(void *raw_page,
			      struct block_map_zone *zone,
			      void *page_context)
{
	struct block_map_page *page = raw_page;
	struct block_map_page_context *context = page_context;

	if (vdo_mark_block_map_page_initialized(page, true)) {
		/* Cause the page to be re-written. */
		return true;
	}

	/* Release the page's references on the recovery journal. */
	vdo_release_recovery_journal_block_reference(zone->block_map->journal,
						     context->recovery_lock,
						     VDO_ZONE_TYPE_LOGICAL,
						     zone->zone_number);
	context->recovery_lock = 0;
	return false;
}

/**
 * Initialize the per-zone portions of the block map.
 *
 * @param map                 The block map
 * @param zone_number         The number of the zone to initialize
 * @param thread_config       The thread config of the VDO
 * @param vdo                 The VDO
 * @param read_only_notifier  The read-only context for the VDO
 * @param cache_size          The size of the page cache for the block map
 * @param maximum_age         The number of journal blocks before a dirtied
 *                            page is considered old and must be written out
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
initialize_block_map_zone(struct block_map *map,
			  zone_count_t zone_number,
			  const struct thread_config *thread_config,
			  struct vdo *vdo,
			  struct read_only_notifier *read_only_notifier,
			  page_count_t cache_size,
			  block_count_t maximum_age)
{
	int result;

	struct block_map_zone *zone = &map->zones[zone_number];

	zone->zone_number = zone_number;
	zone->thread_id =
		vdo_get_logical_zone_thread(thread_config, zone_number);
	zone->block_map = map;
	zone->read_only_notifier = read_only_notifier;
	result = vdo_initialize_tree_zone(zone, vdo, maximum_age);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_set_admin_state_code(&zone->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	return vdo_make_page_cache(vdo,
				   cache_size / map->zone_count,
				   validate_page_on_read,
				   handle_page_write,
				   sizeof(struct block_map_page_context),
				   maximum_age,
				   zone,
				   &zone->page_cache);
}

/**
 * Get the portion of the block map for a given logical zone.
 *
 * @param map          The map
 * @param zone_number  The number of the zone
 *
 * @return The requested block map zone
 **/
struct block_map_zone *vdo_get_block_map_zone(struct block_map *map,
					      zone_count_t zone_number)
{
	return &map->zones[zone_number];
}

/**
 * Get the ID of the thread on which a given block map zone operates.
 *
 * <p>Implements vdo_zone_thread_getter.
 **/
static thread_id_t get_block_map_zone_thread_id(void *context,
						zone_count_t zone_number)
{
	return vdo_get_block_map_zone(context, zone_number)->thread_id;
}

/**
 * Prepare for an era advance.
 *
 * <p>Implements vdo_action_preamble.
 **/
static void prepare_for_era_advance(void *context,
				    struct vdo_completion *parent)
{
	struct block_map *map = context;

	map->current_era_point = map->pending_era_point;
	vdo_complete_completion(parent);
}

/**
 * Update the progress of the era in a zone.
 *
 * <p>Implements vdo_zone_action.
 **/
static void advance_block_map_zone_era(void *context,
				       zone_count_t zone_number,
				       struct vdo_completion *parent)
{
	struct block_map_zone *zone =
		vdo_get_block_map_zone(context, zone_number);
	vdo_advance_page_cache_period(zone->page_cache,
				      zone->block_map->current_era_point);
	vdo_advance_zone_tree_period(&zone->tree_zone,
				     zone->block_map->current_era_point);
	vdo_finish_completion(parent, VDO_SUCCESS);
}

/**
 * Schedule an era advance if necessary. This method should not be called
 * directly. Rather, call vdo_schedule_default_action() on the block map's action
 * manager.
 *
 * <p>Implements vdo_action_scheduler.
 **/
static bool schedule_era_advance(void *context)
{
	struct block_map *map = context;

	if (map->current_era_point == map->pending_era_point) {
		return false;
	}

	return vdo_schedule_action(map->action_manager,
				   prepare_for_era_advance,
				   advance_block_map_zone_era,
				   NULL,
				   NULL);
}

/**
 * Clean up a block_map_zone.
 *
 * @param zone  The zone to uninitialize
 **/
static void uninitialize_block_map_zone(struct block_map_zone *zone)
{
	vdo_uninitialize_block_map_tree_zone(&zone->tree_zone);
	vdo_free_page_cache(UDS_FORGET(zone->page_cache));
}

/**
 * Free a block map.
 *
 * @param map  The block map to free
 **/
void vdo_free_block_map(struct block_map *map)
{
	zone_count_t zone;

	if (map == NULL) {
		return;
	}

	for (zone = 0; zone < map->zone_count; zone++) {
		uninitialize_block_map_zone(&map->zones[zone]);
	}

	vdo_abandon_block_map_growth(map);
	vdo_free_forest(UDS_FORGET(map->forest));
	UDS_FREE(UDS_FORGET(map->action_manager));

	UDS_FREE(map);
}

/**
 * Make a block map and configure it with the state read from the super block.
 *
 * @param [in]  state               The block map state from the super block
 * @param [in]  logical_blocks      The number of logical blocks for the VDO
 * @param [in]  thread_config       The thread configuration of the VDO
 * @param [in]  vdo                 The vdo
 * @param [in]  read_only_notifier  The read only mode context
 * @param [in]  journal             The recovery journal (may be NULL)
 * @param [in]  nonce               The nonce to distinguish initialized pages
 * @param [in]  cache_size          The block map cache size, in pages
 * @param [in]  maximum_age         The number of journal blocks before a
 *                                  dirtied page
 * @param [out] map_ptr             The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int vdo_decode_block_map(struct block_map_state_2_0 state,
			 block_count_t logical_blocks,
			 const struct thread_config *thread_config,
			 struct vdo *vdo,
			 struct read_only_notifier *read_only_notifier,
			 struct recovery_journal *journal,
			 nonce_t nonce,
			 page_count_t cache_size,
			 block_count_t maximum_age,
			 struct block_map **map_ptr)
{
	struct block_map *map;
	int result;
	zone_count_t zone = 0;

	STATIC_ASSERT(VDO_BLOCK_MAP_ENTRIES_PER_PAGE ==
		      ((VDO_BLOCK_SIZE - sizeof(struct block_map_page)) /
		       sizeof(struct block_map_entry)));
	result = ASSERT(cache_size > 0,
			"block map cache size is specified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE_EXTENDED(struct block_map,
				       thread_config->logical_zone_count,
				       struct block_map_zone,
				       __func__,
				       &map);
	if (result != UDS_SUCCESS) {
		return result;
	}

	map->root_origin = state.root_origin;
	map->root_count = state.root_count;
	map->entry_count = logical_blocks;
	map->journal = journal;
	map->nonce = nonce;

	result = vdo_make_forest(map, map->entry_count);
	if (result != VDO_SUCCESS) {
		vdo_free_block_map(map);
		return result;
	}

	vdo_replace_forest(map);

	map->zone_count = thread_config->logical_zone_count;
	for (zone = 0; zone < map->zone_count; zone++) {
		result = initialize_block_map_zone(map,
						   zone,
						   thread_config,
						   vdo,
						   read_only_notifier,
						   cache_size,
						   maximum_age);
		if (result != VDO_SUCCESS) {
			vdo_free_block_map(map);
			return result;
		}
	}


	result = vdo_make_action_manager(map->zone_count,
					 get_block_map_zone_thread_id,
					 vdo_get_recovery_journal_thread_id(journal),
					 map,
					 schedule_era_advance,
					 vdo,
					 &map->action_manager);
	if (result != VDO_SUCCESS) {
		vdo_free_block_map(map);
		return result;
	}

	*map_ptr = map;
	return VDO_SUCCESS;
}

/**
 * Record the state of a block map for encoding in a super block.
 *
 * @param map  The block map to encode
 *
 * @return The state of the block map
 **/
struct block_map_state_2_0 vdo_record_block_map(const struct block_map *map)
{
	struct block_map_state_2_0 state = {
		.flat_page_origin = VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
		/*
		 * This is the flat page count, which has turned out to always
		 * be 0.
		 */
		.flat_page_count = 0,
		.root_origin = map->root_origin,
		.root_count = map->root_count,
	};

	return state;
}

/**
 * Obtain any necessary state from the recovery journal that is needed for
 * normal block map operation.
 *
 * @param map      The map in question
 * @param journal  The journal to initialize from
 **/
void vdo_initialize_block_map_from_journal(struct block_map *map,
					   struct recovery_journal *journal)
{
	zone_count_t zone = 0;

	map->current_era_point =
		vdo_get_recovery_journal_current_sequence_number(journal);
	map->pending_era_point = map->current_era_point;

	for (zone = 0; zone < map->zone_count; zone++) {
		vdo_set_tree_zone_initial_period(&map->zones[zone].tree_zone,
						 map->current_era_point);
		vdo_set_page_cache_initial_period(map->zones[zone].page_cache,
						  map->current_era_point);
	}
}

/**
 * Compute the logical zone on which the entry for a data_vio
 * resides
 *
 * @param data_vio  The data_vio
 *
 * @return The logical zone number for the data_vio
 **/
zone_count_t vdo_compute_logical_zone(struct data_vio *data_vio)
{
	struct block_map *map = vdo_get_from_data_vio(data_vio)->block_map;
	struct tree_lock *tree_lock = &data_vio->tree_lock;

	page_number_t page_number
		= vdo_compute_page_number(data_vio->logical.lbn);
	tree_lock->tree_slots[0].page_index = page_number;
	tree_lock->root_index = page_number % map->root_count;
	return (tree_lock->root_index % map->zone_count);
}

/**
 * Compute the block map slot in which the block map entry for a data_vio
 * resides, and cache that number in the data_vio.
 *
 * @param data_vio  The data_vio
 * @param callback  The function to call once the slot has been found
 * @param thread_id The thread on which to run the callback
 **/
void vdo_find_block_map_slot(struct data_vio *data_vio,
			     vdo_action *callback,
			     thread_id_t thread_id)
{
	struct block_map *map = vdo_get_from_data_vio(data_vio)->block_map;
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	struct block_map_tree_slot *slot = &tree_lock->tree_slots[0];

	if (data_vio->logical.lbn >= map->entry_count) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	slot->block_map_slot.slot = vdo_compute_slot(data_vio->logical.lbn);
	tree_lock->callback = callback;
	tree_lock->thread_id = thread_id;
	vdo_lookup_block_map_pbn(data_vio);
}

/**
 * Get number of block map entries.
 *
 * @param map  The block map
 *
 * @return The number of entries stored in the map
 **/
block_count_t vdo_get_number_of_block_map_entries(const struct block_map *map)
{
	return map->entry_count;
}

/**
 * Notify the block map that the recovery journal has finished a new block.
 * This method must be called from the journal zone thread.
 *
 * @param map                    The block map
 * @param recovery_block_number  The sequence number of the finished recovery
 *                               journal block
 **/
void vdo_advance_block_map_era(struct block_map *map,
			       sequence_number_t recovery_block_number)
{
	if (map == NULL) {
		return;
	}

	map->pending_era_point = recovery_block_number;
	vdo_schedule_default_action(map->action_manager);
}

/**
 * Check whether a zone of the block map has drained, and if so, send a
 * notification thereof.
 *
 * @param zone  The zone to check
 **/
void vdo_block_map_check_for_drain_complete(struct block_map_zone *zone)
{
	if (vdo_is_state_draining(&zone->state) &&
	    !vdo_is_tree_zone_active(&zone->tree_zone) &&
	    !vdo_is_page_cache_active(zone->page_cache)) {
		vdo_finish_draining_with_result(&zone->state,
						(vdo_is_read_only(zone->read_only_notifier) ?
							VDO_READ_ONLY : VDO_SUCCESS));
	}
}

/**
 * Initiate a drain of the trees and page cache of a block map zone.
 *
 * Implements vdo_admin_initiator
 **/
static void initiate_drain(struct admin_state *state)
{
	struct block_map_zone *zone =
		container_of(state, struct block_map_zone, state);
	vdo_drain_zone_trees(&zone->tree_zone);
	vdo_drain_page_cache(zone->page_cache);
	vdo_block_map_check_for_drain_complete(zone);
}

/**
 * Drain a zone of the block map.
 *
 * <p>Implements vdo_zone_action.
 **/
static void
drain_zone(void *context, zone_count_t zone_number,
	   struct vdo_completion *parent)
{
	struct block_map_zone *zone = vdo_get_block_map_zone(context, zone_number);

	vdo_start_draining(&zone->state,
			   vdo_get_current_manager_operation(zone->block_map->action_manager),
			   parent,
			   initiate_drain);
}

/**
 * Quiesce all block map I/O, possibly writing out all dirty metadata.
 *
 * @param map        The block map to drain
 * @param operation  The type of drain to perform
 * @param parent     The completion to notify when the drain is complete
 **/
void vdo_drain_block_map(struct block_map *map,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager, operation, NULL,
			       drain_zone, NULL, parent);
}

/**
 * Resume a zone of the block map.
 *
 * <p>Implements vdo_zone_action.
 **/
static void resume_block_map_zone(void *context,
				  zone_count_t zone_number,
				  struct vdo_completion *parent)
{
	struct block_map_zone *zone = vdo_get_block_map_zone(context, zone_number);

	vdo_finish_completion(parent, vdo_resume_if_quiescent(&zone->state));
}

/**
 * Resume I/O for a quiescent block map.
 *
 * @param map     The block map to resume
 * @param parent  The completion to notify when the resume is complete
 **/
void vdo_resume_block_map(struct block_map *map, struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager,
			       VDO_ADMIN_STATE_RESUMING,
			       NULL,
			       resume_block_map_zone,
			       NULL,
			       parent);
}

/**
 * Prepare to grow the block map by allocating an expanded collection of trees.
 *
 * @param map                 The block map to grow
 * @param new_logical_blocks  The new logical size of the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_prepare_to_grow_block_map(struct block_map *map,
				  block_count_t new_logical_blocks)
{
	if (map->next_entry_count == new_logical_blocks) {
		return VDO_SUCCESS;
	}

	if (map->next_entry_count > 0) {
		vdo_abandon_block_map_growth(map);
	}

	if (new_logical_blocks < map->entry_count) {
		map->next_entry_count = map->entry_count;
		return VDO_SUCCESS;
	}

	return vdo_make_forest(map, new_logical_blocks);
}

/**
 * Get the logical size to which this block map is prepared to grow.
 *
 * @param map  The block map
 *
 * @return The new number of entries the block map will be grown to or 0 if
 *         the block map is not prepared to grow
 **/
block_count_t vdo_get_new_entry_count(struct block_map *map)
{
	return map->next_entry_count;
}

/**
 * Grow the block map by replacing the forest with the one which was prepared.
 *
 * Implements vdo_action_preamble
 **/
static void grow_forest(void *context, struct vdo_completion *completion)
{
	vdo_replace_forest(context);
	vdo_complete_completion(completion);
}

/**
 * Grow a block map on which vdo_prepare_to_grow_block_map() has already been
 *called.
 *
 * @param map     The block map to grow
 * @param parent  The object to notify when the growth is complete
 **/
void vdo_grow_block_map(struct block_map *map, struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager,
			       VDO_ADMIN_STATE_SUSPENDED_OPERATION,
			       grow_forest,
			       NULL,
			       NULL,
			       parent);
}

/**
 * Abandon any preparations which were made to grow this block map.
 *
 * @param map  The map which won't be grown
 **/
void vdo_abandon_block_map_growth(struct block_map *map)
{
	vdo_abandon_forest(map);
}

/**
 * Finish processing a block map get or put operation. This function releases
 * the page completion and then continues the requester.
 *
 * @param completion  The completion for the page fetch
 * @param result      The result of the block map operation
 **/
static inline void finish_processing_page(struct vdo_completion *completion,
					  int result)
{
	struct vdo_completion *parent = completion->parent;

	vdo_release_page_completion(completion);
	vdo_continue_completion(parent, result);
}

/**
 * Handle an error fetching a page from the cache. This error handler is
 * registered in setup_mapped_block().
 *
 * @param completion  The page completion which got an error
 **/
static void handle_page_error(struct vdo_completion *completion)
{
	finish_processing_page(completion, completion->result);
}

/**
 * Get the mapping page for a get/put mapped block operation and dispatch to
 * the appropriate handler.
 *
 * @param data_vio     The data_vio
 * @param modifiable   Whether we intend to modify the mapping
 * @param action       The handler to process the mapping page
 **/
static void
setup_mapped_block(struct data_vio *data_vio, bool modifiable,
		   vdo_action *action)
{
	struct block_map_zone *zone = data_vio->logical.zone->block_map_zone;
	if (vdo_is_state_draining(&zone->state)) {
		finish_data_vio(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	vdo_init_page_completion(&data_vio->page_completion,
				 zone->page_cache,
				 data_vio->tree_lock.tree_slots[0].block_map_slot.pbn,
				 modifiable,
				 data_vio_as_completion(data_vio),
				 action,
				 handle_page_error);
	vdo_get_page(&data_vio->page_completion.completion);
}

/**
 * Decode and validate a block map entry and attempt to use it to set the
 * mapped location of a data_vio.
 *
 * @param data_vio  The data_vio to update with the map entry
 * @param entry    The block map entry for the logical block
 *
 * @return VDO_SUCCESS or VDO_BAD_MAPPING if the map entry is invalid
 *         or an error code for any other failure
 **/
static int __must_check
set_mapped_entry(struct data_vio *data_vio,
		 const struct block_map_entry *entry)
{
	/* Unpack the PBN for logging purposes even if the entry is invalid. */
	struct data_location mapped = vdo_unpack_block_map_entry(entry);

	if (vdo_is_valid_location(&mapped)) {
		int result = set_data_vio_mapped_location(data_vio, mapped.pbn,
							  mapped.state);
		/*
		 * Return success and all errors not specifically known to be
		 * errors from validating the location. Yes, this expression is
		 * redundant; it is intentional.
		 */
		if ((result == VDO_SUCCESS) || ((result != VDO_OUT_OF_RANGE) &&
						(result != VDO_BAD_MAPPING))) {
			return result;
		}
	}

	/*
	 * Log the corruption even if we wind up ignoring it for write VIOs,
	 * converting all cases to VDO_BAD_MAPPING.
	 */
	uds_log_error_strerror(VDO_BAD_MAPPING,
			       "PBN %llu with state %u read from the block map was invalid",
			       (unsigned long long) mapped.pbn,
			       mapped.state);

	/*
	 * A read VIO has no option but to report the bad mapping--reading
	 * zeros would be hiding known data loss.
	 */
	if (is_read_data_vio(data_vio)) {
		return VDO_BAD_MAPPING;
	}

	/*
	 * A write VIO only reads this mapping to decref the old block. Treat
	 * this as an unmapped entry rather than fail the write.
	 */
	clear_data_vio_mapped_location(data_vio);
	return VDO_SUCCESS;
}

/**
 * This callback is registered in vdo_get_mapped_block().
 **/
static void get_mapping_from_fetched_page(struct vdo_completion *completion)
{
	int result;
	const struct block_map_page *page;
	const struct block_map_entry *entry;
	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_tree_slot *tree_slot;

	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	page = vdo_dereference_readable_page(completion);
	result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	tree_slot = &data_vio->tree_lock.tree_slots[0];
	entry = &page->entries[tree_slot->block_map_slot.slot];

	result = set_mapped_entry(data_vio, entry);
	finish_processing_page(completion, result);
}

/**
 * Update an entry on a block map page.
 *
 * @param [in]     page           The page to update
 * @param [in]     data_vio       The data_vio making the update
 * @param [in]     pbn            The new PBN for the entry
 * @param [in]     mapping_state  The new mapping state for the entry
 * @param [in,out] recovery_lock  A reference to the current recovery sequence
 *                                number lock held by the page. Will be updated
 *                                if the lock changes to protect the new entry
 **/
void vdo_update_block_map_page(struct block_map_page *page,
			       struct data_vio *data_vio,
			       physical_block_number_t pbn,
			       enum block_mapping_state mapping_state,
			       sequence_number_t *recovery_lock)
{
	struct block_map_zone *zone = data_vio->logical.zone->block_map_zone;
	struct block_map *block_map = zone->block_map;
	struct recovery_journal *journal = block_map->journal;
	sequence_number_t old_locked, new_locked;

	/* Encode the new mapping. */
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	slot_number_t slot =
		tree_lock->tree_slots[tree_lock->height].block_map_slot.slot;
	page->entries[slot] = vdo_pack_pbn(pbn, mapping_state);

	/* Adjust references (locks) on the recovery journal blocks. */
	old_locked = *recovery_lock;
	new_locked = data_vio->recovery_sequence_number;

	if ((old_locked == 0) || (old_locked > new_locked)) {
		/* Acquire a lock on the newly referenced journal block. */
		vdo_acquire_recovery_journal_block_reference(journal,
							     new_locked,
							     VDO_ZONE_TYPE_LOGICAL,
							     zone->zone_number);

		/* If the block originally held a newer lock, release it. */
		if (old_locked > 0) {
			vdo_release_recovery_journal_block_reference(journal,
								     old_locked,
								     VDO_ZONE_TYPE_LOGICAL,
								     zone->zone_number);
		}

		*recovery_lock = new_locked;
	}

	/* Release the transferred lock from the data_vio. */
	vdo_release_journal_per_entry_lock_from_other_zone(journal, new_locked);
	data_vio->recovery_sequence_number = 0;
}

/**
 * This callback is registered in vdo_put_mapped_block().
 **/
static void put_mapping_in_fetched_page(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_page *page;
	struct block_map_page_context *context;
	sequence_number_t old_lock;
	int result;

	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	page = vdo_dereference_writable_page(completion);
	result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	context = vdo_get_page_completion_context(completion);
	old_lock = context->recovery_lock;
	vdo_update_block_map_page(page,
				  data_vio,
				  data_vio->new_mapped.pbn,
				  data_vio->new_mapped.state,
				  &context->recovery_lock);
	vdo_mark_completed_page_dirty(completion, old_lock,
				      context->recovery_lock);
	finish_processing_page(completion, VDO_SUCCESS);
}

/**
 * Get the block number of the physical block containing the data for the
 * specified logical block number. All blocks are mapped to physical block
 * zero by default, which is conventionally the zero block.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void vdo_get_mapped_block(struct data_vio *data_vio)
{
	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn ==
	    VDO_ZERO_BLOCK) {
		/*
		 * We know that the block map page for this LBN has not been
		 * allocated, so the block must be unmapped.
		 */
		clear_data_vio_mapped_location(data_vio);
		continue_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	setup_mapped_block(data_vio, false, get_mapping_from_fetched_page);
}

/**
 * Associate the logical block number for a block represented by a data_vio
 * with the physical block number in its new_mapped field.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void vdo_put_mapped_block(struct data_vio *data_vio)
{
	setup_mapped_block(data_vio, true, put_mapping_in_fetched_page);
}

/**
 * Get the stats for the block map page cache.
 *
 * @param map  The block map containing the cache
 *
 * @return The block map statistics
 **/
struct block_map_statistics vdo_get_block_map_statistics(struct block_map *map)
{
	zone_count_t zone = 0;
	struct block_map_statistics totals;

	memset(&totals, 0, sizeof(struct block_map_statistics));

	for (zone = 0; zone < map->zone_count; zone++) {
		struct vdo_page_cache *cache = map->zones[zone].page_cache;
		struct block_map_statistics stats =
			vdo_get_page_cache_statistics(cache);

		totals.dirty_pages += stats.dirty_pages;
		totals.clean_pages += stats.clean_pages;
		totals.free_pages += stats.free_pages;
		totals.failed_pages += stats.failed_pages;
		totals.incoming_pages += stats.incoming_pages;
		totals.outgoing_pages += stats.outgoing_pages;
		totals.cache_pressure += stats.cache_pressure;
		totals.read_count += stats.read_count;
		totals.write_count += stats.write_count;
		totals.failed_reads += stats.failed_reads;
		totals.failed_writes += stats.failed_writes;
		totals.reclaimed += stats.reclaimed;
		totals.read_outgoing += stats.read_outgoing;
		totals.found_in_cache += stats.found_in_cache;
		totals.discard_required += stats.discard_required;
		totals.wait_for_page += stats.wait_for_page;
		totals.fetch_required += stats.fetch_required;
		totals.pages_loaded += stats.pages_loaded;
		totals.pages_saved += stats.pages_saved;
		totals.flush_count += stats.flush_count;
	}

	return totals;
}
