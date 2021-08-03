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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoSuspend.c#49 $
 */

#include "vdoSuspend.h"

#include "logger.h"
#include "permassert.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "dedupeIndex.h"
#include "logicalZone.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "vdoInternal.h"

enum {
	SUSPEND_PHASE_START = 0,
	SUSPEND_PHASE_PACKER,
	SUSPEND_PHASE_LOGICAL_ZONES,
	SUSPEND_PHASE_BLOCK_MAP,
	SUSPEND_PHASE_JOURNAL,
	SUSPEND_PHASE_DEPOT,
	SUSPEND_PHASE_WRITE_SUPER_BLOCK,
	SUSPEND_PHASE_END,
};

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
 * Implements vdo_thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		get_vdo_thread_config(admin_completion->vdo);
	switch (admin_completion->phase) {
	case SUSPEND_PHASE_PACKER:
		return thread_config->packer_thread;

	case SUSPEND_PHASE_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
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
		finish_vdo_completion(completion, UDS_BAD_STATE);
		return;
	}

	save_vdo_components(vdo, completion);
}

/**
 * Callback to initiate a suspend, registered in suspend_vdo().
 *
 * @param completion  The sub-task completion
 **/
static void suspend_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;
	struct admin_state *admin_state = &vdo->admin_state;
	int result;

	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_SUSPEND);
	assert_vdo_admin_phase_thread(admin_completion, __func__,
				      SUSPEND_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case SUSPEND_PHASE_START:
		if (!start_vdo_draining(admin_state,
					(vdo->no_flush_suspend
					 ? VDO_ADMIN_STATE_SUSPENDING
					 : VDO_ADMIN_STATE_SAVING),
					&admin_completion->completion,
					NULL)) {
			return;
		}

		/*
		 * Attempt to flush all I/O before completing post suspend
		 * work. We believe a suspended device is expected to have
		 * persisted all data ritten before the suspend, even if it
		 * hasn't been flushed yet.
		 */
		vdo_wait_for_no_requests_active(vdo);
		result = vdo_synchronous_flush(vdo);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo->read_only_notifier,
						 result);
		}
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
							   reset_vdo_admin_sub_task(completion));
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
			set_vdo_completion_result(&admin_completion->completion,
						  VDO_READ_ONLY);
		}

		drain_vdo_packer(vdo->packer,
				 reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_LOGICAL_ZONES:
		drain_vdo_logical_zones(vdo->logical_zones,
					get_vdo_admin_state_code(admin_state),
					reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_BLOCK_MAP:
		drain_vdo_block_map(vdo->block_map,
				    get_vdo_admin_state_code(admin_state),
				    reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_JOURNAL:
		drain_vdo_recovery_journal(vdo->recovery_journal,
					   get_vdo_admin_state_code(admin_state),
					   reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DEPOT:
		drain_vdo_slab_depot(vdo->depot,
				     get_vdo_admin_state_code(admin_state),
				     reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
		if (is_vdo_state_suspending(admin_state) ||
		    (admin_completion->completion.result != VDO_SUCCESS)) {
			// If we didn't save the VDO or there was an error,
			// we're done.
			break;
		}

		write_super_block(vdo, reset_vdo_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_END:
		suspend_vdo_dedupe_index(vdo->dedupe_index,
					 !vdo->no_flush_suspend);
		break;

	default:
		set_vdo_completion_result(completion, UDS_BAD_STATE);
	}

	finish_vdo_draining_with_result(admin_state, completion->result);
}

/**********************************************************************/
int suspend_vdo(struct vdo *vdo)
{
	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	int result = perform_vdo_admin_operation(vdo,
						 VDO_ADMIN_OPERATION_SUSPEND,
						 get_thread_id_for_phase,
						 suspend_callback,
						 preserve_vdo_completion_error_and_continue);

	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		uds_log_error_strerror(result, "%s: Suspend device failed",
				       __func__);
	}

	return result;
}
