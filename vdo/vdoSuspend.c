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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoSuspend.c#28 $
 */

#include "vdoSuspend.h"

#include "logger.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "logicalZone.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "vdoInternal.h"

typedef enum {
	SUSPEND_PHASE_START = 0,
	SUSPEND_PHASE_PACKER,
	SUSPEND_PHASE_LOGICAL_ZONES,
	SUSPEND_PHASE_BLOCK_MAP,
	SUSPEND_PHASE_JOURNAL,
	SUSPEND_PHASE_DEPOT,
	SUSPEND_PHASE_WRITE_SUPER_BLOCK,
	SUSPEND_PHASE_END,
} suspend_phase;

static const char *SUSPEND_PHASE_NAMES[] = {
	"SUSPEND_PHASE_START",
	"SUSPEND_PHASE_PACKER",
	"SUSPEND_PHASE_LOGICAL_ZONES",
	"SUSPEND_PHASE_BLOCK_MAP",
	"SUSPEND_PHASE_JOURNAL",
	"SUSPEND_PHASE_DEPOT",
	"SUSPEND_PHASE_WRITE_SUPER_BLOCK",
	"SUSPEND_PHASE_END",
};

/**
 * Implements thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		get_thread_config(admin_completion->vdo);
	switch (admin_completion->phase) {
	case SUSPEND_PHASE_PACKER:
		return get_packer_zone_thread(thread_config);

	case SUSPEND_PHASE_JOURNAL:
		return get_journal_zone_thread(thread_config);

	default:
		return get_admin_thread(thread_config);
	}
}

/**
 * Update the VDO state and save the super block.
 *
 * @param vdo         The vdo being suspended
 * @param completion  The admin_completion's sub-task completion
 **/
static void write_super_block(struct vdo *vdo,
			      struct vdo_completion *completion)
{
	switch (get_vdo_state(vdo)) {
	case VDO_DIRTY:
	case VDO_NEW:
		set_vdo_state(vdo, VDO_CLEAN);
		break;

	case VDO_CLEAN:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		break;

	case VDO_REPLAYING:
	default:
		finish_completion(completion, UDS_BAD_STATE);
		return;
	}

	save_vdo_components(vdo, completion);
}

/**
 * Callback to initiate a suspend, registered in perform_vdo_suspend().
 *
 * @param completion  The sub-task completion
 **/
static void suspend_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	ASSERT_LOG_ONLY(((admin_completion->type == ADMIN_OPERATION_SUSPEND) ||
			 (admin_completion->type == ADMIN_OPERATION_SAVE)),
			"unexpected admin operation type %u is neither suspend nor save",
			admin_completion->type);
	assert_admin_phase_thread(admin_completion, __func__,
				  SUSPEND_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case SUSPEND_PHASE_START:
		if (!start_draining(&vdo->admin_state,
				    ((admin_completion->type ==
				      ADMIN_OPERATION_SUSPEND) ?
					     ADMIN_STATE_SUSPENDING :
					     ADMIN_STATE_SAVING),
				    &admin_completion->completion,
				    NULL)) {
			return;
		}

		if (!vdo->close_required) {
			// There's nothing to do.
			break;
		}

		wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
						       reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_PACKER:
		/*
		 * If the VDO was already resumed from a prior suspend while
		 * read-only, some of the components may not have been resumed.
		 * By setting a read-only error here, we guarantee that the
		 * result of this suspend will be VDO_READ_ONLY and not
		 * VDO_INVALID_ADMIN_STATE in that case.
		 */
		if (in_read_only_mode(vdo)) {
			set_completion_result(&admin_completion->completion,
					      VDO_READ_ONLY);
		}

		drain_packer(vdo->packer, reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_LOGICAL_ZONES:
		drain_logical_zones(vdo->logical_zones,
				    vdo->admin_state.state,
				    reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_BLOCK_MAP:
		drain_block_map(vdo->block_map,
				vdo->admin_state.state,
				reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_JOURNAL:
		drain_recovery_journal(vdo->recovery_journal,
				       vdo->admin_state.state,
				       reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DEPOT:
		drain_slab_depot(vdo->depot,
				 vdo->admin_state.state,
				 reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
		if (is_suspending(&vdo->admin_state) ||
		    (admin_completion->completion.result != VDO_SUCCESS)) {
			// If we didn't save the VDO or there was an error,
			// we're done.
			break;
		}

		write_super_block(vdo, reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_END:
		break;

	default:
		set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_draining_with_result(&vdo->admin_state, completion->result);
}

/**********************************************************************/
int perform_vdo_suspend(struct vdo *vdo, bool save)
{
	return perform_admin_operation(vdo,
				       (save ? ADMIN_OPERATION_SAVE :
					       ADMIN_OPERATION_SUSPEND),
				       get_thread_id_for_phase,
				       suspend_callback,
				       preserve_error_and_continue);
}
