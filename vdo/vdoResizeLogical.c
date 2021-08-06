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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/vdoResizeLogical.c#1 $
 */

#include "vdoResizeLogical.h"

#include "logger.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "vdoInternal.h"

enum {
	GROW_LOGICAL_PHASE_START = 0,
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
 * perform_vdo_grow_logical().
 *
 * @param completion  The sub-task completion
 **/
static void grow_logical_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_GROW_LOGICAL);
	assert_vdo_admin_phase_thread(admin_completion, __func__,
				      GROW_LOGICAL_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case GROW_LOGICAL_PHASE_START:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			uds_log_error_strerror(VDO_READ_ONLY,
					       "Can't grow logical size of a read-only VDO");
			finish_vdo_completion(reset_vdo_admin_sub_task(completion),
					      VDO_READ_ONLY);
			return;
		}

		if (start_vdo_operation_with_waiter(&vdo->admin_state,
						    VDO_ADMIN_STATE_SUSPENDED_OPERATION,
						    &admin_completion->completion,
						    NULL)) {
			vdo->states.vdo.config.logical_blocks =
				vdo_get_new_entry_count(get_block_map(vdo));
			save_vdo_components(vdo,
					    reset_vdo_admin_sub_task(completion));
		}

		return;

	case GROW_LOGICAL_PHASE_GROW_BLOCK_MAP:
		grow_vdo_block_map(get_block_map(vdo),
				   reset_vdo_admin_sub_task(completion));
		return;

	case GROW_LOGICAL_PHASE_END:
		break;

	case GROW_LOGICAL_PHASE_ERROR:
		vdo_enter_read_only_mode(vdo->read_only_notifier,
					 completion->result);
		break;

	default:
		set_vdo_completion_result(reset_vdo_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	finish_vdo_operation(&vdo->admin_state, completion->result);
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
		// We've failed to write the new size in the super block, so set
		// our in memory config back to the old size.
		struct vdo *vdo = admin_completion->vdo;
		struct block_map *map = get_block_map(vdo);
		vdo->states.vdo.config.logical_blocks =
			vdo_get_number_of_block_map_entries(map);
		vdo_abandon_block_map_growth(map);
	}

	admin_completion->phase = GROW_LOGICAL_PHASE_ERROR;
	grow_logical_callback(completion);
}

/**********************************************************************/
int perform_vdo_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks)
{
	if (vdo_get_new_entry_count(get_block_map(vdo)) != new_logical_blocks) {
		return VDO_PARAMETER_MISMATCH;
	}

	return perform_vdo_admin_operation(vdo,
					   VDO_ADMIN_OPERATION_GROW_LOGICAL,
					   get_thread_id_for_phase,
					   grow_logical_callback,
					   handle_growth_error);
}

/**********************************************************************/
int prepare_vdo_to_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks)
{
	const char *message;
	block_count_t logical_blocks = vdo->states.vdo.config.logical_blocks;
	if (new_logical_blocks > logical_blocks) {
		return vdo_prepare_to_grow_block_map(get_block_map(vdo),
						     new_logical_blocks);
	}

	message = ((new_logical_blocks < logical_blocks)
	           ? "Can't shrink VDO logical size from its current value of "
	           : "Can't grow VDO logical size to its current value of ");
	return uds_log_error_strerror(VDO_PARAMETER_MISMATCH,
				      "%s%llu",
				      message,
				      (unsigned long long) logical_blocks);
}
