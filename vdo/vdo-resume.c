// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-resume.h"

#include <linux/kernel.h>

#include "logger.h"

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
#include "vdo-resize.h"
#include "vdo-resize-logical.h"

enum {
	RESUME_PHASE_START,
	RESUME_PHASE_ALLOW_READ_ONLY_MODE,
	RESUME_PHASE_DEDUPE,
	RESUME_PHASE_DEPOT,
	RESUME_PHASE_JOURNAL,
	RESUME_PHASE_BLOCK_MAP,
	RESUME_PHASE_LOGICAL_ZONES,
	RESUME_PHASE_PACKER,
	RESUME_PHASE_FLUSHER,
	RESUME_PHASE_DATA_VIOS,
	RESUME_PHASE_END,
};

static const char *RESUME_PHASE_NAMES[] = {
	"RESUME_PHASE_START",
	"RESUME_PHASE_ALLOW_READ_ONLY_MODE",
	"RESUME_PHASE_DEDUPE",
	"RESUME_PHASE_DEPOT",
	"RESUME_PHASE_JOURNAL",
	"RESUME_PHASE_BLOCK_MAP",
	"RESUME_PHASE_LOGICAL_ZONES",
	"RESUME_PHASE_PACKER",
	"RESUME_PHASE_FLUSHER",
	"RESUME_PHASE_DATA_VIOS",
	"RESUME_PHASE_END",
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
	case RESUME_PHASE_JOURNAL:
		return thread_config->journal_thread;

	case RESUME_PHASE_PACKER:
	case RESUME_PHASE_FLUSHER:
		return thread_config->packer_thread;

	case RESUME_PHASE_DATA_VIOS:
		return thread_config->cpu_thread;
	default:
		return thread_config->admin_thread;
	}
}

/**
 * write_super_block() - Update the VDO state and save the super block.
 * @vdo: The vdo being resumed.
 * @completion: The admin_completion's sub-task completion.
 */
static void write_super_block(struct vdo *vdo,
			      struct vdo_completion *completion)
{
	switch (vdo_get_state(vdo)) {
	case VDO_CLEAN:
	case VDO_NEW:
		vdo_set_state(vdo, VDO_DIRTY);
		vdo_save_components(vdo, completion);
		return;

	case VDO_DIRTY:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		/* No need to write the super block in these cases */
		vdo_complete_completion(completion);
		return;

	case VDO_REPLAYING:
	default:
		vdo_finish_completion(completion, UDS_BAD_STATE);
	}
}

/**
 * resume_callback() - Callback to resume a VDO.
 * @completion: The sub-task completion.
 */
static void resume_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_RESUME);
	vdo_assert_admin_phase_thread(admin_completion, __func__,
				      RESUME_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case RESUME_PHASE_START:
		if (vdo_start_resuming(&vdo->admin_state,
				       VDO_ADMIN_STATE_RESUMING,
				       &admin_completion->completion,
				       NULL)) {
			write_super_block(vdo, completion);
		}
		return;

	case RESUME_PHASE_ALLOW_READ_ONLY_MODE:
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier,
					       vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_DEDUPE:
		vdo_resume_hash_zones(vdo->hash_zones,
				      vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_DEPOT:
		vdo_resume_slab_depot(vdo->depot, vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_JOURNAL:
		vdo_resume_recovery_journal(vdo->recovery_journal,
					    vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_BLOCK_MAP:
		vdo_resume_block_map(vdo->block_map,
				     vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_LOGICAL_ZONES:
		vdo_resume_logical_zones(vdo->logical_zones,
					 vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_PACKER:
	{
		bool was_enabled = vdo_get_compressing(vdo);
		bool enable = vdo->device_config->compression;

		if (enable != was_enabled) {
			WRITE_ONCE(vdo->compressing, enable);
		}
		uds_log_info("compression is %s",
			     (enable ? "enabled" : "disabled"));

		vdo_resume_packer(vdo->packer,
				  vdo_reset_admin_sub_task(completion));
		return;
	}

	case RESUME_PHASE_FLUSHER:
		vdo_resume_flusher(vdo->flusher,
				   vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_DATA_VIOS:
		resume_data_vio_pool(vdo->data_vio_pool,
				     vdo_reset_admin_sub_task(completion));
		return;

	case RESUME_PHASE_END:
		break;

	default:
		vdo_set_completion_result(vdo_reset_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	vdo_finish_resuming_with_result(&vdo->admin_state, completion->result);
}

/**
 * apply_new_vdo_configuration() - Attempt to make any configuration changes
 *                                 from the table being resumed.
 * @vdo: The vdo being resumed.
 * @config: The new device configuration derived from the table with which
 *          the vdo is being resumed.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
apply_new_vdo_configuration(struct vdo *vdo, struct device_config *config)
{
	int result;

	result = vdo_perform_grow_logical(vdo, config->logical_blocks);
	if (result != VDO_SUCCESS) {
		uds_log_error("grow logical operation failed, result = %d",
			      result);
		return result;
	}

	result = vdo_perform_grow_physical(vdo, config->physical_blocks);
	if (result != VDO_SUCCESS) {
		uds_log_error("resize operation failed, result = %d", result);
	}

	return result;
}

/**
 * vdo_preresume_internal() - Resume a suspended vdo (technically preresume
 *                            because resume can't fail).
 * @vdo: The vdo being resumed.
 * @config: The device config derived from the table with which the vdo is
 *          being resumed.
 * @device_name: The vdo device name (for logging).
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_preresume_internal(struct vdo *vdo,
			   struct device_config *config,
			   const char *device_name)
{
	int result;

	/*
         * If this fails, the VDO was not in a state to be resumed. This should
	 * never happen.
	 */
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

	if (vdo_get_admin_state(vdo)->normal) {
		/* The VDO was just started, so we don't need to resume it. */
		return VDO_SUCCESS;
	}

	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_RESUME,
					     get_thread_id_for_phase,
					     resume_callback,
					     vdo_preserve_completion_error_and_continue);
	BUG_ON(result == VDO_INVALID_ADMIN_STATE);
	if (result == VDO_READ_ONLY) {
		/* Even if the vdo is read-only, it has still resumed. */
		result = VDO_SUCCESS;
	}

	if (result != VDO_SUCCESS) {
		uds_log_error("resume of device '%s' failed with error: %d",
			      device_name,
			      result);
	}

	return result;
}
