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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoLoad.c#51 $
 */

#include "vdoLoad.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "dedupeIndex.h"
#include "deviceConfig.h"
#include "hashZone.h"
#include "header.h"
#include "logicalZone.h"
#include "physicalZone.h"
#include "poolSysfs.h"
#include "readOnlyRebuild.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "superBlockCodec.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoRecovery.h"

enum {
	LOAD_PHASE_START = 0,
	LOAD_PHASE_STATS,
	LOAD_PHASE_LOAD_DEPOT,
	LOAD_PHASE_MAKE_DIRTY,
	LOAD_PHASE_PREPARE_TO_ALLOCATE,
	LOAD_PHASE_SCRUB_SLABS,
	LOAD_PHASE_DATA_REDUCTION,
	LOAD_PHASE_FINISHED,
	LOAD_PHASE_DRAIN_JOURNAL,
	LOAD_PHASE_WAIT_FOR_READ_ONLY,
};

static const char *LOAD_PHASE_NAMES[] = {
	"LOAD_PHASE_START",
	"LOAD_PHASE_STATS",
	"LOAD_PHASE_LOAD_DEPOT",
	"LOAD_PHASE_MAKE_DIRTY",
	"LOAD_PHASE_PREPARE_TO_ALLOCATE",
	"LOAD_PHASE_SCRUB_SLABS",
	"LOAD_PHASE_DATA_REDUCTION",
	"LOAD_PHASE_FINISHED",
	"LOAD_PHASE_DRAIN_JOURNAL",
	"LOAD_PHASE_WAIT_FOR_READ_ONLY",
};


/**
 * Implements vdo_thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		get_vdo_thread_config(admin_completion->vdo);
	switch (admin_completion->phase) {
	case LOAD_PHASE_DRAIN_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
	}
}

/**
 * Extract the vdo from an admin_completion, checking that the current
 * operation is a load.
 *
 * @param completion  The admin_completion's sub-task completion
 *
 * @return The vdo
 **/
static inline struct vdo *
vdo_from_load_sub_task(struct vdo_completion *completion)
{
	return vdo_from_admin_sub_task(completion, VDO_ADMIN_OPERATION_LOAD);
}

/**
 * Determine how the slab depot was loaded.
 *
 * @param vdo  The vdo
 *
 * @return How the depot was loaded
 **/
static enum slab_depot_load_type get_load_type(struct vdo *vdo)
{
	if (requires_read_only_rebuild(vdo)) {
		return VDO_SLAB_DEPOT_REBUILD_LOAD;
	}

	if (requires_recovery(vdo)) {
		return VDO_SLAB_DEPOT_RECOVERY_LOAD;
	}

	return VDO_SLAB_DEPOT_NORMAL_LOAD;
}

/**
 * Initialize the vdo sysfs directory.
 *
 * @param vdo     The vdo being initialized
 *
 * @return VDO_SUCCESS or an error code
 **/
static int initialize_vdo_kobjects(struct vdo *vdo)
{
	int result;
	struct dm_target *target = vdo->device_config->owning_target;
	struct mapped_device *md = dm_table_get_md(target->table);

	kobject_init(&vdo->vdo_directory, &vdo_directory_type);
	vdo->sysfs_added = true;
	result = kobject_add(&vdo->vdo_directory,
			     &disk_to_dev(dm_disk(md))->kobj,
			     "vdo");
	if (result != 0) {
		return VDO_CANT_ADD_SYSFS_NODE;
	}

	result = add_vdo_dedupe_index_sysfs(vdo->dedupe_index,
					    &vdo->vdo_directory);
	if (result != 0) {
		return VDO_CANT_ADD_SYSFS_NODE;
	}

	return add_vdo_sysfs_stats_dir(vdo);
}

/**
 * Callback to do the destructive parts of loading a VDO.
 *
 * @param completion  The sub-task completion
 **/
static void load_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_LOAD);
	assert_vdo_admin_phase_thread(admin_completion,
				      __func__,
				      LOAD_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case LOAD_PHASE_START:
		if (!start_vdo_operation_with_waiter(&vdo->admin_state,
						     VDO_ADMIN_STATE_LOADING,
						     &admin_completion->completion,
						     NULL)) {
			return;
		}

		// Prepare the recovery journal for new entries.
		open_vdo_recovery_journal(vdo->recovery_journal,
					  vdo->depot,
					  vdo->block_map);
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier,
					       reset_vdo_admin_sub_task(completion));
		return;

	case LOAD_PHASE_STATS:
		finish_vdo_completion(reset_vdo_admin_sub_task(completion),
				      initialize_vdo_kobjects(vdo));
		return;

	case LOAD_PHASE_LOAD_DEPOT:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			/*
			 * In read-only mode we don't use the allocator and it
			 * may not even be readable, so don't bother trying to
			 * load it.
			 */
			set_vdo_operation_result(&vdo->admin_state,
						 VDO_READ_ONLY);
			break;
		}

		reset_vdo_admin_sub_task(completion);
		if (requires_read_only_rebuild(vdo)) {
			launch_vdo_rebuild(vdo, completion);
			return;
		}

		if (requires_rebuild(vdo)) {
			vdo_launch_recovery(vdo, completion);
			return;
		}

		load_vdo_slab_depot(vdo->depot,
				    (vdo_was_new(vdo)
				     ? VDO_ADMIN_STATE_FORMATTING
				     : VDO_ADMIN_STATE_LOADING),
				    completion,
				    NULL);
		return;

	case LOAD_PHASE_MAKE_DIRTY:
		set_vdo_state(vdo, VDO_DIRTY);
		save_vdo_components(vdo, reset_vdo_admin_sub_task(completion));
		return;

	case LOAD_PHASE_PREPARE_TO_ALLOCATE:
		initialize_vdo_block_map_from_journal(vdo->block_map,
						      vdo->recovery_journal);
		prepare_vdo_slab_depot_to_allocate(vdo->depot,
						   get_load_type(vdo),
						   reset_vdo_admin_sub_task(completion));
		return;

	case LOAD_PHASE_SCRUB_SLABS:
		if (requires_recovery(vdo)) {
			enter_recovery_mode(vdo);
		}

		vdo_scrub_all_unrecovered_slabs(vdo->depot,
						reset_vdo_admin_sub_task(completion));
		return;

	case LOAD_PHASE_DATA_REDUCTION:
		WRITE_ONCE(vdo->compressing, vdo->device_config->compression);
		fallthrough;

	case LOAD_PHASE_FINISHED:
		break;

	case LOAD_PHASE_DRAIN_JOURNAL:
		drain_vdo_recovery_journal(vdo->recovery_journal,
					   VDO_ADMIN_STATE_SAVING,
					   reset_vdo_admin_sub_task(completion));
		return;

	case LOAD_PHASE_WAIT_FOR_READ_ONLY:
		admin_completion->phase = LOAD_PHASE_FINISHED;
		reset_vdo_admin_sub_task(completion);
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
							   completion);
		return;

	default:
		set_vdo_completion_result(reset_vdo_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	finish_vdo_operation(&vdo->admin_state, completion->result);
}

/**
 * Handle an error during the load operation. If at all possible, bring the vdo
 * online in read-only mode. This handler is registered in load_vdo().
 *
 * @param completion  The sub-task completion
 **/
static void handle_load_error(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_LOAD);

	if (requires_read_only_rebuild(vdo)
	    && (admin_completion->phase == LOAD_PHASE_MAKE_DIRTY)) {
		uds_log_error_strerror(completion->result, "aborting load");

		// Preserve the error.
		set_vdo_operation_result(&vdo->admin_state,
					 completion->result);
		admin_completion->phase = LOAD_PHASE_DRAIN_JOURNAL;
		load_callback(UDS_FORGET(completion));
		return;
	}

	uds_log_error_strerror(completion->result,
			       "Entering read-only mode due to load error");
	admin_completion->phase = LOAD_PHASE_WAIT_FOR_READ_ONLY;
	vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
	set_vdo_operation_result(&vdo->admin_state, VDO_READ_ONLY);
	load_callback(completion);
}

/**********************************************************************/
int load_vdo(struct vdo *vdo)
{
	return perform_vdo_admin_operation(vdo,
					   VDO_ADMIN_OPERATION_LOAD,
					   get_thread_id_for_phase,
					   load_callback,
					   handle_load_error);
}

/**
 * Decode the VDO state from the super block and validate that it is correct.
 * On error from this method, the component states must be destroyed
 * explicitly. If this method returns successfully, the component states must
 * not be destroyed.
 *
 * @param vdo  The vdo being loaded
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check decode_from_super_block(struct vdo *vdo)
{
	block_count_t block_count;
	struct super_block_codec *codec
		= get_vdo_super_block_codec(vdo->super_block);
	int result = decode_vdo_component_states(codec->component_buffer,
						 vdo->geometry.release_version,
						 &vdo->states);
	if (result != VDO_SUCCESS) {
		return result;
	}

	set_vdo_state(vdo, vdo->states.vdo.state);
	vdo->load_state = vdo->states.vdo.state;

	block_count = vdo->device_config->physical_blocks;
	result = validate_vdo_component_states(&vdo->states,
					       vdo->geometry.nonce,
					       block_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return decode_vdo_layout(vdo->states.layout, &vdo->layout);
}

/**
 * Decode the component data portion of a super block and fill in the
 * corresponding portions of the vdo being loaded. This will also allocate the
 * recovery journal and slab depot. If this method is called with an
 * asynchronous layer (i.e. a thread config which specifies at least one base
 * thread), the block map and packer will be constructed as well.
 *
 * @param vdo  The vdo being loaded
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check decode_vdo(struct vdo *vdo)
{
	block_count_t maximum_age, journal_length;
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);
	zone_count_t zone;
	int result = decode_from_super_block(vdo);
	if (result != VDO_SUCCESS) {
		destroy_vdo_component_states(&vdo->states);
		return result;
	}

	maximum_age = get_vdo_configured_block_map_maximum_age(vdo);
	journal_length =
		get_vdo_recovery_journal_length(vdo->states.vdo.config.recovery_journal_size);
	if ((maximum_age > (journal_length / 2)) || (maximum_age < 1)) {
		return VDO_BAD_CONFIGURATION;
	}

	result = make_vdo_read_only_notifier(in_read_only_mode(vdo),
					     thread_config,
					     vdo,
					     &vdo->read_only_notifier);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = enable_read_only_entry(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_recovery_journal(vdo->states.recovery_journal,
					     vdo->states.vdo.nonce,
					     vdo,
					     get_vdo_partition(vdo->layout,
							       RECOVERY_JOURNAL_PARTITION),
					     vdo->states.vdo.complete_recoveries,
					     vdo->states.vdo.config.recovery_journal_size,
					     VDO_RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
					     vdo->read_only_notifier,
					     thread_config,
					     &vdo->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_slab_depot(vdo->states.slab_depot,
				       vdo,
				       get_vdo_partition(vdo->layout,
							 SLAB_SUMMARY_PARTITION),
				       &vdo->depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_block_map(vdo->states.block_map,
				      vdo->states.vdo.config.logical_blocks,
				      thread_config,
				      vdo,
				      vdo->read_only_notifier,
				      vdo->recovery_journal,
				      vdo->states.vdo.nonce,
				      get_vdo_configured_cache_size(vdo),
				      maximum_age,
				      &vdo->block_map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_vdo_flusher(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(thread_config->hash_zone_count,
			      struct hash_zone *,
			      __func__,
			      &vdo->hash_zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
		result = make_vdo_hash_zone(vdo, zone, &vdo->hash_zones[zone]);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	result = make_vdo_logical_zones(vdo, &vdo->logical_zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(thread_config->physical_zone_count,
			      struct physical_zone *,
			      __func__,
			      &vdo->physical_zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (zone = 0; zone < thread_config->physical_zone_count; zone++) {
		result = make_vdo_physical_zone(vdo, zone,
						&vdo->physical_zones[zone]);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return make_vdo_packer(vdo,
			       DEFAULT_PACKER_INPUT_BINS,
			       DEFAULT_PACKER_OUTPUT_BINS,
			       &vdo->packer);
}

/**
 * Callback to finish the load operation.
 *
 * @param completion  The admin_completion's sub-task completion
 **/
static void finish_operation_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	finish_vdo_operation(&vdo->admin_state, completion->result);
}

/**
 * Load the components of a VDO. This is the super block load callback
 * set by load_callback().
 *
 * @param completion The sub-task completion
 **/
static void load_vdo_components(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	prepare_vdo_admin_sub_task(vdo,
				   finish_operation_callback,
				   finish_operation_callback);
	finish_vdo_completion(completion, decode_vdo(vdo));
}

/**
 * Callback to initiate a pre-load, registered in prepare_to_load_vdo().
 *
 * @param completion  The sub-task completion
 **/
static void pre_load_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_load_sub_task(completion);

	ASSERT_LOG_ONLY((admin_completion->type == VDO_ADMIN_OPERATION_LOAD),
			"unexpected admin operation type %u when preloading",
			admin_completion->type);
	assert_on_admin_thread(vdo, __func__);
	if (!start_vdo_operation_with_waiter(&vdo->admin_state,
					     VDO_ADMIN_STATE_PRE_LOADING,
					     &admin_completion->completion,
					     NULL)) {
		return;
	}

	prepare_vdo_admin_sub_task(vdo,
				   load_vdo_components,
				   finish_operation_callback);
	load_vdo_super_block(vdo, completion, get_vdo_first_block_offset(vdo),
			     &vdo->super_block);
}

/**********************************************************************/
int prepare_to_load_vdo(struct vdo *vdo)
{
	return perform_vdo_admin_operation(vdo,
					   VDO_ADMIN_OPERATION_LOAD,
					   NULL,
					   pre_load_callback,
					   pre_load_callback);
}
