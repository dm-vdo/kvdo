// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-resize.h"

#include "logger.h"

#include "admin-completion.h"
#include "completion.h"
#include "kernel-types.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vdo-layout.h"

enum {
	GROW_PHYSICAL_PHASE_START,
	GROW_PHYSICAL_PHASE_COPY_SUMMARY,
	GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS,
	GROW_PHYSICAL_PHASE_USE_NEW_SLABS,
	GROW_PHYSICAL_PHASE_END,
	GROW_PHYSICAL_PHASE_ERROR,
};

static const char *GROW_PHYSICAL_PHASE_NAMES[] = {
	"GROW_PHYSICAL_PHASE_START",
	"GROW_PHYSICAL_PHASE_COPY_SUMMARY",
	"GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS",
	"GROW_PHYSICAL_PHASE_USE_NEW_SLABS",
	"GROW_PHYSICAL_PHASE_END",
	"GROW_PHYSICAL_PHASE_ERROR",
};

/**
 * get_thread_id_for_phase() - Implements vdo_thread_id_getter_for_phase.
 */
static thread_id_t __must_check
get_thread_id_for_phase(struct admin_completion *admin_completion)
{
	return admin_completion->vdo->thread_config->admin_thread;
}

/**
 * grow_physical_callback() - Callback to initiate a grow physical.
 * @completion: The sub-task completion.
 *
 * Registered in vdo_perform_grow_physical().
 */
static void grow_physical_callback(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_GROW_PHYSICAL);
	vdo_assert_admin_phase_thread(admin_completion, __func__,
				      GROW_PHYSICAL_PHASE_NAMES);

	switch (admin_completion->phase++) {
	case GROW_PHYSICAL_PHASE_START:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			uds_log_error_strerror(VDO_READ_ONLY,
					       "Can't grow physical size of a read-only VDO");
			vdo_set_completion_result(vdo_reset_admin_sub_task(completion),
						  VDO_READ_ONLY);
			break;
		}

		if (vdo_start_operation_with_waiter(&vdo->admin_state,
						    VDO_ADMIN_STATE_SUSPENDED_OPERATION,
						    &admin_completion->completion,
						    NULL)) {
			/* Copy the journal into the new layout. */
			vdo_copy_layout_partition(vdo->layout,
						  VDO_RECOVERY_JOURNAL_PARTITION,
						  vdo_reset_admin_sub_task(completion));
		}
		return;

	case GROW_PHYSICAL_PHASE_COPY_SUMMARY:
		vdo_copy_layout_partition(vdo->layout,
					  VDO_SLAB_SUMMARY_PARTITION,
					  vdo_reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS:
		vdo->states.vdo.config.physical_blocks =
			vdo_grow_layout(vdo->layout);
		vdo_update_slab_depot_size(vdo->depot);
		vdo_save_components(vdo, vdo_reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_USE_NEW_SLABS:
		vdo_use_new_slabs(vdo->depot, vdo_reset_admin_sub_task(completion));
		return;

	case GROW_PHYSICAL_PHASE_END:
		vdo_set_slab_summary_origin(vdo_get_slab_summary(vdo->depot),
					    vdo_get_partition(vdo->layout,
							      VDO_SLAB_SUMMARY_PARTITION));
		vdo_set_recovery_journal_partition(vdo->recovery_journal,
						   vdo_get_partition(vdo->layout,
								     VDO_RECOVERY_JOURNAL_PARTITION));
		break;

	case GROW_PHYSICAL_PHASE_ERROR:
		vdo_enter_read_only_mode(vdo->read_only_notifier,
					 completion->result);
		break;

	default:
		vdo_set_completion_result(vdo_reset_admin_sub_task(completion),
					  UDS_BAD_STATE);
	}

	vdo_finish_layout_growth(vdo->layout);
	vdo_finish_operation(&vdo->admin_state, completion->result);
}

/**
 * handle_growth_error() - Handle an error during the grow physical process.
 * @completion: The sub-task completion.
 */
static void handle_growth_error(struct vdo_completion *completion)
{
	vdo_admin_completion_from_sub_task(completion)->phase =
		GROW_PHYSICAL_PHASE_ERROR;
	grow_physical_callback(completion);
}

/**
 * vdo_perform_grow_physical() - Grow the physical size of the vdo.
 * @vdo: The vdo to resize.
 * @new_physical_blocks: The new physical size in blocks.
 *
 * Context: This method may only be called when the vdo has been suspended and
 * must not be called from a base thread.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_perform_grow_physical(struct vdo *vdo,
			      block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size, prepared_depot_size;

	block_count_t old_physical_blocks =
		vdo->states.vdo.config.physical_blocks;

	/* Skip any noop grows. */
	if (old_physical_blocks == new_physical_blocks) {
		return VDO_SUCCESS;
	}

	if (new_physical_blocks != vdo_get_next_layout_size(vdo->layout)) {
		/*
		 * Either the VDO isn't prepared to grow, or it was prepared to
		 * grow to a different size. Doing this check here relies on
		 * the fact that the call to this method is done under the
		 * dmsetup message lock.
		 */
		vdo_finish_layout_growth(vdo->layout);
		vdo_abandon_new_slabs(vdo->depot);
		return VDO_PARAMETER_MISMATCH;
	}

	/* Validate that we are prepared to grow appropriately. */
	new_depot_size =
		vdo_get_next_block_allocator_partition_size(vdo->layout);
	prepared_depot_size = vdo_get_slab_depot_new_size(vdo->depot);
	if (prepared_depot_size != new_depot_size) {
		return VDO_PARAMETER_MISMATCH;
	}

	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_GROW_PHYSICAL,
					     get_thread_id_for_phase,
					     grow_physical_callback,
					     handle_growth_error);
	if (result != VDO_SUCCESS) {
		return result;
	}

	uds_log_info("Physical block count was %llu, now %llu",
		     (unsigned long long) old_physical_blocks,
		     (unsigned long long) new_physical_blocks);
	return VDO_SUCCESS;
}

/**
 * check_may_grow_physical() - Callback to check that we're not in recovery
 *                             mode, used in vdo_prepare_to_grow_physical().
 * @completion: The sub-task completion.
 */
static void check_may_grow_physical(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	struct vdo *vdo = admin_completion->vdo;

	vdo_assert_admin_operation_type(admin_completion,
					VDO_ADMIN_OPERATION_PREPARE_GROW_PHYSICAL);
	vdo_assert_on_admin_thread(vdo, __func__);

	vdo_reset_admin_sub_task(completion);

	/* This check can only be done from a base code thread. */
	if (vdo_is_read_only(vdo->read_only_notifier)) {
		vdo_finish_completion(completion->parent, VDO_READ_ONLY);
		return;
	}

	/* This check should only be done from a base code thread. */
	if (vdo_in_recovery_mode(vdo)) {
		vdo_finish_completion(completion->parent, VDO_RETRY_AFTER_REBUILD);
		return;
	}

	vdo_complete_completion(completion->parent);
}

/**
 * vdo_prepare_to_grow_physical() - Prepare to resize the vdo, allocating
 *                                  memory as needed.
 * @vdo: The vdo.
 * @new_physical_blocks: The new physical size in blocks.
 */
int vdo_prepare_to_grow_physical(struct vdo *vdo,
				 block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size;
	block_count_t current_physical_blocks =
		vdo->states.vdo.config.physical_blocks;

	uds_log_info("Preparing to resize physical to %llu",
		     new_physical_blocks);
	ASSERT_LOG_ONLY((new_physical_blocks > current_physical_blocks),
			"New physical size is larger than current physical size");
	result = vdo_perform_admin_operation(vdo,
					     VDO_ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
					     get_thread_id_for_phase,
					     check_may_grow_physical,
					     vdo_finish_completion_parent_callback);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = prepare_to_vdo_grow_layout(vdo->layout,
					    current_physical_blocks,
					    new_physical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	new_depot_size =
		vdo_get_next_block_allocator_partition_size(vdo->layout);
	result = vdo_prepare_to_grow_slab_depot(vdo->depot, new_depot_size);
	if (result != VDO_SUCCESS) {
		vdo_finish_layout_growth(vdo->layout);
		return result;
	}

	uds_log_info("Done preparing to resize physical");
	return VDO_SUCCESS;
}
