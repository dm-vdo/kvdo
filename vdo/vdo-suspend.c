// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-suspend.h"

#include "logger.h"
#include "permassert.h"

#include "admin-completion.h"
#include "block-map.h"
#include "completion.h"
#include "data-vio-pool.h"
#include "dedupe.h"
#include "kernel-types.h"
#include "logical-zone.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

enum {
	SUSPEND_PHASE_START,
	SUSPEND_PHASE_PACKER,
	SUSPEND_PHASE_DATA_VIOS,
	SUSPEND_PHASE_DEDUPE,
	SUSPEND_PHASE_FLUSHES,
	SUSPEND_PHASE_LOGICAL_ZONES,
	SUSPEND_PHASE_BLOCK_MAP,
	SUSPEND_PHASE_JOURNAL,
	SUSPEND_PHASE_DEPOT,
	SUSPEND_PHASE_READ_ONLY_WAIT,
	SUSPEND_PHASE_WRITE_SUPER_BLOCK,
	SUSPEND_PHASE_END,
};

static const char *SUSPEND_PHASE_NAMES[] = {
	"SUSPEND_PHASE_START",
	"SUSPEND_PHASE_PACKER",
	"SUSPEND_PHASE_DATA_VIOS",
	"SUSPEND_PHASE_DEDUPE",
	"SUSPEND_PHASE_FLUSHES",
	"SUSPEND_PHASE_LOGICAL_ZONES",
	"SUSPEND_PHASE_BLOCK_MAP",
	"SUSPEND_PHASE_JOURNAL",
	"SUSPEND_PHASE_DEPOT",
	"SUSPEND_PHASE_READ_ONLY_WAIT",
	"SUSPEND_PHASE_WRITE_SUPER_BLOCK",
	"SUSPEND_PHASE_END",
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
	case SUSPEND_PHASE_PACKER:
	case SUSPEND_PHASE_FLUSHES:
		return thread_config->packer_thread;

	case SUSPEND_PHASE_DATA_VIOS:
		return thread_config->cpu_thread;

	case SUSPEND_PHASE_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
	}
}

/**
 * write_super_block() - Update the VDO state and save the super block.
 * @vdo: The vdo being suspended.
 * @completion: The admin_completion's sub-task completion.
 */
static void write_super_block(struct vdo *vdo,
			      struct vdo_completion *completion)
{
	switch (vdo_get_state(vdo)) {
	case VDO_DIRTY:
	case VDO_NEW:
		vdo_set_state(vdo, VDO_CLEAN);
		break;

	case VDO_CLEAN:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		break;

	case VDO_REPLAYING:
	default:
		vdo_finish_completion(completion, UDS_BAD_STATE);
		return;
	}

	vdo_save_components(vdo, completion);
}

/**
 * suspend_callback() - Callback to initiate a suspend, registered in
 *                      vdo_suspend().
 * @completion: The sub-task completion.
 */
static void suspend_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;
	struct admin_state *admin_state = &vdo->admin_state;
	int result;

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_SUSPEND);
	vdo_assert_admin_phase_thread(admin_completion, __func__,
				      SUSPEND_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case SUSPEND_PHASE_START:
		if (vdo_start_draining(admin_state,
				       vdo->suspend_type,
				       &admin_completion->completion,
				       NULL)) {
			vdo_complete_completion(vdo_reset_admin_sub_task(completion));
		}
		return;

	case SUSPEND_PHASE_PACKER:
		/*
		 * If the VDO was already resumed from a prior suspend while
		 * read-only, some of the components may not have been resumed.
		 * By setting a read-only error here, we guarantee that the
		 * result of this suspend will be VDO_READ_ONLY and not
		 * VDO_INVALID_ADMIN_STATE in that case.
		 */
		if (vdo_in_read_only_mode(vdo)) {
			vdo_set_completion_result(&admin_completion->completion,
						  VDO_READ_ONLY);
		}

		vdo_drain_packer(vdo->packer,
				 vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DATA_VIOS:
		drain_data_vio_pool(vdo->data_vio_pool,
				    vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DEDUPE:
		vdo_drain_hash_zones(vdo->hash_zones,
				     vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_FLUSHES:
		vdo_drain_flusher(vdo->flusher,
				  vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_LOGICAL_ZONES:
		/*
		 * Attempt to flush all I/O before completing post suspend
		 * work. We believe a suspended device is expected to have
		 * persisted all data written before the suspend, even if it
		 * hasn't been flushed yet.
		 */
		result = vdo_synchronous_flush(vdo);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo->read_only_notifier,
						 result);
		}

		vdo_drain_logical_zones(vdo->logical_zones,
					vdo_get_admin_state_code(admin_state),
					vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_BLOCK_MAP:
		vdo_drain_block_map(vdo->block_map,
				    vdo_get_admin_state_code(admin_state),
				    vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_JOURNAL:
		vdo_drain_recovery_journal(vdo->recovery_journal,
					   vdo_get_admin_state_code(admin_state),
					   vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_DEPOT:
		vdo_drain_slab_depot(vdo->depot,
				     vdo_get_admin_state_code(admin_state),
				     vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_READ_ONLY_WAIT:
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
							   vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
		if (vdo_is_state_suspending(admin_state) ||
		    (admin_completion->completion.result != VDO_SUCCESS)) {
			/*
			 * If we didn't save the VDO or there was an error,
			 * we're done.
			 */
			break;
		}

		write_super_block(vdo, vdo_reset_admin_sub_task(completion));
		return;

	case SUSPEND_PHASE_END:
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	vdo_finish_draining_with_result(admin_state, completion->result);
}

/**
 * vdo_suspend() - Ensure that the vdo has no outstanding I/O and will issue
 *                 none until it is resumed.
 * @vdo: The vdo to suspend.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_suspend(struct vdo *vdo)
{
	const char *device_name;
	int result;

	device_name = vdo_get_device_name(vdo->device_config->owning_target);
	uds_log_info("suspending device '%s'", device_name);

	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_SUSPEND,
					     get_thread_id_for_phase,
					     suspend_callback,
					     vdo_preserve_completion_error_and_continue);

	/*
	 * Treat VDO_READ_ONLY as a success since a read-only suspension still
	 * leaves the VDO suspended.
	 */
	if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
		uds_log_info("device '%s' suspended", device_name);
		return VDO_SUCCESS;
	}

	if (result == VDO_INVALID_ADMIN_STATE) {
		uds_log_error("Suspend invoked while in unexpected state: %s",
			      vdo_get_admin_state(vdo)->name);
		result = -EINVAL;
	}

	uds_log_error_strerror(result,
			       "Suspend of device '%s' failed",
			       device_name);
	return result;
}
