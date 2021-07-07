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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLoad.c#93 $
 */

#include "vdoLoad.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "deviceConfig.h"
#include "hashZone.h"
#include "header.h"
#include "logicalZone.h"
#include "physicalZone.h"
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
 * Finish aborting a load now that any entry to read-only mode is complete.
 * This callback is registered in abort_load().
 *
 * @param completion  The sub-task completion
 **/
static void finish_aborting(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	vdo->close_required = false;
	finish_vdo_completion_parent_callback(completion);
}

/**
 * Make sure the recovery journal is closed when aborting a load.
 *
 * @param completion  The sub-task completion
 **/
static void close_recovery_journal_for_abort(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	prepare_vdo_admin_sub_task(vdo, finish_aborting, finish_aborting);
	drain_vdo_recovery_journal(vdo->recovery_journal,
				   VDO_ADMIN_STATE_SAVING,
				   completion);
}

/**
 * Clean up after an error loading a VDO. This error handler is set in
 * load_callback() and load_vdo_components().
 *
 * @param completion  The sub-task completion
 **/
static void abort_load(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	uds_log_error_strerror(completion->result, "aborting load");
	if (vdo->read_only_notifier == NULL) {
		// There are no threads, so we're done
		finish_vdo_completion_parent_callback(completion);
		return;
	}

	// Preserve the error.
	set_vdo_completion_result(completion->parent, completion->result);
	if (vdo->recovery_journal == NULL) {
		prepare_vdo_admin_sub_task(vdo, finish_aborting, finish_aborting);
	} else {
		prepare_vdo_admin_sub_task_on_thread(vdo,
						     close_recovery_journal_for_abort,
						     close_recovery_journal_for_abort,
						     vdo_get_journal_zone_thread(get_vdo_thread_config(vdo)));
	}

	vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
						   completion);
}

/**
 * Wait for the VDO to be in read-only mode.
 *
 * @param completion  The sub-task completion
 **/
static void wait_for_read_only_mode(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	prepare_vdo_completion_to_finish_parent(completion, completion->parent);
	set_vdo_completion_result(completion, VDO_READ_ONLY);
	vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
						   completion);
}

/**
 * Finish loading the VDO after an error, but leave it in read-only
 * mode.  This error handler is set in make_dirty(), scrub_vdo_slabs(), and
 * load_vdo_components().
 *
 * @param completion  The sub-task completion
 **/
static void continue_load_read_only(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	uds_log_error_strerror(completion->result,
			       "Entering read-only mode due to load error");
	vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
	wait_for_read_only_mode(completion);
}

/**
 * Initiate slab scrubbing if necessary. This callback is registered in
 * prepare_to_come_online().
 *
 * @param completion   The sub-task completion
 **/
static void scrub_vdo_slabs(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	if (requires_recovery(vdo)) {
		enter_recovery_mode(vdo);
	}

	prepare_vdo_admin_sub_task(vdo, finish_vdo_completion_parent_callback,
				   continue_load_read_only);
	vdo_scrub_all_unrecovered_slabs(vdo->depot, completion);
}

/**
 * This is the error handler for slab scrubbing. It is registered in
 * prepare_to_come_online().
 *
 * @param completion  The sub-task completion
 **/
static void handle_scrubbing_error(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
	wait_for_read_only_mode(completion);
}

/**
 * This is the callback after the super block is written. It prepares the block
 * allocator to come online and start allocating. It is registered in
 * make_dirty().
 *
 * @param completion  The sub-task completion
 **/
static void prepare_to_come_online(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	enum slab_depot_load_type load_type = NORMAL_LOAD;
	if (requires_read_only_rebuild(vdo)) {
		load_type = REBUILD_LOAD;
	} else if (requires_recovery(vdo)) {
		load_type = RECOVERY_LOAD;
	}

	initialize_vdo_block_map_from_journal(vdo->block_map,
					      vdo->recovery_journal);

	prepare_vdo_admin_sub_task(vdo, scrub_vdo_slabs, handle_scrubbing_error);
	prepare_vdo_slab_depot_to_allocate(vdo->depot, load_type, completion);
}

/**
 * Mark the super block as dirty now that everything has been loaded or
 * rebuilt.
 *
 * @param completion  The sub-task completion
 **/
static void make_dirty(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	if (vdo_is_read_only(vdo->read_only_notifier)) {
		finish_vdo_completion(completion->parent, VDO_READ_ONLY);
		return;
	}

	set_vdo_state(vdo, VDO_DIRTY);
	prepare_vdo_admin_sub_task(vdo, prepare_to_come_online,
				   continue_load_read_only);
	save_vdo_components(vdo, completion);
}

/**
 * Callback to do the destructive parts of a load now that the new VDO device
 * is being resumed.
 *
 * @param completion  The sub-task completion
 **/
static void load_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	assert_on_admin_thread(vdo, __func__);

	// Prepare the recovery journal for new entries.
	open_vdo_recovery_journal(vdo->recovery_journal, vdo->depot,
				  vdo->block_map);
	vdo->close_required = true;
	if (vdo_is_read_only(vdo->read_only_notifier)) {
		// In read-only mode we don't use the allocator and it may not
		// even be readable, so use the default structure.
		finish_vdo_completion(completion->parent, VDO_READ_ONLY);
		return;
	}

	if (requires_read_only_rebuild(vdo)) {
		prepare_vdo_admin_sub_task(vdo, make_dirty, abort_load);
		launch_vdo_rebuild(vdo, completion);
		return;
	}

	if (requires_rebuild(vdo)) {
		prepare_vdo_admin_sub_task(vdo, make_dirty, continue_load_read_only);
		vdo_launch_recovery(vdo, completion);
		return;
	}

	prepare_vdo_admin_sub_task(vdo, make_dirty, continue_load_read_only);
	load_vdo_slab_depot(vdo->depot,
			    (vdo_was_new(vdo) ? VDO_ADMIN_STATE_FORMATTING :
						VDO_ADMIN_STATE_LOADING),
			    completion,
			    NULL);
}

/**********************************************************************/
int perform_vdo_load(struct vdo *vdo)
{
	return perform_vdo_admin_operation(vdo, VDO_ADMIN_OPERATION_LOAD, NULL,
					   load_callback, load_callback);
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
 * Load the components of a VDO. This is the super block load callback
 * set by load_callback().
 *
 * @param completion The sub-task completion
 **/
static void load_vdo_components(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);

	prepare_vdo_completion(completion,
			       finish_vdo_completion_parent_callback,
			       abort_load,
			       completion->callback_thread_id,
			       completion->parent);
	finish_vdo_completion(completion, decode_vdo(vdo));
}

/**
 * Callback to initiate a pre-load, registered in prepare_to_load_vdo().
 *
 * @param completion  The sub-task completion
 **/
static void pre_load_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = vdo_from_load_sub_task(completion);
	assert_on_admin_thread(vdo, __func__);
	prepare_vdo_admin_sub_task(vdo, load_vdo_components, abort_load);
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
