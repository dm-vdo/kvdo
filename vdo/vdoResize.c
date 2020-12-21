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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoResize.c#37 $
 */

#include "vdoResize.h"

#include "logger.h"

#include "adminCompletion.h"
#include "completion.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "vdoInternal.h"
#include "vdoLayout.h"

typedef enum {
	GROW_PHYSICAL_PHASE_START = 0,
	GROW_PHYSICAL_PHASE_COPY_SUMMARY,
	GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS,
	GROW_PHYSICAL_PHASE_USE_NEW_SLABS,
	GROW_PHYSICAL_PHASE_END,
	GROW_PHYSICAL_PHASE_ERROR,
} GrowPhysicalPhase;

static const char *GROW_PHYSICAL_PHASE_NAMES[] = {
	"GROW_PHYSICAL_PHASE_START",
	"GROW_PHYSICAL_PHASE_COPY_SUMMARY",
	"GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS",
	"GROW_PHYSICAL_PHASE_USE_NEW_SLABS",
	"GROW_PHYSICAL_PHASE_END",
	"GROW_PHYSICAL_PHASE_ERROR",
};

/**
 * Implements thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	return get_admin_thread(get_thread_config(admin_completion->vdo));
}

/**
 * Callback to initiate a grow physical, registered in perform_grow_physical().
 *
 * @param completion  The sub-task completion
 **/
static void grow_physical_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	assert_admin_operation_type(admin_completion,
				    ADMIN_OPERATION_GROW_PHYSICAL);
	assert_admin_phase_thread(admin_completion, __func__,
	                          GROW_PHYSICAL_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case GROW_PHYSICAL_PHASE_START:
		if (is_read_only(vdo->read_only_notifier)) {
			log_error_strerror(VDO_READ_ONLY,
					   "Can't grow physical size of a read-only VDO");
			set_completion_result(reset_admin_sub_task(completion),
					      VDO_READ_ONLY);
			break;
		}

		if (start_operation_with_waiter(&vdo->admin_state,
						ADMIN_STATE_SUSPENDED_OPERATION,
						&admin_completion->completion,
						NULL)) {
			// Copy the journal into the new layout.
			copy_vdo_layout_partition(vdo->layout,
						  RECOVERY_JOURNAL_PARTITION,
						  reset_admin_sub_task(completion));
		}
		return;

	case GROW_PHYSICAL_PHASE_COPY_SUMMARY:
		copy_vdo_layout_partition(vdo->layout,
					  SLAB_SUMMARY_PARTITION,
					  reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS:
		vdo->states.vdo.config.physical_blocks =
			grow_vdo_layout(vdo->layout);
		update_slab_depot_size(vdo->depot);
		save_vdo_components(vdo, reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_USE_NEW_SLABS:
		use_new_slabs(vdo->depot, reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_END:
		set_slab_summary_origin(get_slab_summary(vdo->depot),
					get_vdo_partition(vdo->layout,
							  SLAB_SUMMARY_PARTITION));
		set_recovery_journal_partition(vdo->recovery_journal,
					       get_vdo_partition(vdo->layout,
								 RECOVERY_JOURNAL_PARTITION));
		break;

	case GROW_PHYSICAL_PHASE_ERROR:
		enter_read_only_mode(vdo->read_only_notifier, completion->result);
		break;

	default:
		set_completion_result(reset_admin_sub_task(completion),
				      UDS_BAD_STATE);
	}

	finish_vdo_layout_growth(vdo->layout);
	finish_operation_with_result(&vdo->admin_state, completion->result);
}

/**
 * Handle an error during the grow physical process.
 *
 * @param completion  The sub-task completion
 **/
static void handle_growth_error(struct vdo_completion *completion)
{
	admin_completion_from_sub_task(completion)->phase =
		GROW_PHYSICAL_PHASE_ERROR;
	grow_physical_callback(completion);
}

/**********************************************************************/
int perform_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size, prepared_depot_size;

	block_count_t old_physical_blocks =
		vdo->states.vdo.config.physical_blocks;

	// Skip any noop grows.
	if (old_physical_blocks == new_physical_blocks) {
		return VDO_SUCCESS;
	}

	if (new_physical_blocks != get_next_vdo_layout_size(vdo->layout)) {
		/*
		 * Either the VDO isn't prepared to grow, or it was prepared to
		 * grow to a different size. Doing this check here relies on
		 * the fact that the call to this method is done under the
		 * dmsetup message lock.
		 */
		finish_vdo_layout_growth(vdo->layout);
		abandon_new_slabs(vdo->depot);
		return VDO_PARAMETER_MISMATCH;
	}

	// Validate that we are prepared to grow appropriately.
	new_depot_size =
		get_next_block_allocator_partition_size(vdo->layout);
	prepared_depot_size = get_new_depot_size(vdo->depot);
	if (prepared_depot_size != new_depot_size) {
		return VDO_PARAMETER_MISMATCH;
	}

	result = perform_admin_operation(vdo, ADMIN_OPERATION_GROW_PHYSICAL,
					 get_thread_id_for_phase,
					 grow_physical_callback,
					 handle_growth_error);
	if (result != VDO_SUCCESS) {
		return result;
	}

	log_info("Physical block count was %llu, now %llu",
		 old_physical_blocks,
		 new_physical_blocks);
	return VDO_SUCCESS;
}

/**
 * Callback to check that we're not in recovery mode, used in
 * prepare_to_grow_physical().
 *
 * @param completion  The sub-task completion
 **/
static void check_may_grow_physical(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	assert_admin_operation_type(admin_completion,
				    ADMIN_OPERATION_PREPARE_GROW_PHYSICAL);
	assert_on_admin_thread(vdo, __func__);

	reset_admin_sub_task(completion);

	// This check can only be done from a base code thread.
	if (is_read_only(vdo->read_only_notifier)) {
		finish_completion(completion->parent, VDO_READ_ONLY);
		return;
	}

	// This check should only be done from a base code thread.
	if (in_recovery_mode(vdo)) {
		finish_completion(completion->parent, VDO_RETRY_AFTER_REBUILD);
		return;
	}

	complete_completion(completion->parent);
}

/**********************************************************************/
int prepare_to_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size;

	block_count_t current_physical_blocks =
		vdo->states.vdo.config.physical_blocks;
	if (new_physical_blocks < current_physical_blocks) {
		return log_error_strerror(VDO_NOT_IMPLEMENTED,
					  "Removing physical storage from a VDO is not supported");
	}

	if (new_physical_blocks == current_physical_blocks) {
		log_warning("Requested physical block count %llu not greater than %llu",
			    new_physical_blocks,
			    current_physical_blocks);
		finish_vdo_layout_growth(vdo->layout);
		abandon_new_slabs(vdo->depot);
		return VDO_PARAMETER_MISMATCH;
	}

	result =
		perform_admin_operation(vdo,
					ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
					get_thread_id_for_phase,
					check_may_grow_physical,
					finish_parent_callback);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = prepare_to_grow_vdo_layout(vdo->layout,
					    current_physical_blocks,
					    new_physical_blocks,
					    vdo->layer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	new_depot_size =
		get_next_block_allocator_partition_size(vdo->layout);
	result = prepare_to_grow_slab_depot(vdo->depot, new_depot_size);
	if (result != VDO_SUCCESS) {
		finish_vdo_layout_growth(vdo->layout);
		return result;
	}

	return VDO_SUCCESS;
}
