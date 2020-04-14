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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMap.c#55 $
 */

#include "blockMap.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "actionManager.h"
#include "adminState.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapTree.h"
#include "constants.h"
#include "dataVIO.h"
#include "forest.h"
#include "numUtils.h"
#include "recoveryJournal.h"
#include "statusCodes.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

struct block_map_state_2_0 {
	PhysicalBlockNumber flat_page_origin;
	block_count_t flat_page_count;
	PhysicalBlockNumber root_origin;
	block_count_t root_count;
} __attribute__((packed));

static const struct header BLOCK_MAP_HEADER_2_0 = {
	.id = BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

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
	SequenceNumber recovery_lock;
};

/**
 * Implements VDOPageReadFunction.
 **/
static int validate_page_on_read(void *buffer,
				 PhysicalBlockNumber pbn,
				 struct block_map_zone *zone,
				 void *page_context)
{
	struct block_map_page *page = buffer;
	struct block_map_page_context *context = page_context;
	Nonce nonce = zone->block_map->nonce;

	block_map_page_validity validity = validate_block_map_page(page,
								   nonce,
								   pbn);
	if (validity == BLOCK_MAP_PAGE_BAD) {
		return logErrorWithStringError(VDO_BAD_PAGE,
					       "Expected page %" PRIu64
					       " but got page %" PRIu64
					       " instead",
					       pbn,
					       get_block_map_page_pbn(page));
	}

	if (validity == BLOCK_MAP_PAGE_INVALID) {
		format_block_map_page(page, nonce, pbn, false);
	}

	context->recovery_lock = 0;
	return VDO_SUCCESS;
}

/**
 * Handle journal updates and torn write protection.
 *
 * Implements VDOPageWriteFunction.
 **/
static bool handle_page_write(void *raw_page,
			      struct block_map_zone *zone,
			      void *page_context)
{
	struct block_map_page *page = raw_page;
	struct block_map_page_context *context = page_context;

	if (mark_block_map_page_initialized(page, true)) {
		// Cause the page to be re-written.
		return true;
	}

	// Release the page's references on the recovery journal.
	release_recovery_journal_block_reference(zone->block_map->journal,
						 context->recovery_lock,
						 ZONE_TYPE_LOGICAL,
						 zone->zone_number);
	context->recovery_lock = 0;
	return false;
}

/**********************************************************************/
PageCount compute_block_map_page_count(block_count_t entries)
{
	return compute_bucket_count(entries, BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**********************************************************************/
int make_block_map(block_count_t logical_blocks,
		   const struct thread_config *thread_config,
		   block_count_t flat_page_count,
		   PhysicalBlockNumber root_origin,
		   block_count_t root_count,
		   struct block_map **map_ptr)
{
	STATIC_ASSERT(BLOCK_MAP_ENTRIES_PER_PAGE ==
		      ((VDO_BLOCK_SIZE - sizeof(struct block_map_page)) /
		       sizeof(block_map_entry)));

	struct block_map *map;
	int result = ALLOCATE_EXTENDED(struct block_map,
				       thread_config->logical_zone_count,
				       struct block_map_zone,
				       __func__,
				       &map);
	if (result != UDS_SUCCESS) {
		return result;
	}

	map->flat_page_count = flat_page_count;
	map->root_origin = root_origin;
	map->root_count = root_count;
	map->entry_count = logical_blocks;

	ZoneCount zone_count = thread_config->logical_zone_count;
	ZoneCount zone = 0;
	for (zone = 0; zone < zone_count; zone++) {
		struct block_map_zone *block_map_zone = &map->zones[zone];
		block_map_zone->zone_number = zone;
		block_map_zone->thread_id =
			get_logical_zone_thread(thread_config, zone);
		block_map_zone->block_map = map;
		map->zone_count++;
	}

	*map_ptr = map;
	return VDO_SUCCESS;
}

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decode_block_map_state_2_0(struct buffer *buffer,
				      struct block_map_state_2_0 *state)
{
	size_t initial_length = content_length(buffer);

	PhysicalBlockNumber flat_page_origin;
	int result = get_uint64_le_from_buffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t flat_page_count;
	result = get_uint64_le_from_buffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	PhysicalBlockNumber root_origin;
	result = get_uint64_le_from_buffer(buffer, &root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t root_count;
	result = get_uint64_le_from_buffer(buffer, &root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*state = (struct block_map_state_2_0) {
		.flat_page_origin = flat_page_origin,
		.flat_page_count = flat_page_count,
		.root_origin = root_origin,
		.root_count = root_count,
	};

	size_t decoded_size = initial_length - content_length(buffer);
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == decoded_size,
		      "decoded block map component size must match header size");
}

/**********************************************************************/
int decode_block_map(struct buffer *buffer,
		     block_count_t logical_blocks,
		     const struct thread_config *thread_config,
		     struct block_map **map_ptr)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result =
		validate_header(&BLOCK_MAP_HEADER_2_0, &header, true, __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct block_map_state_2_0 state;
	result = decode_block_map_state_2_0(buffer, &state);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(state.flat_page_origin == BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			BLOCK_MAP_FLAT_PAGE_ORIGIN,
			state.flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct block_map *map;
	result = make_block_map(logical_blocks,
				thread_config,
				state.flat_page_count,
				state.root_origin,
				state.root_count,
				&map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*map_ptr = map;
	return VDO_SUCCESS;
}

/**********************************************************************/
int decode_sodium_block_map(struct buffer *buffer,
			    block_count_t logical_blocks,
			    const struct thread_config *thread_config,
			    struct block_map **map_ptr)
{
	// Sodium uses state version 2.0.
	return decode_block_map(buffer, logical_blocks, thread_config, map_ptr);
}

/**
 * Initialize the per-zone portions of the block map.
 *
 * @param zone                The zone to initialize
 * @param layer               The physical layer on which the zone resides
 * @param read_only_notifier  The read-only context for the VDO
 * @param cache_size          The size of the page cache for the zone
 * @param maximum_age         The number of journal blocks before a dirtied page
 *                            is considered old and must be written out
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
initialize_block_map_zone(struct block_map_zone *zone,
		       PhysicalLayer *layer,
		       struct read_only_notifier *read_only_notifier,
		       PageCount cache_size,
		       block_count_t maximum_age)
{
	zone->read_only_notifier = read_only_notifier;
	int result = initialize_tree_zone(zone, layer, maximum_age);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return make_vdo_page_cache(layer,
				   cache_size,
				   validate_page_on_read,
				   handle_page_write,
				   sizeof(struct block_map_page_context),
				   maximum_age,
				   zone,
				   &zone->page_cache);
}

/**********************************************************************/
struct block_map_zone *get_block_map_zone(struct block_map *map,
					  ZoneCount zoneNumber)
{
	return &map->zones[zoneNumber];
}

/**
 * Get the ID of the thread on which a given block map zone operates.
 *
 * <p>Implements ZoneThreadGetter.
 **/
static ThreadID get_block_map_zone_thread_id(void *context,
					     ZoneCount zone_number)
{
	return get_block_map_zone(context, zone_number)->thread_id;
}

/**
 * Prepare for an era advance.
 *
 * <p>Implements ActionPreamble.
 **/
static void prepare_for_era_advance(void *context,
				    struct vdo_completion *parent)
{
	struct block_map *map = context;
	map->current_era_point = map->pending_era_point;
	complete_completion(parent);
}

/**
 * Update the progress of the era in a zone.
 *
 * <p>Implements ZoneAction.
 **/
static void advance_block_map_zone_era(void *context,
				       ZoneCount zone_number,
				       struct vdo_completion *parent)
{
	struct block_map_zone *zone = get_block_map_zone(context, zone_number);
	advance_vdo_page_cache_period(zone->page_cache,
				      zone->block_map->current_era_point);
	advance_zone_tree_period(&zone->tree_zone,
				 zone->block_map->current_era_point);
	finish_completion(parent, VDO_SUCCESS);
}

/**
 * Schedule an era advance if necessary.
 *
 * <p>Implements ActionScheduler.
 **/
static bool schedule_era_advance(void *context)
{
	struct block_map *map = context;
	if (map->current_era_point == map->pending_era_point) {
		return false;
	}

	return schedule_action(map->action_manager,
			       prepare_for_era_advance,
			       advance_block_map_zone_era,
			       NULL,
			       NULL);
}

/**********************************************************************/
int make_block_map_caches(struct block_map *map,
			  PhysicalLayer *layer,
			  struct read_only_notifier *read_only_notifier,
			  struct recovery_journal *journal,
			  Nonce nonce,
			  PageCount cache_size,
			  block_count_t maximum_age)
{
	int result = ASSERT(cache_size > 0,
			    "block map cache size is specified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	map->journal = journal;
	map->nonce = nonce;

	result = make_forest(map, map->entry_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	replace_forest(map);
	ZoneCount zone = 0;
	for (zone = 0; zone < map->zone_count; zone++) {
		result = initialize_block_map_zone(&map->zones[zone],
						   layer,
						   read_only_notifier,
						   cache_size / map->zone_count,
						   maximum_age);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return make_action_manager(map->zone_count,
				   get_block_map_zone_thread_id,
				   get_recovery_journal_thread_id(journal),
				   map,
				   schedule_era_advance,
				   layer,
				   &map->action_manager);
}

/**
 * Clean up a block_map_zone.
 *
 * @param zone  The zone to uninitialize
 **/
static void uninitialize_block_map_zone(struct block_map_zone *zone)
{
	uninitialize_block_map_tree_zone(&zone->tree_zone);
	free_vdo_page_cache(&zone->page_cache);
}

/**********************************************************************/
void free_block_map(struct block_map **map_ptr)
{
	struct block_map *map = *map_ptr;
	if (map == NULL) {
		return;
	}

	ZoneCount zone = 0;
	for (zone = 0; zone < map->zone_count; zone++) {
		uninitialize_block_map_zone(&map->zones[zone]);
	}

	abandon_block_map_growth(map);
	free_forest(&map->forest);
	free_action_manager(&map->action_manager);

	FREE(map);
	*map_ptr = NULL;
}

/**********************************************************************/
size_t get_block_map_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct block_map_state_2_0);
}

/**********************************************************************/
int encode_block_map(const struct block_map *map, struct buffer *buffer)
{
	int result = encode_header(&BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = put_uint64_le_into_buffer(buffer, BLOCK_MAP_FLAT_PAGE_ORIGIN);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, map->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, map->root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, map->root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**********************************************************************/
void initialize_block_map_from_journal(struct block_map *map,
				       struct recovery_journal *journal)
{
	map->current_era_point = get_current_journal_sequence_number(journal);
	map->pending_era_point = map->current_era_point;

	ZoneCount zone = 0;
	for (zone = 0; zone < map->zone_count; zone++) {
		set_tree_zone_initial_period(&map->zones[zone].tree_zone,
					     map->current_era_point);
		set_vdo_page_cache_initial_period(map->zones[zone].page_cache,
						  map->current_era_point);
	}
}

/**********************************************************************/
ZoneCount compute_logical_zone(struct data_vio *data_vio)
{
	struct block_map *map = get_block_map(get_vdo_from_data_vio(data_vio));
	struct tree_lock *tree_lock = &data_vio->treeLock;
	PageNumber page_number = compute_page_number(data_vio->logical.lbn);
	tree_lock->treeSlots[0].pageIndex = page_number;
	tree_lock->rootIndex = page_number % map->root_count;
	return (tree_lock->rootIndex % map->zone_count);
}

/**********************************************************************/
void find_block_map_slot_async(struct data_vio *data_vio,
			       vdo_action *callback,
			       ThreadID thread_id)
{
	struct block_map *map = get_block_map(get_vdo_from_data_vio(data_vio));
	if (data_vio->logical.lbn >= map->entry_count) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	struct tree_lock *tree_lock = &data_vio->treeLock;
	struct block_map_tree_slot *slot = &tree_lock->treeSlots[0];
	slot->blockMapSlot.slot = compute_slot(data_vio->logical.lbn);
	if (slot->pageIndex < map->flat_page_count) {
		slot->blockMapSlot.pbn =
			slot->pageIndex + BLOCK_MAP_FLAT_PAGE_ORIGIN;
		launch_callback(data_vio_as_completion(data_vio),
			        callback,
			        thread_id);
		return;
	}

	tree_lock->callback = callback;
	tree_lock->threadID = thread_id;
	lookup_block_map_pbn(data_vio);
}

/**********************************************************************/
PageCount get_number_of_fixed_block_map_pages(const struct block_map *map)
{
	return (map->flat_page_count + map->root_count);
}

/**********************************************************************/
block_count_t get_number_of_block_map_entries(const struct block_map *map)
{
	return map->entry_count;
}

/**********************************************************************/
void advance_block_map_era(struct block_map *map,
			   SequenceNumber recovery_block_number)
{
	if (map == NULL) {
		return;
	}

	map->pending_era_point = recovery_block_number;
	schedule_era_advance(map);
}

/**********************************************************************/
void check_for_drain_complete(struct block_map_zone *zone)
{
	if (is_draining(&zone->state) &&
	    !is_tree_zone_active(&zone->tree_zone) &&
	    !is_page_cache_active(zone->page_cache)) {
		finish_draining_with_result(&zone->state,
					    (is_read_only(zone->read_only_notifier) ?
					     VDO_READ_ONLY : VDO_SUCCESS));
	}
}

/**
 * Initiate a drain of the trees and page cache of a block map zone.
 *
 * Implements AdminInitiator
 **/
static void initiate_drain(struct admin_state *state)
{
	struct block_map_zone *zone =
		container_of(state, struct block_map_zone, state);
	drain_zone_trees(&zone->tree_zone);
	drain_vdo_page_cache(zone->page_cache);
	check_for_drain_complete(zone);
}

/**
 * Drain a zone of the block map.
 *
 * <p>Implements ZoneAction.
 **/
static void
drain_zone(void *context, ZoneCount zone_number, struct vdo_completion *parent)
{
	struct block_map_zone *zone = get_block_map_zone(context, zone_number);
	start_draining(&zone->state,
		       get_current_manager_operation(zone->block_map->action_manager),
		       parent,
		       initiate_drain);
}

/**********************************************************************/
void drain_block_map(struct block_map *map,
		     AdminStateCode operation,
		     struct vdo_completion *parent)
{
	schedule_operation(map->action_manager, operation, NULL, drain_zone,
			   NULL, parent);
}

/**
 * Resume a zone of the block map.
 *
 * <p>Implements ZoneAction.
 **/
static void resume_block_map_zone(void *context,
				  ZoneCount zone_number,
				  struct vdo_completion *parent)
{
	struct block_map_zone *zone = get_block_map_zone(context, zone_number);
	finish_completion(parent, resume_if_quiescent(&zone->state));
}

/**********************************************************************/
void resume_block_map(struct block_map *map, struct vdo_completion *parent)
{
	schedule_operation(map->action_manager,
			   ADMIN_STATE_RESUMING,
			   NULL,
			   resume_block_map_zone,
			   NULL,
			   parent);
}

/**********************************************************************/
int prepare_to_grow_block_map(struct block_map *map,
			      block_count_t new_logical_blocks)
{
	if (map->next_entry_count == new_logical_blocks) {
		return VDO_SUCCESS;
	}

	if (map->next_entry_count > 0) {
		abandon_block_map_growth(map);
	}

	if (new_logical_blocks < map->entry_count) {
		map->next_entry_count = map->entry_count;
		return VDO_SUCCESS;
	}

	return make_forest(map, new_logical_blocks);
}

/**********************************************************************/
block_count_t get_new_entry_count(struct block_map *map)
{
	return map->next_entry_count;
}

/**
 * Grow the block map by replacing the forest with the one which was prepared.
 *
 * Implements ActionPreamble
 **/
static void grow_forest(void *context, struct vdo_completion *completion)
{
	replace_forest(context);
	complete_completion(completion);
}

/**********************************************************************/
void grow_block_map(struct block_map *map, struct vdo_completion *parent)
{
	schedule_operation(map->action_manager,
			   ADMIN_STATE_SUSPENDED_OPERATION,
			   grow_forest,
			   NULL,
			   NULL,
			   parent);
}

/**********************************************************************/
void abandon_block_map_growth(struct block_map *map)
{
	abandon_forest(map);
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
	release_vdo_page_completion(completion);
	continue_completion(parent, result);
}

/**
 * Handle an error fetching a page from the cache. This error handler is
 * registered in setupMappedBlock().
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
	struct block_map_zone *zone =
		get_block_map_for_zone(data_vio->logical.zone);
	if (is_draining(&zone->state)) {
		finish_data_vio(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	init_vdo_page_completion(&data_vio->pageCompletion,
				 zone->page_cache,
				 data_vio->treeLock.treeSlots[0].blockMapSlot.pbn,
				 modifiable,
				 data_vio_as_completion(data_vio),
				 action,
				 handle_page_error);
	get_vdo_page_async(&data_vio->pageCompletion.completion);
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
__attribute__((warn_unused_result)) static int
set_mapped_entry(struct data_vio *data_vio, const block_map_entry *entry)
{
	// Unpack the PBN for logging purposes even if the entry is invalid.
	struct data_location mapped = unpack_block_map_entry(entry);

	if (is_valid_location(&mapped)) {
		int result =
			set_mapped_location(data_vio, mapped.pbn, mapped.state);
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

	// Log the corruption even if we wind up ignoring it for write VIOs,
	// converting all cases to VDO_BAD_MAPPING.
	logErrorWithStringError(VDO_BAD_MAPPING,
				"PBN %llu with state %u read from the block map was invalid",
				mapped.pbn,
				mapped.state);

	// A read VIO has no option but to report the bad mapping--reading
	// zeros would be hiding known data loss.
	if (is_read_data_vio(data_vio)) {
		return VDO_BAD_MAPPING;
	}

	// A write VIO only reads this mapping to decref the old block. Treat
	// this as an unmapped entry rather than fail the write.
	clear_mapped_location(data_vio);
	return VDO_SUCCESS;
}

/**
 * This callback is registered in get_mapped_block_async().
 **/
static void get_mapping_from_fetched_page(struct vdo_completion *completion)
{
	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	const struct block_map_page *page =
		dereference_readable_vdo_page(completion);
	int result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_tree_slot *tree_slot =
		&data_vio->treeLock.treeSlots[0];
	const block_map_entry *entry =
		&page->entries[tree_slot->blockMapSlot.slot];

	result = set_mapped_entry(data_vio, entry);
	finish_processing_page(completion, result);
}

/**
 * This callback is registered in put_mapped_block_async().
 **/
static void put_mapping_in_fetched_page(struct vdo_completion *completion)
{
	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	struct block_map_page *page = dereference_writable_vdo_page(completion);
	int result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_page_context *context =
		get_vdo_page_completion_context(completion);
	SequenceNumber oldLock = context->recovery_lock;
	update_block_map_page(page,
			      data_vio,
			      data_vio->newMapped.pbn,
			      data_vio->newMapped.state,
			      &context->recovery_lock);
	mark_completed_vdo_page_dirty(completion, oldLock,
				      context->recovery_lock);
	finish_processing_page(completion, VDO_SUCCESS);
}

/**********************************************************************/
void get_mapped_block_async(struct data_vio *data_vio)
{
	if (data_vio->treeLock.treeSlots[0].blockMapSlot.pbn == ZERO_BLOCK) {
		// We know that the block map page for this LBN has not been
		// allocated, so the block must be unmapped.
		clear_mapped_location(data_vio);
		continue_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	setup_mapped_block(data_vio, false, get_mapping_from_fetched_page);
}

/**********************************************************************/
void put_mapped_block_async(struct data_vio *data_vio)
{
	setup_mapped_block(data_vio, true, put_mapping_in_fetched_page);
}

/**********************************************************************/
struct block_map_statistics get_block_map_statistics(struct block_map *map)
{
	struct block_map_statistics stats;
	memset(&stats, 0, sizeof(struct block_map_statistics));

	ZoneCount zone = 0;
	for (zone = 0; zone < map->zone_count; zone++) {
		const struct atomic_page_cache_statistics *atoms =
			get_vdo_page_cache_statistics(map->zones[zone].page_cache);
		stats.dirty_pages += atomicLoad64(&atoms->counts.dirty_pages);
		stats.clean_pages += atomicLoad64(&atoms->counts.clean_pages);
		stats.free_pages += atomicLoad64(&atoms->counts.free_pages);
		stats.failed_pages += atomicLoad64(&atoms->counts.failed_pages);
		stats.incoming_pages +=
			atomicLoad64(&atoms->counts.incoming_pages);
		stats.outgoing_pages +=
			atomicLoad64(&atoms->counts.outgoing_pages);

		stats.cache_pressure += atomicLoad64(&atoms->cache_pressure);
		stats.read_count += atomicLoad64(&atoms->read_count);
		stats.write_count += atomicLoad64(&atoms->write_count);
		stats.failed_reads += atomicLoad64(&atoms->failed_reads);
		stats.failed_writes += atomicLoad64(&atoms->failed_writes);
		stats.reclaimed += atomicLoad64(&atoms->reclaimed);
		stats.read_outgoing += atomicLoad64(&atoms->read_outgoing);
		stats.found_in_cache += atomicLoad64(&atoms->found_in_cache);
		stats.discard_required +=
			atomicLoad64(&atoms->discard_required);
		stats.wait_for_page += atomicLoad64(&atoms->wait_for_page);
		stats.fetch_required += atomicLoad64(&atoms->fetch_required);
		stats.pages_loaded += atomicLoad64(&atoms->pages_loaded);
		stats.pages_saved += atomicLoad64(&atoms->pages_saved);
		stats.flush_count += atomicLoad64(&atoms->flush_count);
	}

	return stats;
}
