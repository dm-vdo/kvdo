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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logicalZone.c#23 $
 */

#include "logicalZone.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "actionManager.h"
#include "adminState.h"
#include "allocationSelector.h"
#include "atomic.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "dataVIO.h"
#include "flush.h"
#include "intMap.h"
#include "vdoInternal.h"

struct logical_zone {
	/** The completion for flush notifications */
	struct vdo_completion completion;
	/** The owner of this zone */
	struct logical_zones *zones;
	/** Which logical zone this is */
	ZoneCount zone_number;
	/** The thread id for this zone */
	ThreadID thread_id;
	/** In progress operations keyed by LBN */
	struct int_map *lbn_operations;
	/** The logical to physical map */
	struct block_map_zone *block_map_zone;
	/** The current flush generation */
	SequenceNumber flush_generation;
	/** The oldest active generation in this zone */
	SequenceNumber oldest_active_generation;
	/** The number of IOs in the current flush generation */
	BlockCount ios_in_flush_generation;
	/**
	 * The oldest locked generation in this zone (an atomic copy of
	 * oldest_active_generation)
	 **/
	Atomic64 oldest_locked_generation;
	/** The youngest generation of the current notification */
	SequenceNumber notification_generation;
	/** Whether a notification is in progress */
	bool notifying;
	/** The queue of active data write VIOs */
	RingNode write_vios;
	/** The administrative state of the zone */
	struct admin_state state;
	/** The selector for determining which physical zone to allocate from */
	struct allocation_selector *selector;
};

struct logical_zones {
	/** The vdo whose zones these are */
	struct vdo *vdo;
	/** The manager for administrative actions */
	struct action_manager *manager;
	/** The number of zones */
	ZoneCount zone_count;
	/** The logical zones themselves */
	struct logical_zone zones[];
};

/**
 * Convert a generic vdo_completion to a logical_zone.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a logical_zone
 **/
static struct logical_zone *as_logical_zone(struct vdo_completion *completion)
{
	STATIC_ASSERT(offsetof(struct logical_zone, completion) == 0);
	assertCompletionType(completion->type, GENERATION_FLUSHED_COMPLETION);
	return (struct logical_zone *) completion;
}

/**********************************************************************/
struct logical_zone *get_logical_zone(struct logical_zones *zones,
				      ZoneCount zone_number)
{
	return (zone_number < zones->zone_count) ? &zones->zones[zone_number]
		: NULL;
}

/**
 * Implements ZoneThreadGetter
 **/
static ThreadID get_thread_id_for_zone(void *context, ZoneCount zone_number)
{
	return get_logical_zone_thread_id(get_logical_zone(context,
							   zone_number));
}

/**
 * Initialize a logical zone.
 *
 * @param zones        The logical_zones to which this zone belongs
 * @param zone_number  The logical_zone's index
 **/
static int initialize_zone(struct logical_zones *zones, ZoneCount zone_number)
{
	struct logical_zone *zone = &zones->zones[zone_number];
	zone->zones = zones;
	int result = make_int_map(LOCK_MAP_CAPACITY, 0, &zone->lbn_operations);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct vdo *vdo = zones->vdo;
	result = initializeEnqueueableCompletion(&zone->completion,
						 GENERATION_FLUSHED_COMPLETION,
						 vdo->layer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	zone->zone_number = zone_number;
	zone->thread_id = getLogicalZoneThread(getThreadConfig(vdo),
					       zone_number);
	zone->block_map_zone = getBlockMapZone(vdo->blockMap, zone_number);
	initializeRing(&zone->write_vios);
	atomicStore64(&zone->oldest_locked_generation, 0);

	return make_allocation_selector(getThreadConfig(vdo)->physicalZoneCount,
					zone->thread_id, &zone->selector);
}

/**********************************************************************/
int make_logical_zones(struct vdo *vdo, struct logical_zones **zones_ptr)
{
	const ThreadConfig *thread_config = getThreadConfig(vdo);
	if (thread_config->logicalZoneCount == 0) {
		return VDO_SUCCESS;
	}

	struct logical_zones *zones;
	int result = ALLOCATE_EXTENDED(struct logical_zones,
				       thread_config->logicalZoneCount,
				       struct logical_zone, __func__, &zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	zones->vdo = vdo;
	zones->zone_count = thread_config->logicalZoneCount;
	ZoneCount zone;
	for (zone = 0; zone < thread_config->logicalZoneCount; zone++) {
		result = initialize_zone(zones, zone);
		if (result != VDO_SUCCESS) {
			free_logical_zones(&zones);
			return result;
		}
	}

	result = make_action_manager(zones->zone_count, get_thread_id_for_zone,
				     getAdminThread(thread_config), zones, NULL,
				     vdo->layer, &zones->manager);
	if (result != VDO_SUCCESS) {
		free_logical_zones(&zones);
		return result;
	}

	*zones_ptr = zones;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_logical_zones(struct logical_zones **zones_ptr)
{
	struct logical_zones *zones = *zones_ptr;
	if (zones == NULL) {
		return;
	}

	free_action_manager(&zones->manager);

	ZoneCount index;
	for (index = 0; index < zones->zone_count; index++) {
		struct logical_zone *zone = &zones->zones[index];
		free_allocation_selector(&zone->selector);
		destroyEnqueueable(&zone->completion);
		free_int_map(&zone->lbn_operations);
	}

	FREE(zones);
	*zones_ptr = NULL;
}

/**********************************************************************/
static inline void assert_on_zone_thread(struct logical_zone *zone,
					 const char *what)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() == zone->thread_id),
			"%s() called on correct thread", what);
}

/**
 * Check whether this zone has drained.
 *
 * @param zone  The zone to check
 **/
static void check_for_drain_complete(struct logical_zone *zone)
{
	if (!is_draining(&zone->state) || zone->notifying
	    || !isRingEmpty(&zone->write_vios)) {
		return;
	}

	finish_draining(&zone->state);
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state, struct logical_zone, state));
}

/**
 * Drain a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void drain_logical_zone(void *context, ZoneCount zone_number,
			       struct vdo_completion *parent)
{
	struct logical_zone *zone = get_logical_zone(context, zone_number);
	start_draining(&zone->state,
		       get_current_manager_operation(zone->zones->manager),
		       parent, initiate_drain);
}

/**********************************************************************/
void drain_logical_zones(struct logical_zones *zones, AdminStateCode operation,
			 struct vdo_completion *parent)
{
	schedule_operation(zones->manager, operation, NULL, drain_logical_zone,
			   NULL, parent);
}

/**
 * Resume a logical zone.
 *
 * <p>Implements ZoneAction.
 **/
static void resume_logical_zone(void *context, ZoneCount zone_number,
				struct vdo_completion *parent)
{
	struct logical_zone *zone = get_logical_zone(context, zone_number);
	finishCompletion(parent, resume_if_quiescent(&zone->state));
}

/**********************************************************************/
void resume_logical_zones(struct logical_zones *zones,
			  struct vdo_completion *parent)
{
	schedule_operation(zones->manager, ADMIN_STATE_RESUMING, NULL,
			   resume_logical_zone, NULL, parent);
}

/**********************************************************************/
ThreadID get_logical_zone_thread_id(const struct logical_zone *zone)
{
	return zone->thread_id;
}

/**********************************************************************/
struct block_map_zone *get_block_map_for_zone(const struct logical_zone *zone)
{
	return zone->block_map_zone;
}

/**********************************************************************/
struct int_map *get_lbn_lock_map(const struct logical_zone *zone)
{
	return zone->lbn_operations;
}

/**********************************************************************/
struct logical_zone *get_next_logical_zone(const struct logical_zone *zone)
{
	return get_logical_zone(zone->zones, zone->zone_number + 1);
}

/**
 * Convert a RingNode to a data_vio.
 *
 * @param ring_node The RingNode to convert
 *
 * @return The data_vio which owns the RingNode
 **/
static inline struct data_vio *data_vio_from_ring_node(RingNode *ring_node)
{
	return container_of(ring_node, struct data_vio, writeNode);
}

/**
 * Update the oldest active generation. If it has changed, update the
 * atomic copy as well.
 *
 * @param zone  The zone
 *
 * @return <code>true</code> if the oldest active generation has changed
 **/
static bool update_oldest_active_generation(struct logical_zone *zone)
{
	SequenceNumber current_oldest = zone->oldest_active_generation;
	if (isRingEmpty(&zone->write_vios)) {
		zone->oldest_active_generation = zone->flush_generation;
	} else {
		zone->oldest_active_generation =
			data_vio_from_ring_node(zone->write_vios.next)->flushGeneration;
	}

	if (zone->oldest_active_generation == current_oldest) {
		return false;
	}

	atomicStore64(&zone->oldest_locked_generation,
		      zone->oldest_active_generation);
	return true;
}

/**********************************************************************/
void increment_flush_generation(struct logical_zone *zone,
				SequenceNumber expected_generation)
{
	assert_on_zone_thread(zone, __func__);
	ASSERT_LOG_ONLY((zone->flush_generation == expected_generation),
			"logical zone %u flush generation %" PRIu64
			" should be %llu before increment",
			zone->zone_number, zone->flush_generation,
			expected_generation);

	zone->flush_generation++;
	zone->ios_in_flush_generation = 0;
	update_oldest_active_generation(zone);
}

/**********************************************************************/
SequenceNumber get_oldest_locked_generation(const struct logical_zone *zone)
{
	return (SequenceNumber) atomicLoad64(&zone->oldest_locked_generation);
}

/**********************************************************************/
int acquire_flush_generation_lock(struct data_vio *dataVIO)
{
	struct logical_zone *zone = dataVIO->logical.zone;
	assert_on_zone_thread(zone, __func__);
	if (!is_normal(&zone->state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	dataVIO->flushGeneration = zone->flush_generation;
	pushRingNode(&zone->write_vios, &dataVIO->writeNode);
	dataVIO->hasFlushGenerationLock = true;
	zone->ios_in_flush_generation++;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void
attempt_generation_complete_notification(struct vdo_completion *completion);

/**
 * Notify the flush that at least one generation no longer has active VIOs.
 * This callback is registered in attempt_generation_complete_notification().
 *
 * @param completion  The zone completion
 **/
static void notify_flusher(struct vdo_completion *completion)
{
	struct logical_zone *zone = as_logical_zone(completion);
	complete_flushes(zone->zones->vdo->flusher);
	launchCallback(completion, attempt_generation_complete_notification,
		       zone->thread_id);
}

/**
 * Notify the flusher if some generation no longer has active VIOs.
 *
 * @param completion  The zone completion
 **/
static void
attempt_generation_complete_notification(struct vdo_completion *completion)
{
	struct logical_zone *zone = as_logical_zone(completion);
	assert_on_zone_thread(zone, __func__);
	if (zone->oldest_active_generation <= zone->notification_generation) {
		zone->notifying = false;
		check_for_drain_complete(zone);
		return;
	}

	zone->notifying = true;
	zone->notification_generation = zone->oldest_active_generation;
	launchCallback(&zone->completion, notify_flusher,
		       get_flusher_thread_id(zone->zones->vdo->flusher));
}

/**********************************************************************/
void release_flush_generation_lock(struct data_vio *data_vio)
{
	struct logical_zone *zone = data_vio->logical.zone;
	assert_on_zone_thread(zone, __func__);
	if (isRingEmpty(&data_vio->writeNode)) {
		// This VIO never got a lock, either because it is a read, or
		// because we are in read-only mode.
		ASSERT_LOG_ONLY(!data_vio->hasFlushGenerationLock,
				"hasFlushGenerationLock false for VIO not on active list");
		return;
	}

	unspliceRingNode(&data_vio->writeNode);
	data_vio->hasFlushGenerationLock = false;
	ASSERT_LOG_ONLY(zone->oldest_active_generation
				<= data_vio->flushGeneration,
			"data_vio releasing lock on generation %" PRIu64
			" is not older than oldest active generation %llu",
			data_vio->flushGeneration, zone->oldest_active_generation);

	if (!update_oldest_active_generation(zone) || zone->notifying) {
		return;
	}

	attempt_generation_complete_notification(&zone->completion);
}

/**********************************************************************/
struct allocation_selector *get_allocation_selector(struct logical_zone *zone)
{
	return zone->selector;
}

/**********************************************************************/
void dump_logical_zone(const struct logical_zone *zone)
{
	logInfo("logical_zone %u", zone->zone_number);
	logInfo("  flush_generation=%llu oldest_active_generation=%" PRIu64
		" oldest_locked_generation=%" PRIu64
		" notification_generation=%" PRIu64
		" notifying=%s iosInCurrentGeneration=%llu",
		zone->flush_generation, zone->oldest_active_generation,
		relaxedLoad64(&zone->oldest_locked_generation),
		zone->notification_generation, boolToString(zone->notifying),
		zone->ios_in_flush_generation);
}
