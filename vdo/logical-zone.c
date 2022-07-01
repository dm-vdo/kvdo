// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "logical-zone.h"

#include "logger.h"
#include "memory-alloc.h"
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
 * as_logical_zone() - Convert a generic vdo_completion to a logical_zone.
 * @completion: The completion to convert.
 *
 * Return: The completion as a logical_zone.
 */
static struct logical_zone *as_logical_zone(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_GENERATION_FLUSHED_COMPLETION);
	return container_of(completion, struct logical_zone, completion);
}

/**
 * get_thread_id_for_zone() - Implements vdo_zone_thread_getter.
 */
static thread_id_t get_thread_id_for_zone(void *context,
					  zone_count_t zone_number)
{
	struct logical_zones *zones = context;

	return zones->zones[zone_number].thread_id;
}

/**
 * initialize_zone() - Initialize a logical zone.
 * @zones: The logical_zones to which this zone belongs.
 * @zone_number: The logical_zone's index.
 */
static int initialize_zone(struct logical_zones *zones,
			   zone_count_t zone_number)
{
	int result;
	struct vdo *vdo = zones->vdo;
	struct logical_zone *zone = &zones->zones[zone_number];
	zone_count_t physical_zone_count =
		vdo->thread_config->physical_zone_count;

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0, &zone->lbn_operations);
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
	zone->block_map_zone = &vdo->block_map->zones[zone_number];
	INIT_LIST_HEAD(&zone->write_vios);
	vdo_set_admin_state_code(&zone->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	result = vdo_make_allocation_selector(physical_zone_count,
					      zone->thread_id,
					      &zone->selector);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_make_default_thread(vdo, zone->thread_id);
}

/**
 * vdo_make_logical_zones() - Create a set of logical zones.
 * @vdo: The vdo to which the zones will belong.
 * @zones_ptr: A pointer to hold the new zones.
 *
 * Return: VDO_SUCCESS or an error code.
 */
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
 * vdo_free_logical_zones() - Free a set of logical zones.
 * @zones: The set of zones to free.
 */
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
 * check_for_drain_complete() - Check whether this zone has drained.
 * @zone: The zone to check.
 */
static void check_for_drain_complete(struct logical_zone *zone)
{
	if (!vdo_is_state_draining(&zone->state) || zone->notifying
	    || !list_empty(&zone->write_vios)) {
		return;
	}

	vdo_finish_draining(&zone->state);
}

/**
 * initiate_drain() - Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct logical_zone,
					      state));
}

/**
 * drain_logical_zone() - Drain a logical zone.
 *
 * Implements vdo_zone_action.
 */
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

void vdo_drain_logical_zones(struct logical_zones *zones,
			     const struct admin_state_code *operation,
			     struct vdo_completion *parent)
{
	vdo_schedule_operation(zones->manager, operation, NULL,
			       drain_logical_zone, NULL, parent);
}

/**
 * resume_logical_zone() - Resume a logical zone.
 *
 * Implements vdo_zone_action.
 */
static void resume_logical_zone(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct logical_zone *zone
		= &(((struct logical_zones *) context)->zones[zone_number]);

	vdo_finish_completion(parent, vdo_resume_if_quiescent(&zone->state));
}

/**
 * vdo_resume_logical_zones() - Resume a set of logical zones.
 * @zones: The logical zones to resume.
 * @parent: The object to notify when the zones have resumed.
 */
void vdo_resume_logical_zones(struct logical_zones *zones,
			      struct vdo_completion *parent)
{
	vdo_schedule_operation(zones->manager, VDO_ADMIN_STATE_RESUMING, NULL,
			       resume_logical_zone, NULL, parent);
}

/**
 * update_oldest_active_generation() - Update the oldest active generation.
 * @zone: The zone.
 *
 * Return: true if the oldest active generation has changed.
 */
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
 * vdo_increment_logical_zone_flush_generation() - Increment the flush
 *                                                 generation in a logical zone.
 * @zone: The logical zone.
 * @expected_generation: The expected value of the flush generation
 *                       before the increment.
 */
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
 * vdo_acquire_flush_generation_lock() - Acquire the shared lock on a flush
 *                                       generation by a write data_vio.
 * @data_vio: The data_vio.
 *
 * Return: VDO_SUCCESS or an error code.
 */
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
 * notify_flusher() - Notify the flush that at least one generation no longer
 *                    has active VIOs.
 * @completion: The zone completion.
 *
 * This callback is registered in attempt_generation_complete_notification().
 */
static void notify_flusher(struct vdo_completion *completion)
{
	struct logical_zone *zone = as_logical_zone(completion);

	vdo_complete_flushes(zone->zones->vdo->flusher);
	vdo_launch_completion_callback(completion,
				       attempt_generation_complete_notification,
				       zone->thread_id);
}

/**
 * void attempt_generation_complete_notification() - Notify the flusher if
 *                                                   some generation no longer
 *                                                   has active VIOs.
 * @completion: The zone completion.
 */
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
 * vdo_release_flush_generation_lock() - Release the shared lock on a flush
 *                                       generation held by a write data_vio.
 * @data_vio: The data_vio whose lock is to be released.
 *
 * If there are pending flushes, and this data_vio completes the oldest
 * generation active in this zone, an attempt will be made to finish any
 * flushes which may now be complete.
 */
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
 * vdo_dump_logical_zone() - Dump information about a logical zone to the log
 *                           for debugging.
 * @zone: The zone to dump
 *
 * Context: the information is dumped in a thread-unsafe fashion.
 *
 */
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
