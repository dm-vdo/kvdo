// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-load.h"

#include "logger.h"
#include "memory-alloc.h"

#include "admin-completion.h"
#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "dedupe.h"
#include "device-config.h"
#include "header.h"
#include "kernel-types.h"
#include "logical-zone.h"
#include "physical-zone.h"
#include "pool-sysfs.h"
#include "read-only-rebuild.h"
#include "recovery-journal.h"
#include "release-versions.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "super-block-codec.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-recovery.h"
#include "vdo-suspend.h"

enum {
	LOAD_PHASE_START,
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
 * get_thread_id_for_phase() - Implements vdo_thread_id_getter_for_phase.
 */
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		admin_completion->vdo->thread_config;
	switch (admin_completion->phase) {
	case LOAD_PHASE_DRAIN_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
	}
}

/**
 * vdo_from_load_sub_task() - Extract the vdo from an admin_completion,
 *                            checking that the current operation is a load.
 * @completion: The admin_completion's sub-task completion.
 *
 * Return: The vdo.
 */
static inline struct vdo *
vdo_from_load_sub_task(struct vdo_completion *completion)
{
	return vdo_from_admin_sub_task(completion, VDO_ADMIN_OPERATION_LOAD);
}

/**
 * was_new() - Check whether the vdo was new when it was loaded.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo was new.
 */
static bool was_new(const struct vdo *vdo)
{
	return (vdo->load_state == VDO_NEW);
}

/**
 * requires_read_only_rebuild() - Check whether the vdo requires a read-only
 *                                mode rebuild.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo requires a read-only rebuild.
 */
static bool __must_check requires_read_only_rebuild(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_FORCE_REBUILD) ||
		(vdo->load_state == VDO_REBUILD_FOR_UPGRADE));
}

/**
 * requires_recovery() - Check whether a vdo should enter recovery mode.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo requires recovery.
 */
static bool __must_check requires_recovery(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_DIRTY) ||
		(vdo->load_state == VDO_REPLAYING) ||
		(vdo->load_state == VDO_RECOVERING));
}

/**
 * requires_rebuild() - Check whether a vdo requires rebuilding.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo must be rebuilt.
 */
static bool __must_check requires_rebuild(const struct vdo *vdo)
{
	switch (vdo_get_state(vdo)) {
	case VDO_DIRTY:
	case VDO_FORCE_REBUILD:
	case VDO_REPLAYING:
	case VDO_REBUILD_FOR_UPGRADE:
		return true;

	default:
		return false;
	}
}

/**
 * get_load_type() - Determine how the slab depot was loaded.
 * @vdo: The vdo.
 *
 * Return: How the depot was loaded.
 */
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
 * vdo_initialize_kobjects() - Initialize the vdo sysfs directory.
 * @vdo: The vdo being initialized.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int vdo_initialize_kobjects(struct vdo *vdo)
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

	result = vdo_add_dedupe_index_sysfs(vdo->hash_zones);
	if (result != 0) {
		return VDO_CANT_ADD_SYSFS_NODE;
	}

	return vdo_add_sysfs_stats_dir(vdo);
}

/**
 * load_callback() - Callback to do the destructive parts of loading a VDO.
 * @completion: The sub-task completion.
 */
static void load_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_load_sub_task(completion);

	vdo_assert_admin_phase_thread(admin_completion,
				      __func__,
				      LOAD_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case LOAD_PHASE_START:
		if (!vdo_start_operation_with_waiter(&vdo->admin_state,
						     VDO_ADMIN_STATE_LOADING,
						     &admin_completion->completion,
						     NULL)) {
			return;
		}

		/* Prepare the recovery journal for new entries. */
		vdo_open_recovery_journal(vdo->recovery_journal,
					  vdo->depot,
					  vdo->block_map);
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier,
					       vdo_reset_admin_sub_task(completion));
		return;

	case LOAD_PHASE_STATS:
		vdo_finish_completion(vdo_reset_admin_sub_task(completion),
				      vdo_initialize_kobjects(vdo));
		return;

	case LOAD_PHASE_LOAD_DEPOT:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			/*
			 * In read-only mode we don't use the allocator and it
			 * may not even be readable, so don't bother trying to
			 * load it.
			 */
			vdo_set_operation_result(&vdo->admin_state,
						 VDO_READ_ONLY);
			break;
		}

		vdo_reset_admin_sub_task(completion);
		if (requires_read_only_rebuild(vdo)) {
			vdo_launch_rebuild(vdo, completion);
			return;
		}

		if (requires_rebuild(vdo)) {
			vdo_launch_recovery(vdo, completion);
			return;
		}

		vdo_load_slab_depot(vdo->depot,
				    (was_new(vdo)
				     ? VDO_ADMIN_STATE_FORMATTING
				     : VDO_ADMIN_STATE_LOADING),
				    completion,
				    NULL);
		return;

	case LOAD_PHASE_MAKE_DIRTY:
		vdo_set_state(vdo, VDO_DIRTY);
		vdo_save_components(vdo, vdo_reset_admin_sub_task(completion));
		return;

	case LOAD_PHASE_PREPARE_TO_ALLOCATE:
		vdo_initialize_block_map_from_journal(vdo->block_map,
						      vdo->recovery_journal);
		vdo_prepare_slab_depot_to_allocate(vdo->depot,
						   get_load_type(vdo),
						   vdo_reset_admin_sub_task(completion));
		return;

	case LOAD_PHASE_SCRUB_SLABS:
		if (requires_recovery(vdo)) {
			vdo_enter_recovery_mode(vdo);
		}

		vdo_scrub_all_unrecovered_slabs(vdo->depot,
						vdo_reset_admin_sub_task(completion));
		return;

	case LOAD_PHASE_DATA_REDUCTION:
		WRITE_ONCE(vdo->compressing, vdo->device_config->compression);
		if (vdo->device_config->deduplication) {
			/*
			 * Don't try to load or rebuild the index first (and
			 * log scary error messages) if this is known to be a
			 * newly-formatted volume.
			 */
			vdo_start_dedupe_index(vdo->hash_zones, was_new(vdo));
		}

		vdo->allocations_allowed = false;
		fallthrough;

	case LOAD_PHASE_FINISHED:
		break;

	case LOAD_PHASE_DRAIN_JOURNAL:
		vdo_drain_recovery_journal(vdo->recovery_journal,
					   VDO_ADMIN_STATE_SAVING,
					   vdo_reset_admin_sub_task(completion));
		return;

	case LOAD_PHASE_WAIT_FOR_READ_ONLY:
		admin_completion->phase = LOAD_PHASE_FINISHED;
		vdo_reset_admin_sub_task(completion);
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
							   completion);
		return;

	default:
		vdo_set_completion_result(vdo_reset_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	vdo_finish_operation(&vdo->admin_state, completion->result);
}

/**
 * handle_load_error() - Handle an error during the load operation.
 * @completion: The sub-task completion.
 *
 * If at all possible, brings the vdo online in read-only mode. This handler
 * is registered in vdo_load().
 */
static void handle_load_error(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_load_sub_task(completion);

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_LOAD);

	if (requires_read_only_rebuild(vdo)
	    && (admin_completion->phase == LOAD_PHASE_MAKE_DIRTY)) {
		uds_log_error_strerror(completion->result, "aborting load");

		/* Preserve the error. */
		vdo_set_operation_result(&vdo->admin_state,
					 completion->result);
		admin_completion->phase = LOAD_PHASE_DRAIN_JOURNAL;
		load_callback(UDS_FORGET(completion));
		return;
	}

	uds_log_error_strerror(completion->result,
			       "Entering read-only mode due to load error");
	admin_completion->phase = LOAD_PHASE_WAIT_FOR_READ_ONLY;
	vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
	vdo_set_operation_result(&vdo->admin_state, VDO_READ_ONLY);
	load_callback(completion);
}

/**
 * vdo_load() - Load a vdo for normal operation.
 * @vdo: The vdo to load.
 *
 * Context: This method must not be called from a base thread.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_load(struct vdo *vdo)
{
	const char *device_name;
	int result;

	device_name = vdo_get_device_name(vdo->device_config->owning_target);
	uds_log_info("starting device '%s'", device_name);
	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_LOAD,
					     get_thread_id_for_phase,
					     load_callback,
					     handle_load_error);

	if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
		/*
		 * Even if the VDO is read-only, it is now able to handle
		 * (read) requests.
		 */
		uds_log_info("device '%s' started", device_name);
		return VDO_SUCCESS;
	}

	/*
	 * Something has gone very wrong. Make sure everything has drained and
	 * leave the device in an unresumable state.
	 */
	uds_log_error_strerror(result,
			       "Start failed, could not load VDO metadata");
	vdo->suspend_type = VDO_ADMIN_STATE_STOPPING;
	vdo_suspend(vdo);
	return result;
}

/**
 * vdo_from_pre_load_sub_task() - Extract the vdo from an admin_completion,
 * @completion: The admin_completion's sub-task completion.
 *
 * Checks that the current operation is a pre-load.
 *
 * Return: The vdo.
 */
static inline struct vdo *
vdo_from_pre_load_sub_task(struct vdo_completion *completion)
{
	return vdo_from_admin_sub_task(completion,
				       VDO_ADMIN_OPERATION_PRE_LOAD);
}

/**
 * decode_from_super_block() - Decode the VDO state from the super block and
 *                             validate that it is correct.
 * @vdo: The vdo being loaded.
 *
 * On error from this method, the component states must be destroyed
 * explicitly. If this method returns successfully, the component states must
 * not be destroyed.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check decode_from_super_block(struct vdo *vdo)
{
	const struct device_config *config = vdo->device_config;
	struct super_block_codec *codec
		= vdo_get_super_block_codec(vdo->super_block);
	int result = vdo_decode_component_states(codec->component_buffer,
						 vdo->geometry.release_version,
						 &vdo->states);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_set_state(vdo, vdo->states.vdo.state);
	vdo->load_state = vdo->states.vdo.state;
	result = vdo_validate_component_states(&vdo->states,
					       vdo->geometry.nonce,
					       config->physical_blocks,
					       config->logical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_decode_layout(vdo->states.layout, &vdo->layout);
}

/**
 * decode_vdo() - Decode the component data portion of a super block and fill
 *                in the corresponding portions of the vdo being loaded.
 * @vdo: The vdo being loaded.
 *
 * This will also allocate the recovery journal and slab depot. If this method
 * is called with an asynchronous layer (i.e. a thread config which specifies
 * at least one base thread), the block map and packer will be constructed as
 * well.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check decode_vdo(struct vdo *vdo)
{
	block_count_t maximum_age, journal_length;
	const struct thread_config *thread_config = vdo->thread_config;
	int result = decode_from_super_block(vdo);

	if (result != VDO_SUCCESS) {
		vdo_destroy_component_states(&vdo->states);
		return result;
	}

	maximum_age = vdo->device_config->block_map_maximum_age;
	journal_length =
		vdo_get_recovery_journal_length(vdo->states.vdo.config.recovery_journal_size);
	if ((maximum_age > (journal_length / 2)) || (maximum_age < 1)) {
		return VDO_BAD_CONFIGURATION;
	}

	result = vdo_make_read_only_notifier(vdo_in_read_only_mode(vdo),
					     thread_config,
					     vdo,
					     &vdo->read_only_notifier);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_enable_read_only_entry(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_recovery_journal(vdo->states.recovery_journal,
					     vdo->states.vdo.nonce,
					     vdo,
					     vdo_get_partition(vdo->layout,
							       VDO_RECOVERY_JOURNAL_PARTITION),
					     vdo->states.vdo.complete_recoveries,
					     vdo->states.vdo.config.recovery_journal_size,
					     VDO_RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
					     vdo->read_only_notifier,
					     thread_config,
					     &vdo->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_slab_depot(vdo->states.slab_depot,
				       vdo,
				       vdo_get_partition(vdo->layout,
							 VDO_SLAB_SUMMARY_PARTITION),
				       &vdo->depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_block_map(vdo->states.block_map,
				      vdo->states.vdo.config.logical_blocks,
				      thread_config,
				      vdo,
				      vdo->read_only_notifier,
				      vdo->recovery_journal,
				      vdo->states.vdo.nonce,
				      vdo->device_config->cache_size,
				      maximum_age,
				      &vdo->block_map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_logical_zones(vdo, &vdo->logical_zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_physical_zones(vdo, &vdo->physical_zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_make_hash_zones(vdo, &vdo->hash_zones);
}

/**
 * finish_operation_callback() - Callback to finish the load operation.
 * @completion: The admin_completion's sub-task completion.
 */
static void finish_operation_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_pre_load_sub_task(completion);

	vdo_finish_operation(&vdo->admin_state, completion->result);
}

/**
 * vdo_load_components() - Load the components of a VDO.
 * @completion: The sub-task completion.
 *
 * This is the super block load callback set by load_callback().
 */
static void vdo_load_components(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_pre_load_sub_task(completion);

	vdo_prepare_admin_sub_task(vdo,
				   finish_operation_callback,
				   finish_operation_callback);
	vdo_finish_completion(completion, decode_vdo(vdo));
}

/**
 * pre_load_callback() - Callback to initiate a pre-load, registered in
 *                       vdo_prepare_to_load().
 * @completion: The sub-task completion.
 */
static void pre_load_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = vdo_from_pre_load_sub_task(completion);

	vdo_assert_on_admin_thread(vdo, __func__);
	if (!vdo_start_operation_with_waiter(&vdo->admin_state,
					     VDO_ADMIN_STATE_PRE_LOADING,
					     &admin_completion->completion,
					     NULL)) {
		return;
	}

	vdo_prepare_admin_sub_task(vdo,
				   vdo_load_components,
				   finish_operation_callback);
	vdo_load_super_block(vdo,
			     completion,
			     vdo_get_data_region_start(vdo->geometry),
			     &vdo->super_block);
}

/**
 * vdo_prepare_to_load() - Perpare a vdo for loading by reading structures off
 *                         disk.
 *
 * This method does not alter the on-disk state. It should be called from the
 * vdo constructor, whereas perform_vdo_load() will be called during
 * pre-resume if the vdo has not been resumed before.
 */
int vdo_prepare_to_load(struct vdo *vdo)
{
	return vdo_perform_admin_operation(vdo,
					   VDO_ADMIN_OPERATION_PRE_LOAD,
					   NULL,
					   pre_load_callback,
					   pre_load_callback);
}
