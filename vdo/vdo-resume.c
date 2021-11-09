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

#include "vdo-resume.h"

#include <linux/kernel.h>

#include "logger.h"

#include "admin-completion.h"
#include "block-map.h"
#include "completion.h"
#include "dedupeIndex.h"
#include "kernel-types.h"
#include "logical-zone.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-resize.h"
#include "vdo-resize-logical.h"

enum {
	RESUME_PHASE_START,
	RESUME_PHASE_ALLOW_READ_ONLY_MODE,
	RESUME_PHASE_INDEX,
	RESUME_PHASE_DEPOT,
	RESUME_PHASE_JOURNAL,
	RESUME_PHASE_BLOCK_MAP,
	RESUME_PHASE_LOGICAL_ZONES,
	RESUME_PHASE_PACKER,
	RESUME_PHASE_FLUSHER,
	RESUME_PHASE_END,
};

static const char *RESUME_PHASE_NAMES[] = {
	"RESUME_PHASE_START",
	"RESUME_PHASE_ALLOW_READ_ONLY_MODE",
	"RESUME_PHASE_INDEX",
	"RESUME_PHASE_DEPOT",
	"RESUME_PHASE_JOURNAL",
	"RESUME_PHASE_BLOCK_MAP",
	"RESUME_PHASE_LOGICAL_ZONES",
	"RESUME_PHASE_PACKER",
	"RESUME_PHASE_FLUSHER",
	"RESUME_PHASE_END",
};

/**
 * Implements vdo_thread_id_getter_for_phase.
 **/
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	const struct thread_config *thread_config =
		admin_completion->vdo->thread_config;
	switch (admin_completion->phase) {
	case RESUME_PHASE_JOURNAL:
		return thread_config->journal_thread;

	case RESUME_PHASE_PACKER:
	case RESUME_PHASE_FLUSHER:
		return thread_config->packer_thread;

	default:
		return thread_config->admin_thread;
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
	switch (get_vdo_state(vdo)) {
	case VDO_CLEAN:
	case VDO_NEW:
		set_vdo_state(vdo, VDO_DIRTY);
		save_vdo_components(vdo, completion);
		return;

	case VDO_DIRTY:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		/* No need to write the super block in these cases */
		complete_vdo_completion(completion);
		return;

	case VDO_REPLAYING:
	default:
		vdo_finish_completion(completion, UDS_BAD_STATE);
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
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	assert_vdo_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_RESUME);
	assert_vdo_admin_phase_thread(admin_completion, __func__,
				      RESUME_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case RESUME_PHASE_START:
		if (start_vdo_resuming(&vdo->admin_state,
				       VDO_ADMIN_STATE_RESUMING,
				       &admin_completion->completion,
				       NULL)) {
			write_super_block(vdo, completion);
		}
		return;

	case RESUME_PHASE_ALLOW_READ_ONLY_MODE:
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier,
					       reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_INDEX:
		if (!vdo_is_read_only(vdo->read_only_notifier)) {
			resume_vdo_dedupe_index(vdo->dedupe_index,
						vdo->device_config->deduplication,
						vdo->load_state == VDO_NEW);
		}

		complete_vdo_completion(reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_DEPOT:
		resume_vdo_slab_depot(vdo->depot, reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_JOURNAL:
		resume_vdo_recovery_journal(vdo->recovery_journal,
					    reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_BLOCK_MAP:
		resume_vdo_block_map(vdo->block_map,
				     reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_LOGICAL_ZONES:
		resume_vdo_logical_zones(vdo->logical_zones,
					 reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_PACKER:
	{
		bool was_enabled = get_vdo_compressing(vdo);
		bool enable = vdo->device_config->compression;

		if (enable != was_enabled) {
			WRITE_ONCE(vdo->compressing, enable);
		}
		uds_log_info("compression is %s",
			     (enable ? "enabled" : "disabled"));

		resume_vdo_packer(vdo->packer,
				  reset_vdo_admin_sub_task(completion));
		return;
	}
	case RESUME_PHASE_FLUSHER:
		resume_vdo_flusher(vdo->flusher,
				  reset_vdo_admin_sub_task(completion));
		return;

	case RESUME_PHASE_END:
		break;

	default:
		set_vdo_completion_result(reset_vdo_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	finish_vdo_resuming_with_result(&vdo->admin_state, completion->result);
}

/**
 * Attempt to make any configuration changes from the table being resumed.
 *
 * @param vdo     The vdo being resumed
 * @param config  The new device configuration derived from the table with which
 *                the vdo is being resumed
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
apply_new_vdo_configuration(struct vdo *vdo, struct device_config *config)
{
	int result;

	result = perform_vdo_grow_logical(vdo, config->logical_blocks);
	if (result != VDO_SUCCESS) {
		uds_log_error("grow logical operation failed, result = %d",
			      result);
		return result;
	}

	result = perform_vdo_grow_physical(vdo, config->physical_blocks);
	if (result != VDO_SUCCESS) {
		uds_log_error("resize operation failed, result = %d", result);
	}

	return result;
}

/**
 * Resume a suspended vdo (technically preresume because resume can't fail).
 *
 * @param vdo          The vdo being resumed
 * @param config       The device config derived from the table with which the
 *                     vdo is being resumed
 * @param device_name  The vdo device name (for logging)
 *
 * @return VDO_SUCCESS or an error
 **/
int preresume_vdo(struct vdo *vdo,
		  struct device_config *config,
		  const char *device_name)
{
	int result;

	/* The VDO was not in a state to be resumed. This should never happen. */
	result = apply_new_vdo_configuration(vdo, config);
	BUG_ON(result == VDO_INVALID_ADMIN_STATE);

	/*
	 * Now that we've tried to modify the vdo, the new config *is* the 
	 * config, whether the modifications worked or not. 
	 */
	vdo->device_config = config;

	/*
	 * Any error here is highly unexpected and the state of the vdo is
	 * questionable, so we mark it read-only in memory. Because we are
	 * suspended, the read-only state will not be written to disk.
	 */
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result,
				       "Commit of modifications to device '%s' failed",
				       device_name);
		vdo_enter_read_only_mode(vdo->read_only_notifier, result);
		return result;
	}

	if (get_vdo_admin_state(vdo)->normal) {
		/* The VDO was just started, so we don't need to resume it. */
		return VDO_SUCCESS;
	}

	result = perform_vdo_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_RESUME,
					     get_thread_id_for_phase,
					     resume_callback,
					     preserve_vdo_completion_error_and_continue);
	BUG_ON(result == VDO_INVALID_ADMIN_STATE);
	if (result != VDO_SUCCESS) {
		uds_log_error("resume of device '%s' failed with error: %d",
			      device_name,
			      result);
	}

	return result;
}
