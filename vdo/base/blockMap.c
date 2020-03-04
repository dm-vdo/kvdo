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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMap.c#40 $
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
	BlockCount flat_page_count;
	PhysicalBlockNumber root_origin;
	BlockCount root_count;
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

	BlockMapPageValidity validity = validateBlockMapPage(page, nonce, pbn);
	if (validity == BLOCK_MAP_PAGE_BAD) {
		return logErrorWithStringError(VDO_BAD_PAGE,
					       "Expected page %" PRIu64
					       " but got page %" PRIu64
					       " instead",
					       pbn,
					       getBlockMapPagePBN(page));
	}

	if (validity == BLOCK_MAP_PAGE_INVALID) {
		formatBlockMapPage(page, nonce, pbn, false);
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

	if (markBlockMapPageInitialized(page, true)) {
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
PageCount compute_block_map_page_count(BlockCount entries)
{
	return compute_bucket_count(entries, BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**********************************************************************/
int make_block_map(BlockCount logical_blocks,
		   const ThreadConfig *thread_config,
		   BlockCount flat_page_count,
		   PhysicalBlockNumber root_origin,
		   BlockCount root_count,
		   struct block_map **map_ptr)
{
	STATIC_ASSERT(BLOCK_MAP_ENTRIES_PER_PAGE ==
		      ((VDO_BLOCK_SIZE - sizeof(struct block_map_page)) /
		       sizeof(BlockMapEntry)));

	struct block_map *map;
	int result = ALLOCATE_EXTENDED(struct block_map,
				       thread_config->logicalZoneCount,
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

	ZoneCount zone_count = thread_config->logicalZoneCount;
	ZoneCount zone = 0;
	for (zone = 0; zone < zone_count; zone++) {
		struct block_map_zone *block_map_zone = &map->zones[zone];
		block_map_zone->zone_number = zone;
		block_map_zone->thread_id =
			getLogicalZoneThread(thread_config, zone);
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
static int decode_block_map_state_2_0(Buffer *buffer,
				      struct block_map_state_2_0 *state)
{
	size_t initial_length = contentLength(buffer);

	PhysicalBlockNumber flat_page_origin;
	int result = getUInt64LEFromBuffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	BlockCount flat_page_count;
	result = getUInt64LEFromBuffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	PhysicalBlockNumber root_origin;
	result = getUInt64LEFromBuffer(buffer, &root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	BlockCount root_count;
	result = getUInt64LEFromBuffer(buffer, &root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*state = (struct block_map_state_2_0) {
		.flat_page_origin = flat_page_origin,
		.flat_page_count = flat_page_count,
		.root_origin = root_origin,
		.root_count = root_count,
	};

	size_t decoded_size = initial_length - contentLength(buffer);
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == decoded_size,
		      "decoded block map component size must match header size");
}

/**********************************************************************/
int decode_block_map(Buffer *buffer,
		     BlockCount logical_blocks,
		     const ThreadConfig *thread_config,
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
int decode_sodium_block_map(Buffer *buffer,
			    BlockCount logical_blocks,
			    const ThreadConfig *thread_config,
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
		       BlockCount maximum_age)
{
	zone->read_only_notifier = read_only_notifier;
	int result = initializeTreeZone(zone, layer, maximum_age);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return makeVDOPageCache(layer,
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
	completeCompletion(parent);
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
	advanceVDOPageCachePeriod(zone->page_cache,
				  zone->block_map->current_era_point);
	advanceZoneTreePeriod(&zone->tree_zone, zone->block_map->current_era_point);
	finishCompletion(parent, VDO_SUCCESS);
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
			  BlockCount maximum_age)
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
	uninitializeBlockMapTreeZone(&zone->tree_zone);
	freeVDOPageCache(&zone->page_cache);
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
int encode_block_map(const struct block_map *map, Buffer *buffer)
{
	int result = encode_header(&BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = contentLength(buffer);

	result = putUInt64LEIntoBuffer(buffer, BLOCK_MAP_FLAT_PAGE_ORIGIN);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, map->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, map->root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = putUInt64LEIntoBuffer(buffer, map->root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = contentLength(buffer) - initial_length;
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
		setTreeZoneInitialPeriod(&map->zones[zone].tree_zone,
					 map->current_era_point);
		setVDOPageCacheInitialPeriod(map->zones[zone].page_cache,
					     map->current_era_point);
	}
}

/**********************************************************************/
ZoneCount compute_logical_zone(struct data_vio *data_vio)
{
	struct block_map *map = getBlockMap(getVDOFromDataVIO(data_vio));
	struct tree_lock *tree_lock = &data_vio->treeLock;
	PageNumber page_number = compute_page_number(data_vio->logical.lbn);
	tree_lock->treeSlots[0].pageIndex = page_number;
	tree_lock->rootIndex = page_number % map->root_count;
	return (tree_lock->rootIndex % map->zone_count);
}

/**********************************************************************/
void find_block_map_slot_async(struct data_vio *data_vio,
			       VDOAction *callback,
			       ThreadID thread_id)
{
	struct block_map *map = getBlockMap(getVDOFromDataVIO(data_vio));
	if (data_vio->logical.lbn >= map->entry_count) {
		finishDataVIO(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	struct tree_lock *tree_lock = &data_vio->treeLock;
	struct block_map_tree_slot *slot = &tree_lock->treeSlots[0];
	slot->blockMapSlot.slot = compute_slot(data_vio->logical.lbn);
	if (slot->pageIndex < map->flat_page_count) {
		slot->blockMapSlot.pbn =
			slot->pageIndex + BLOCK_MAP_FLAT_PAGE_ORIGIN;
		launchCallback(dataVIOAsCompletion(data_vio),
			       callback,
			       thread_id);
		return;
	}

	tree_lock->callback = callback;
	tree_lock->threadID = thread_id;
	lookupBlockMapPBN(data_vio);
}

/**********************************************************************/
PageCount get_number_of_fixed_block_map_pages(const struct block_map *map)
{
	return (map->flat_page_count + map->root_count);
}

/**********************************************************************/
BlockCount get_number_of_block_map_entries(const struct block_map *map)
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
	if (is_draining(&zone->state) && !isTreeZoneActive(&zone->tree_zone) &&
	    !isPageCacheActive(zone->page_cache)) {
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
	drainZoneTrees(&zone->tree_zone);
	drainVDOPageCache(zone->page_cache);
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
	finishCompletion(parent, resume_if_quiescent(&zone->state));
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
			      BlockCount new_logical_blocks)
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
BlockCount get_new_entry_count(struct block_map *map)
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
	completeCompletion(completion);
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
	releaseVDOPageCompletion(completion);
	continueCompletion(parent, result);
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
		   VDOAction *action)
{
	struct block_map_zone *zone =
		get_block_map_for_zone(data_vio->logical.zone);
	if (is_draining(&zone->state)) {
		finishDataVIO(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	initVDOPageCompletion(&data_vio->pageCompletion,
			      zone->page_cache,
			      data_vio->treeLock.treeSlots[0].blockMapSlot.pbn,
			      modifiable,
			      dataVIOAsCompletion(data_vio),
			      action,
			      handle_page_error);
	getVDOPageAsync(&data_vio->pageCompletion.completion);
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
set_mapped_entry(struct data_vio *data_vio, const BlockMapEntry *entry)
{
	// Unpack the PBN for logging purposes even if the entry is invalid.
	struct data_location mapped = unpackBlockMapEntry(entry);

	if (isValidLocation(&mapped)) {
		int result =
			setMappedLocation(data_vio, mapped.pbn, mapped.state);
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
	if (isReadDataVIO(data_vio)) {
		return VDO_BAD_MAPPING;
	}

	// A write VIO only reads this mapping to decref the old block. Treat
	// this as an unmapped entry rather than fail the write.
	clearMappedLocation(data_vio);
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
		dereferenceReadableVDOPage(completion);
	int result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	struct data_vio *data_vio = asDataVIO(completion->parent);
	struct block_map_tree_slot *tree_slot =
		&data_vio->treeLock.treeSlots[0];
	const BlockMapEntry *entry =
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

	struct block_map_page *page = dereferenceWritableVDOPage(completion);
	int result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	struct data_vio *data_vio = asDataVIO(completion->parent);
	struct block_map_page_context *context =
		getVDOPageCompletionContext(completion);
	SequenceNumber oldLock = context->recovery_lock;
	updateBlockMapPage(page,
			   data_vio,
			   data_vio->newMapped.pbn,
			   data_vio->newMapped.state,
			   &context->recovery_lock);
	markCompletedVDOPageDirty(completion, oldLock, context->recovery_lock);
	finish_processing_page(completion, VDO_SUCCESS);
}

/**********************************************************************/
void get_mapped_block_async(struct data_vio *data_vio)
{
	if (data_vio->treeLock.treeSlots[0].blockMapSlot.pbn == ZERO_BLOCK) {
		// We know that the block map page for this LBN has not been
		// allocated, so the block must be unmapped.
		clearMappedLocation(data_vio);
		continueDataVIO(data_vio, VDO_SUCCESS);
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
BlockMapStatistics get_block_map_statistics(struct block_map *map)
{
	BlockMapStatistics stats;
	memset(&stats, 0, sizeof(BlockMapStatistics));

	ZoneCount zone = 0;
	for (zone = 0; zone < map->zone_count; zone++) {
		const struct atomic_page_cache_statistics *atoms =
			getVDOPageCacheStatistics(map->zones[zone].page_cache);
		stats.dirtyPages += atomicLoad64(&atoms->counts.dirtyPages);
		stats.cleanPages += atomicLoad64(&atoms->counts.cleanPages);
		stats.freePages += atomicLoad64(&atoms->counts.freePages);
		stats.failedPages += atomicLoad64(&atoms->counts.failedPages);
		stats.incomingPages +=
			atomicLoad64(&atoms->counts.incomingPages);
		stats.outgoingPages +=
			atomicLoad64(&atoms->counts.outgoingPages);

		stats.cachePressure += atomicLoad64(&atoms->cachePressure);
		stats.readCount += atomicLoad64(&atoms->readCount);
		stats.writeCount += atomicLoad64(&atoms->writeCount);
		stats.failedReads += atomicLoad64(&atoms->failedReads);
		stats.failedWrites += atomicLoad64(&atoms->failedWrites);
		stats.reclaimed += atomicLoad64(&atoms->reclaimed);
		stats.readOutgoing += atomicLoad64(&atoms->readOutgoing);
		stats.foundInCache += atomicLoad64(&atoms->foundInCache);
		stats.discardRequired += atomicLoad64(&atoms->discardRequired);
		stats.waitForPage += atomicLoad64(&atoms->waitForPage);
		stats.fetchRequired += atomicLoad64(&atoms->fetchRequired);
		stats.pagesLoaded += atomicLoad64(&atoms->pagesLoaded);
		stats.pagesSaved += atomicLoad64(&atoms->pagesSaved);
		stats.flushCount += atomicLoad64(&atoms->flushCount);
	}

	return stats;
}
