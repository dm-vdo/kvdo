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

#include "logical-zone.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "action-manager.h"
#include "admin-state.h"
#include "allocation-selector.h"
#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "data-vio.h"
#include "flush.h"
#include "int-map.h"
#include "vdo.h"

/**
 * Convert a generic vdo_completion to a logical_zone.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a logical_zone
 **/
static struct logical_zone *as_logical_zone(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_GENERATION_FLUSHED_COMPLETION);
	return container_of(completion, struct logical_zone, completion);
}

/**
 * Implements vdo_zone_thread_getter
 **/
static thread_id_t get_thread_id_for_zone(void *context,
					  zone_count_t zone_number)
{
	struct logical_zones *zones = context;

	return zones->zones[zone_number].thread_id;
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

	if (zone_number < vdo->thread_config->logical_zone_count - 1) {
		zone->next = &zones->zones[zone_number + 1];
	}

	vdo_initialize_completion(&zone->completion, vdo,
				  VDO_GENERATION_FLUSHED_COMPLETION);
	zone->zones = zones;
	zone->zone_number = zone_number;
	zone->thread_id = vdo_get_logical_zone_thread(vdo->thread_config,
						      zone_number);
	zone->block_map_zone = vdo_get_block_map_zone(vdo->block_map,
						      zone_number);
	INIT_LIST_HEAD(&zone->write_vios);
	vdo_set_admin_state_code(&zone->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	return vdo_make_allocation_selector(vdo->thread_config->physical_zone_count,
					    zone->thread_id, &zone->selector);
}

/**
 * Create a set of logical zones.
 *
 * @param [in]  vdo        The vdo to which the zones will belong
 * @param [out] zones_ptr  A pointer to hold the new zones
 *
 * @return VDO_SUCCESS or an error code
 **/
int vdo_make_logical_zones(struct vdo *vdo, struct logical_zones **zones_ptr)
{
	struct logical_zones *zones;
	int result;
	zone_count_t zone;
	zone_count_t zone_count = vdo->thread_config->logical_zone_count;

	if (zone_count == 0) {
		return VDO_SUCCESS;
	}

	result = UDS_ALLOCATE_EXTENDED(struct logical_zones, zone_count,
				       struct logical_zone, __func__, &zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	zones->vdo = vdo;
	zones->zone_count = zone_count;
	for (zone = 0; zone < zone_count; zone++) {
		result = initialize_zone(zones, zone);
		if (result != VDO_SUCCESS) {
			vdo_free_logical_zones(zones);
			return result;
		}
	}

	result = vdo_make_action_manager(zones->zone_count,
					 get_thread_id_for_zone,
					 vdo->thread_config->admin_thread,
					 zones,
					 NULL,
					 vdo,
					 &zones->manager);
	if (result != VDO_SUCCESS) {
		vdo_free_logical_zones(zones);
		return result;
	}

	*zones_ptr = zones;
	return VDO_SUCCESS;
}

/**
 * Free a set of logical zones.
 *
 * @param zones The set of zones to free
 **/
void vdo_free_logical_zones(struct logical_zones *zones)
{
	zone_count_t index;

	if (zones == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(zones->manager));

	for (index = 0; index < zones->zone_count; index++) {
		struct logical_zone *zone = &zones->zones[index];

		UDS_FREE(UDS_FORGET(zone->selector));
		free_int_map(UDS_FORGET(zone->lbn_operations));
	}

	UDS_FREE(zones);
}

static inline void assert_on_zone_thread(struct logical_zone *zone,
					 const char *what)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == zone->thread_id),
			"%s() called on correct thread", what);
}

/**
 * Check whether this zone has drained.
 *
 * @param zone  The zone to check
 **/
static void check_for_drain_complete(struct logical_zone *zone)
{
	if (!vdo_is_state_draining(&zone->state) || zone->notifying
	    || !list_empty(&zone->write_vios)) {
		return;
	}

	vdo_finish_draining(&zone->state);
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
static void drain_logical_zone(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent)
{
	struct logical_zones *zones = context;

	vdo_start_draining(&zones->zones[zone_number].state,
			   vdo_get_current_manager_operation(zones->manager),
			   parent,
			   initiate_drain);
}

/**********************************************************************/
void vdo_drain_logical_zones(struct logical_zones *zones,
			     const struct admin_state_code *operation,
			     struct vdo_completion *parent)
{
	vdo_schedule_operation(zones->manager, operation, NULL,
			       drain_logical_zone, NULL, parent);
}

/**
 * Resume a logical zone.
 *
 * <p>Implements vdo_zone_action.
 **/
static void resume_logical_zone(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct logical_zone *zone
		= &(((struct logical_zones *) context)->zones[zone_number]);

	vdo_finish_completion(parent, vdo_resume_if_quiescent(&zone->state));
}

/**
 * Resume a set of logical zones.
 *
 * @param zones   The logical zones to resume
 * @param parent  The object to notify when the zones have resumed
 **/
void vdo_resume_logical_zones(struct logical_zones *zones,
			      struct vdo_completion *parent)
{
	vdo_schedule_operation(zones->manager, VDO_ADMIN_STATE_RESUMING, NULL,
			       resume_logical_zone, NULL, parent);
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

/**
 * Increment the flush generation in a logical zone.
 *
 * @param zone                 The logical zone
 * @param expected_generation  The expected value of the flush generation
 *                             before the increment
 **/
void
vdo_increment_logical_zone_flush_generation(struct logical_zone *zone,
					    sequence_number_t expected_generation)
{
	assert_on_zone_thread(zone, __func__);
	ASSERT_LOG_ONLY((zone->flush_generation == expected_generation),
			"logical zone %u flush generation %llu should be %llu before increment",
			zone->zone_number,
			(unsigned long long) zone->flush_generation,
			(unsigned long long) expected_generation);

	zone->flush_generation++;
	zone->ios_in_flush_generation = 0;
	update_oldest_active_generation(zone);
}

/**
 * Acquire the shared lock on a flush generation by a write data_vio.
 *
 * @param data_vio   The data_vio
 *
 * @return VDO_SUCCESS or an error code
 **/
int vdo_acquire_flush_generation_lock(struct data_vio *data_vio)
{
	struct logical_zone *zone = data_vio->logical.zone;

	assert_on_zone_thread(zone, __func__);
	if (!vdo_is_state_normal(&zone->state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	data_vio->flush_generation = zone->flush_generation;
	list_move_tail(&data_vio->write_entry, &zone->write_vios);
	data_vio->has_flush_generation_lock = true;
	zone->ios_in_flush_generation++;
	return VDO_SUCCESS;
}

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

	vdo_complete_flushes(zone->zones->vdo->flusher);
	vdo_launch_completion_callback(completion,
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
	vdo_launch_completion_callback(&zone->completion, notify_flusher,
				       vdo_get_flusher_thread_id(zone->zones->vdo->flusher));
}

/**
 * Release the shared lock on a flush generation held by a write data_vio. If
 * there are pending flushes, and this data_vio completes the oldest generation
 * active in this zone, an attempt will be made to finish any flushes which may
 * now be complete.
 *
 * @param data_vio  The data_vio whose lock is to be released
 **/
void vdo_release_flush_generation_lock(struct data_vio *data_vio)
{
	struct logical_zone *zone = data_vio->logical.zone;

	assert_on_zone_thread(zone, __func__);
	if (list_empty(&data_vio->write_entry)) {
		/*
		 * This VIO never got a lock, either because it is a read, or
		 * because we are in read-only mode.
		 */
		ASSERT_LOG_ONLY(!data_vio->has_flush_generation_lock,
				"has_flush_generation_lock false for VIO not on active list");
		return;
	}

	list_del_init(&data_vio->write_entry);
	data_vio->has_flush_generation_lock = false;
	ASSERT_LOG_ONLY(zone->oldest_active_generation
				<= data_vio->flush_generation,
			"data_vio releasing lock on generation %llu is not older than oldest active generation %llu",
			(unsigned long long) data_vio->flush_generation,
			(unsigned long long) zone->oldest_active_generation);

	if (!update_oldest_active_generation(zone) || zone->notifying) {
		return;
	}

	attempt_generation_complete_notification(&zone->completion);
}

/**
 * Dump information about a logical zone to the log for debugging, in a
 * thread-unsafe fashion.
 *
 * @param zone   The zone to dump
 **/
void vdo_dump_logical_zone(const struct logical_zone *zone)
{
	uds_log_info("logical_zone %u", zone->zone_number);
	uds_log_info("  flush_generation=%llu oldest_active_generation=%llu notification_generation=%llu notifying=%s ios_in_flush_generation=%llu",
		     (unsigned long long) READ_ONCE(zone->flush_generation),
		     (unsigned long long) READ_ONCE(zone->oldest_active_generation),
		     (unsigned long long) READ_ONCE(zone->notification_generation),
		     uds_bool_to_string(READ_ONCE(zone->notifying)),
		     (unsigned long long) READ_ONCE(zone->ios_in_flush_generation));
}
