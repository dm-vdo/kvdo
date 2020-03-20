/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoResume.c#17 $
 */

#include "vdoResume.h"

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
	RESUME_PHASE_START = 0,
	RESUME_PHASE_ALLOW_READ_ONLY_MODE,
	RESUME_PHASE_DEPOT,
	RESUME_PHASE_JOURNAL,
	RESUME_PHASE_BLOCK_MAP,
	RESUME_PHASE_LOGICAL_ZONES,
	RESUME_PHASE_PACKER,
	RESUME_PHASE_END,
} resume_phase;

static const char *RESUME_PHASE_NAMES[] = {
	"RESUME_PHASE_START",
	"RESUME_PHASE_ALLOW_READ_ONLY_MODE",
	"RESUME_PHASE_DEPOT",
	"RESUME_PHASE_JOURNAL",
	"RESUME_PHASE_BLOCK_MAP",
	"RESUME_PHASE_LOGICAL_ZONES",
	"RESUME_PHASE_PACKER",
	"RESUME_PHASE_END",
};

/**
 * Implements ThreadIDGetterForPhase.
 **/
__attribute__((warn_unused_result)) static ThreadID
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const ThreadConfig *thread_config =
		getThreadConfig(admin_completion->vdo);
	switch (admin_completion->phase) {
	case RESUME_PHASE_JOURNAL:
		return getJournalZoneThread(thread_config);

	case RESUME_PHASE_PACKER:
		return getPackerZoneThread(thread_config);

	default:
		return getAdminThread(thread_config);
	}
}

/**
 * Update the VDO state and save the super block.
 *
 * @param vdo         The vdo being resumed
 * @param completion  The admin_completion's sub-task completion
 **/
static void write_super_block(struct vdo *vdo,
			      struct vdo_completion *completion)
{
	switch (getVDOState(vdo)) {
	case VDO_CLEAN:
	case VDO_NEW:
		setVDOState(vdo, VDO_DIRTY);
		saveVDOComponentsAsync(vdo, completion);
		return;

	case VDO_DIRTY:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		// No need to write the super block in these cases
		completeCompletion(completion);
		return;

	case VDO_REPLAYING:
	default:
		finishCompletion(completion, UDS_BAD_STATE);
	}
}

/**
 * Callback to resume a VDO.
 *
 * @param completion  The sub-task completion
 **/
static void resume_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	assert_admin_operation_type(admin_completion, ADMIN_OPERATION_RESUME);
	assert_admin_phase_thread(admin_completion, __func__,
				  RESUME_PHASE_NAMES);

	struct vdo *vdo = admin_completion->vdo;
	switch (admin_completion->phase++) {
	case RESUME_PHASE_START:
		if (start_resuming(&vdo->adminState,
				   ADMIN_STATE_RESUMING,
				   &admin_completion->completion,
				   NULL)) {
			write_super_block(vdo, completion);
		}
		return;

	case RESUME_PHASE_ALLOW_READ_ONLY_MODE:
		allow_read_only_mode_entry(vdo->readOnlyNotifier,
					   reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_DEPOT:
		resume_slab_depot(vdo->depot, reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_JOURNAL:
		resume_recovery_journal(vdo->recoveryJournal,
					reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_BLOCK_MAP:
		resume_block_map(vdo->blockMap,
				 reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_LOGICAL_ZONES:
		resume_logical_zones(vdo->logicalZones,
				     reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_PACKER:
		resume_packer(vdo->packer, reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_END:
		break;

	default:
		setCompletionResult(reset_admin_sub_task(completion),
				    UDS_BAD_STATE);
	}

	finish_resuming_with_result(&vdo->adminState, completion->result);
}

/**********************************************************************/
int perform_vdo_resume(struct vdo *vdo)
{
	return perform_admin_operation(vdo,
				       ADMIN_OPERATION_RESUME,
				       get_thread_id_for_phase,
				       resume_callback,
				       preserveErrorAndContinue);
}
