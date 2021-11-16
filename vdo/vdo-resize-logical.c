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

#include "vdo-resize-logical.h"

#include "logger.h"

#include "admin-completion.h"
#include "block-map.h"
#include "completion.h"
#include "kernel-types.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

enum {
	GROW_LOGICAL_PHASE_START,
	GROW_LOGICAL_PHASE_GROW_BLOCK_MAP,
	GROW_LOGICAL_PHASE_END,
	GROW_LOGICAL_PHASE_ERROR,
};

static const char *GROW_LOGICAL_PHASE_NAMES[] = {
	"GROW_LOGICAL_PHASE_START",
	"GROW_LOGICAL_PHASE_GROW_BLOCK_MAP",
	"GROW_LOGICAL_PHASE_END",
	"GROW_LOGICAL_PHASE_ERROR",
};

/**
 * Implements vdo_thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	return admin_completion->vdo->thread_config->admin_thread;
}

/**
 * Callback to initiate a grow logical, registered in
 * vdo_perform_grow_logical().
 *
 * @param completion  The sub-task completion
 **/
static void grow_logical_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_GROW_LOGICAL);
	vdo_assert_admin_phase_thread(admin_completion, __func__,
				      GROW_LOGICAL_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case GROW_LOGICAL_PHASE_START:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			uds_log_error_strerror(VDO_READ_ONLY,
					       "Can't grow logical size of a read-only VDO");
			vdo_finish_completion(vdo_reset_admin_sub_task(completion),
					      VDO_READ_ONLY);
			return;
		}

		if (vdo_start_operation_with_waiter(&vdo->admin_state,
						    VDO_ADMIN_STATE_SUSPENDED_OPERATION,
						    &admin_completion->completion,
						    NULL)) {
			vdo->states.vdo.config.logical_blocks =
				vdo_get_new_entry_count(vdo->block_map);
			vdo_save_components(vdo,
					    vdo_reset_admin_sub_task(completion));
		}

		return;

	case GROW_LOGICAL_PHASE_GROW_BLOCK_MAP:
		vdo_grow_block_map(vdo->block_map,
				   vdo_reset_admin_sub_task(completion));
		return;

	case GROW_LOGICAL_PHASE_END:
		break;

	case GROW_LOGICAL_PHASE_ERROR:
		vdo_enter_read_only_mode(vdo->read_only_notifier,
					 completion->result);
		break;

	default:
		vdo_set_completion_result(vdo_reset_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	vdo_finish_operation(&vdo->admin_state, completion->result);
}

/**
 * Handle an error during the grow physical process.
 *
 * @param completion  The sub-task completion
 **/
static void handle_growth_error(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	if (admin_completion->phase == GROW_LOGICAL_PHASE_GROW_BLOCK_MAP) {
		/*
		 * We've failed to write the new size in the super block, so set 
		 * our in memory config back to the old size. 
		 */
		struct vdo *vdo = admin_completion->vdo;

		vdo->states.vdo.config.logical_blocks =
			vdo_get_number_of_block_map_entries(vdo->block_map);
		vdo_abandon_block_map_growth(vdo->block_map);
	}

	admin_completion->phase = GROW_LOGICAL_PHASE_ERROR;
	grow_logical_callback(completion);
}

/**
 * Grow the logical size of the vdo. This method may only be called when the
 * vdo has been suspended and must not be called from a base thread.
 *
 * @param vdo               	The vdo to grow
 * @param new_logical_blocks	The size to which the vdo should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_perform_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks)
{
	int result;

	if (vdo->device_config->logical_blocks == new_logical_blocks) {
		/*
		 * A table was loaded for which we prepared to grow, but 
		 * a table without that growth was what we are resuming with. 
		 */
		vdo_abandon_block_map_growth(vdo->block_map);
		return VDO_SUCCESS;
	}

	uds_log_info("Resizing logical to %llu",
		     (unsigned long long) new_logical_blocks);

	if (vdo_get_new_entry_count(vdo->block_map) != new_logical_blocks) {
		return VDO_PARAMETER_MISMATCH;
	}

	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_GROW_LOGICAL,
					     get_thread_id_for_phase,
					     grow_logical_callback,
					     handle_growth_error);
	if (result != VDO_SUCCESS) {
		return result;
	}

	uds_log_info("Logical blocks now %llu",
		     (unsigned long long) new_logical_blocks);
	return VDO_SUCCESS;
}

/**
 * Prepare to grow the logical size of vdo. This method may only be called
 * while the vdo is running.
 *
 * @param vdo               	The vdo to prepare for growth
 * @param new_logical_blocks	The size to which the vdo should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_prepare_to_grow_logical(struct vdo *vdo,
				block_count_t new_logical_blocks)
{
	block_count_t logical_blocks = vdo->states.vdo.config.logical_blocks;
	int result;

	uds_log_info("Preparing to resize logical to %llu",
		     (unsigned long long) new_logical_blocks);
	ASSERT_LOG_ONLY((new_logical_blocks > logical_blocks),
			"New logical size is larger than current size");
	result = vdo_prepare_to_grow_block_map(vdo->block_map,
					       new_logical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	uds_log_info("Done preparing to resize logical");
	return VDO_SUCCESS;
}
