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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logicalZone.c#59 $
 */

#include "logicalZone.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "actionManager.h"
#include "adminState.h"
#include "allocationSelector.h"
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
	zone_count_t zone_number;
	/** The thread id for this zone */
	thread_id_t thread_id;
	/** In progress operations keyed by LBN */
	struct int_map *lbn_operations;
	/** The logical to physical map */
	struct block_map_zone *block_map_zone;
	/** The current flush generation */
	sequence_number_t flush_generation;
	/**
	 * The oldest active generation in this zone. This is mutated only on
	 * the logical zone thread but is queried from the flusher thread.
	 **/
	sequence_number_t oldest_active_generation;
	/** The number of IOs in the current flush generation */
	block_count_t ios_in_flush_generation;
	/** The youngest generation of the current notification */
	sequence_number_t notification_generation;
	/** Whether a notification is in progress */
	bool notifying;
	/** The queue of active data write VIOs */
	struct list_head write_vios;
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
	zone_count_t zone_count;
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
	assert_vdo_completion_type(completion->type,
				   GENERATION_FLUSHED_COMPLETION);
	return container_of(completion, struct logical_zone, completion);
}

/**********************************************************************/
struct logical_zone *get_vdo_logical_zone(struct logical_zones *zones,
					  zone_count_t zone_number)
{
	return (zone_number < zones->zone_count) ? &zones->zones[zone_number]
		: NULL;
}

/**
 * Implements vdo_zone_thread_getter
 **/
static thread_id_t get_thread_id_for_zone(void *context,
					  zone_count_t zone_number)
{
	return get_vdo_logical_zone_thread_id(get_vdo_logical_zone(context,
								   zone_number));
}

/**
 * Initialize a logical zone.
 *
 * @param zones        The logical_zones to which this zone belongs
 * @param zone_number  The logical_zone's index
 **/
static int initialize_zone(struct logical_zones *zones,
			   zone_count_t zone_number)
{
	struct vdo *vdo = zones->vdo;
	struct logical_zone *zone = &zones->zones[zone_number];
	int result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0,
				  &zone->lbn_operations);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&zone->completion, vdo,
				  GENERATION_FLUSHED_COMPLETION);
	zone->zones = zones;
	zone->zone_number = zone_number;
	zone->thread_id = get_logical_zone_thread(get_thread_config(vdo),
					       	  zone_number);
	zone->block_map_zone = get_block_map_zone(vdo->block_map, zone_number);
	INIT_LIST_HEAD(&zone->write_vios);

	return make_vdo_allocation_selector(get_thread_config(vdo)->physical_zone_count,
					    zone->thread_id, &zone->selector);
}

/**********************************************************************/
int make_vdo_logical_zones(struct vdo *vdo, struct logical_zones **zones_ptr)
{
	struct logical_zones *zones;
	int result;
	zone_count_t zone;

	const struct thread_config *thread_config = get_thread_config(vdo);
	if (thread_config->logical_zone_count == 0) {
		return VDO_SUCCESS;
	}

	result = ALLOCATE_EXTENDED(struct logical_zones,
				   thread_config->logical_zone_count,
				   struct logical_zone, __func__, &zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	zones->vdo = vdo;
	zones->zone_count = thread_config->logical_zone_count;
	for (zone = 0; zone < thread_config->logical_zone_count; zone++) {
		result = initialize_zone(zones, zone);
		if (result != VDO_SUCCESS) {
			free_logical_zones(&zones);
			return result;
		}
	}

	result = make_vdo_action_manager(zones->zone_count,
					 get_thread_id_for_zone,
					 get_admin_thread(thread_config),
					 zones,
					 NULL,
					 vdo,
					 &zones->manager);
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
	zone_count_t index;
	struct logical_zones *zones = *zones_ptr;
	if (zones == NULL) {
		return;
	}

	FREE(FORGET(zones->manager));

	for (index = 0; index < zones->zone_count; index++) {
		struct logical_zone *zone = &zones->zones[index];
		FREE(FORGET(zone->selector));
		free_int_map(&zone->lbn_operations);
	}

	FREE(zones);
	*zones_ptr = NULL;
}

/**********************************************************************/
static inline void assert_on_zone_thread(struct logical_zone *zone,
					 const char *what)
{
	ASSERT_LOG_ONLY((get_callback_thread_id() == zone->thread_id),
			"%s() called on correct thread", what);
}

/**
 * Check whether this zone has drained.
 *
 * @param zone  The zone to check
 **/
static void check_for_drain_complete(struct logical_zone *zone)
{
	if (!is_vdo_state_draining(&zone->state) || zone->notifying
	    || !list_empty(&zone->write_vios)) {
		return;
	}

	finish_vdo_draining(&zone->state);
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct logical_zone,
					      state));
}

/**
 * Drain a logical zone.
 *
 * <p>Implements vdo_zone_action.
 **/
static void drain_logical_zone(void *context, zone_count_t zone_number,
			       struct vdo_completion *parent)
{
	struct logical_zone *zone = get_vdo_logical_zone(context, zone_number);
	start_vdo_draining(&zone->state,
			   get_current_vdo_manager_operation(zone->zones->manager),
			   parent, initiate_drain);
}

/**********************************************************************/
void drain_vdo_logical_zones(struct logical_zones *zones,
			     enum admin_state_code operation,
			     struct vdo_completion *parent)
{
	schedule_vdo_operation(zones->manager, operation, NULL,
			       drain_logical_zone, NULL, parent);
}

/**
 * Resume a logical zone.
 *
 * <p>Implements vdo_zone_action.
 **/
static void resume_logical_zone(void *context, zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct logical_zone *zone = get_vdo_logical_zone(context, zone_number);
	finish_vdo_completion(parent, resume_vdo_if_quiescent(&zone->state));
}

/**********************************************************************/
void resume_vdo_logical_zones(struct logical_zones *zones,
			      struct vdo_completion *parent)
{
	schedule_vdo_operation(zones->manager, ADMIN_STATE_RESUMING, NULL,
			       resume_logical_zone, NULL, parent);
}

/**********************************************************************/
thread_id_t get_vdo_logical_zone_thread_id(const struct logical_zone *zone)
{
	return zone->thread_id;
}

/**********************************************************************/
struct block_map_zone *
get_vdo_logical_zone_block_map(const struct logical_zone *zone)
{
	return zone->block_map_zone;
}

/**********************************************************************/
struct int_map *
get_vdo_logical_zone_lbn_lock_map(const struct logical_zone *zone)
{
	return zone->lbn_operations;
}

/**********************************************************************/
struct logical_zone *get_next_vdo_logical_zone(const struct logical_zone *zone)
{
	return get_vdo_logical_zone(zone->zones, zone->zone_number + 1);
}

/**
 * Update the oldest active generation.
 *
 * @param zone  The zone
 *
 * @return <code>true</code> if the oldest active generation has changed
 **/
static bool update_oldest_active_generation(struct logical_zone *zone)
{
	sequence_number_t oldest;
	if (list_empty(&zone->write_vios)) {
		oldest = zone->flush_generation;
	} else {
		struct data_vio *data_vio = list_entry(zone->write_vios.next,
						       struct data_vio,
						       write_entry);
		oldest = data_vio->flush_generation;
	}

	if (oldest == zone->oldest_active_generation) {
		return false;
	}

	WRITE_ONCE(zone->oldest_active_generation, oldest);
	return true;
}

/**********************************************************************/
void
increment_vdo_logical_zone_flush_generation(struct logical_zone *zone,
					    sequence_number_t expected_generation)
{
	assert_on_zone_thread(zone, __func__);
	ASSERT_LOG_ONLY((zone->flush_generation == expected_generation),
			"logical zone %u flush generation %llu should be %llu before increment",
			zone->zone_number, zone->flush_generation,
			expected_generation);

	zone->flush_generation++;
	zone->ios_in_flush_generation = 0;
	update_oldest_active_generation(zone);
}

/**********************************************************************/
sequence_number_t
get_vdo_logical_zone_oldest_locked_generation(const struct logical_zone *zone)
{
	return READ_ONCE(zone->oldest_active_generation);
}

/**********************************************************************/
int acquire_vdo_flush_generation_lock(struct data_vio *data_vio)
{
	struct logical_zone *zone = data_vio->logical.zone;
	assert_on_zone_thread(zone, __func__);
	if (!is_vdo_state_normal(&zone->state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	data_vio->flush_generation = zone->flush_generation;
	list_move_tail(&data_vio->write_entry, &zone->write_vios);
	data_vio->has_flush_generation_lock = true;
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
	complete_vdo_flushes(zone->zones->vdo->flusher);
	launch_vdo_completion_callback(completion,
				       attempt_generation_complete_notification,
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
	launch_vdo_completion_callback(&zone->completion, notify_flusher,
				       get_vdo_flusher_thread_id(zone->zones->vdo->flusher));
}

/**********************************************************************/
void release_vdo_flush_generation_lock(struct data_vio *data_vio)
{
	struct logical_zone *zone = data_vio->logical.zone;
	assert_on_zone_thread(zone, __func__);
	if (list_empty(&data_vio->write_entry)) {
		// This VIO never got a lock, either because it is a read, or
		// because we are in read-only mode.
		ASSERT_LOG_ONLY(!data_vio->has_flush_generation_lock,
				"has_flush_generation_lock false for VIO not on active list");
		return;
	}

	list_del_init(&data_vio->write_entry);
	data_vio->has_flush_generation_lock = false;
	ASSERT_LOG_ONLY(zone->oldest_active_generation
				<= data_vio->flush_generation,
			"data_vio releasing lock on generation %llu is not older than oldest active generation %llu",
			data_vio->flush_generation,
			zone->oldest_active_generation);

	if (!update_oldest_active_generation(zone) || zone->notifying) {
		return;
	}

	attempt_generation_complete_notification(&zone->completion);
}

/**********************************************************************/
struct allocation_selector *
get_vdo_logical_zone_allocation_selector(struct logical_zone *zone)
{
	return zone->selector;
}

/**********************************************************************/
void dump_vdo_logical_zone(const struct logical_zone *zone)
{
	log_info("logical_zone %u", zone->zone_number);
	log_info("  flush_generation=%llu oldest_active_generation=%llu notification_generation=%llu notifying=%s ios_in_flush_generation=%llu",
		 READ_ONCE(zone->flush_generation),
		 READ_ONCE(zone->oldest_active_generation),
		 READ_ONCE(zone->notification_generation),
		 bool_to_string(READ_ONCE(zone->notifying)),
		 READ_ONCE(zone->ios_in_flush_generation));
}
