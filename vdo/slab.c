// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "completion.h"
#include "constants.h"
#include "num-utils.h"
#include "pbn-lock.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab-journal-format.h"
#include "slab-summary.h"

/**
 * vdo_make_slab() - Construct a new, empty slab.
 * @slab_origin: The physical block number within the block allocator
 *               partition of the first block in the slab.
 * @allocator: The block allocator to which the slab belongs.
 * @translation: The translation from the depot's partition to the 
 *               physical storage.
 * @recovery_journal: The recovery journal of the VDO.
 * @slab_number: The slab number of the slab.
 * @is_new: true if this slab is being allocated as part of a resize.
 * @slab_ptr: A pointer to receive the new slab.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_make_slab(physical_block_number_t slab_origin,
		  struct block_allocator *allocator,
		  physical_block_number_t translation,
		  struct recovery_journal *recovery_journal,
		  slab_count_t slab_number,
		  bool is_new,
		  struct vdo_slab **slab_ptr)
{
	const struct slab_config *slab_config =
		vdo_get_slab_config(allocator->depot);

	struct vdo_slab *slab;
	int result = UDS_ALLOCATE(1, struct vdo_slab, __func__, &slab);

	if (result != VDO_SUCCESS) {
		return result;
	}

	slab->allocator = allocator;
	slab->start = slab_origin;
	slab->end = slab->start + slab_config->slab_blocks;
	slab->slab_number = slab_number;
	INIT_LIST_HEAD(&slab->allocq_entry);

	slab->ref_counts_origin =
		slab_origin + slab_config->data_blocks + translation;
	slab->journal_origin =
		(vdo_get_slab_journal_start_block(slab_config, slab_origin)
		 + translation);

	result = vdo_make_slab_journal(allocator, slab, recovery_journal,
				       &slab->journal);
	if (result != VDO_SUCCESS) {
		vdo_free_slab(slab);
		return result;
	}

	if (is_new) {
		vdo_set_admin_state_code(&slab->state, VDO_ADMIN_STATE_NEW);
		result = vdo_allocate_ref_counts_for_slab(slab);
		if (result != VDO_SUCCESS) {
			vdo_free_slab(slab);
			return result;
		}
	} else {
		vdo_set_admin_state_code(&slab->state,
					 VDO_ADMIN_STATE_NORMAL_OPERATION);
	}

	*slab_ptr = slab;
	return VDO_SUCCESS;
}

/**
 * vdo_allocate_ref_counts_for_slab() - Allocate the reference counts for a
 *                                      slab.
 * @slab: The slab whose reference counts need allocation.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_allocate_ref_counts_for_slab(struct vdo_slab *slab)
{
	struct block_allocator *allocator = slab->allocator;
	const struct slab_config *slab_config
		= vdo_get_slab_config(allocator->depot);

	int result = ASSERT(slab->reference_counts == NULL,
			    "vdo_slab %u doesn't allocate refcounts twice",
			    slab->slab_number);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_make_ref_counts(slab_config->data_blocks,
				   slab,
				   slab->ref_counts_origin,
				   allocator->read_only_notifier,
				   &slab->reference_counts);
}

/**
 * vdo_free_slab() - Destroy a slab.
 * @slab: The slab to destroy.
 */
void vdo_free_slab(struct vdo_slab *slab)
{
	if (slab == NULL) {
		return;
	}

	list_del(&slab->allocq_entry);
	vdo_free_slab_journal(UDS_FORGET(slab->journal));
	vdo_free_ref_counts(UDS_FORGET(slab->reference_counts));
	UDS_FREE(slab);
}

/**
 * vdo_get_slab_zone_number() - Get the physical zone number of a slab.
 * @slab: The slab.
 *
 * Return: The number of the slab's physical zone.
 */
zone_count_t vdo_get_slab_zone_number(struct vdo_slab *slab)
{
	return slab->allocator->zone_number;
}

/**
 * vdo_mark_slab_replaying() - Mark a slab as replaying, during offline
 *                             recovery.
 * @slab: The slab to mark.
 */
void vdo_mark_slab_replaying(struct vdo_slab *slab)
{
	if (slab->status == VDO_SLAB_REBUILT) {
		slab->status = VDO_SLAB_REPLAYING;
	}
}

/**
 * vdo_mark_slab_unrecovered() - Mark a slab as unrecovered, for online
 *                               recovery.
 * @slab: The slab to mark.
 */
void vdo_mark_slab_unrecovered(struct vdo_slab *slab)
{
	slab->status = VDO_SLAB_REQUIRES_SCRUBBING;
}

/**
 * get_slab_free_block_count() - Get the current number of free blocks in a
 *                               slab.
 * @slab: The slab to query.
 *
 * Return: The number of free blocks in the slab.
 */
block_count_t get_slab_free_block_count(const struct vdo_slab *slab)
{
	return vdo_get_unreferenced_block_count(slab->reference_counts);
}

/**
 * vdo_modify_slab_reference_count() - Increment or decrement the reference
 *                                     count of a block in a slab.
 * @slab: The slab containing the block (may be NULL when referencing the zero
 *        block).
 * @journal_point: The slab journal entry corresponding to this change.
 * @operation: The operation to perform on the reference count.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_modify_slab_reference_count(struct vdo_slab *slab,
				    const struct journal_point *journal_point,
				    struct reference_operation operation)
{
	bool free_status_changed;
	int result;

	if (slab == NULL) {
		return VDO_SUCCESS;
	}

	/*
	 * If the slab is unrecovered, preserve the refCount state and let
	 * scrubbing correct the refCount. Note that the slab journal has
	 * already captured all refCount updates.
	 */
	if (vdo_is_unrecovered_slab(slab)) {
		sequence_number_t entry_lock = journal_point->sequence_number;

		vdo_adjust_slab_journal_block_reference(slab->journal,
							entry_lock,
							-1);
		return VDO_SUCCESS;
	}

	result = vdo_adjust_reference_count(slab->reference_counts, operation,
					    journal_point,
					    &free_status_changed);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (free_status_changed) {
		vdo_adjust_free_block_count(slab,
					    !vdo_is_journal_increment_operation(operation.type));
	}

	return VDO_SUCCESS;
}

/**
 * vdo_open_slab() - Perform all necessary initialization of a slab necessary
 *                   for allocations.
 * @slab: The slab.
 */
void vdo_open_slab(struct vdo_slab *slab)
{
	vdo_reset_search_cursor(slab->reference_counts);
	if (vdo_is_slab_journal_blank(slab->journal)) {
		WRITE_ONCE(slab->allocator->statistics.slabs_opened,
			   slab->allocator->statistics.slabs_opened + 1);
		vdo_dirty_all_reference_blocks(slab->reference_counts);
	} else {
		WRITE_ONCE(slab->allocator->statistics.slabs_reopened,
			   slab->allocator->statistics.slabs_reopened + 1);
	}
}

/**
 * vdo_acquire_provisional_reference() - Acquire a provisional reference on
 *                                       behalf of a PBN lock if the block it
 *                                       locks is unreferenced.
 * @slab: The slab which contains the block.
 * @pbn: The physical block to reference.
 * @lock: The lock.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_acquire_provisional_reference(struct vdo_slab *slab,
				      physical_block_number_t pbn,
				      struct pbn_lock *lock)
{
	int result;

	if (vdo_pbn_lock_has_provisional_reference(lock)) {
		return VDO_SUCCESS;
	}

	result = vdo_provisionally_reference_block(slab->reference_counts,
						   pbn, lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (vdo_pbn_lock_has_provisional_reference(lock)) {
		vdo_adjust_free_block_count(slab, false);
	}

	return VDO_SUCCESS;
}

/**
 * vdo_slab_block_number_from_pbn() - Determine the index within the slab of a
 *                                    particular physical block number.
 * @slab: The slab.
 * @physical_block_number: The physical block number.
 * @slab_block_number_ptr: A pointer to the slab block number.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_slab_block_number_from_pbn(struct vdo_slab *slab,
				   physical_block_number_t physical_block_number,
				   slab_block_number *slab_block_number_ptr)
{
	uint64_t slab_block_number;

	if (physical_block_number < slab->start) {
		return VDO_OUT_OF_RANGE;
	}

	slab_block_number = physical_block_number - slab->start;
	if (slab_block_number
	    >= vdo_get_slab_config(slab->allocator->depot)->data_blocks) {
		return VDO_OUT_OF_RANGE;
	}

	*slab_block_number_ptr = slab_block_number;
	return VDO_SUCCESS;
}

/**
 * vdo_should_save_fully_built_slab() - Check whether the reference counts for
 *                                      a given rebuilt slab should be saved.
 * @slab: The slab to check.
 *
 * Return: true if the slab should be saved.
 */
bool vdo_should_save_fully_built_slab(const struct vdo_slab *slab)
{
	/*
	 * Write out the ref_counts if the slab has written them before, or it
	 * has any non-zero reference counts, or there are any slab journal
	 * blocks.
	 */
	block_count_t data_blocks =
		vdo_get_slab_config(slab->allocator->depot)->data_blocks;
	return (vdo_must_load_ref_counts(slab->allocator->summary,
					 slab->slab_number)
		|| (get_slab_free_block_count(slab) != data_blocks)
		|| !vdo_is_slab_journal_blank(slab->journal));
}

/**
 * initiate_slab_action() - Initiate a slab action.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_slab_action(struct admin_state *state)
{
	struct vdo_slab *slab = container_of(state, struct vdo_slab, state);

	if (vdo_is_state_draining(state)) {
		const struct admin_state_code *operation =
			vdo_get_admin_state_code(state);
		if (operation == VDO_ADMIN_STATE_SCRUBBING) {
			slab->status = VDO_SLAB_REBUILDING;
		}

		vdo_drain_slab_journal(slab->journal);

		if (slab->reference_counts != NULL) {
			vdo_drain_ref_counts(slab->reference_counts);
		}

		vdo_check_if_slab_drained(slab);
		return;
	}

	if (vdo_is_state_loading(state)) {
		vdo_decode_slab_journal(slab->journal);
		return;
	}

	if (vdo_is_state_resuming(state)) {
		vdo_queue_slab(slab);
		vdo_finish_resuming(state);
		return;
	}

	vdo_finish_operation(state, VDO_INVALID_ADMIN_STATE);
}

/**
 * vdo_start_slab_action() - Start an administrative operation on a slab.
 * @slab: The slab to load.
 * @operation: The type of load to perform.
 * @parent: The object to notify when the operation is complete.
 */
void vdo_start_slab_action(struct vdo_slab *slab,
			   const struct admin_state_code *operation,
			   struct vdo_completion *parent)
{
	vdo_start_operation_with_waiter(&slab->state, operation, parent,
					initiate_slab_action);
}

/**
 * vdo_notify_slab_journal_is_loaded() - Inform a slab that its journal has
 *                                       been loaded.
 * @slab: The slab whose journal has been loaded.
 * @result: The result of the load operation.
 */
void vdo_notify_slab_journal_is_loaded(struct vdo_slab *slab, int result)
{
	if ((result == VDO_SUCCESS) && vdo_is_state_clean_load(&slab->state)) {
		/*
		 * Since this is a normal or new load, we don't need the memory
		 * to read and process the recovery journal, so we can allocate
		 * reference counts now.
		 */
		result = vdo_allocate_ref_counts_for_slab(slab);
	}

	vdo_finish_loading_with_result(&slab->state, result);
}

/**
 * vdo_is_slab_open() - Check whether a slab is open, i.e. is neither
 *                      quiescent nor quiescing.
 * @slab: The slab to check.
 *
 * Return: true if the slab is open.
 */
bool vdo_is_slab_open(struct vdo_slab *slab)
{
	return (!vdo_is_state_quiescing(&slab->state) &&
		!vdo_is_state_quiescent(&slab->state));
}

/**
 * vdo_is_slab_draining() - Check whether a slab is currently draining.
 * @slab: The slab to check.
 *
 * Return: true if the slab is performing a drain operation.
 */
bool vdo_is_slab_draining(struct vdo_slab *slab)
{
	return vdo_is_state_draining(&slab->state);
}

/**
 * vdo_check_if_slab_drained() - Check whether a slab has drained, and if so,
 *                               send a notification thereof.
 * @slab: The slab to check.
 */
void vdo_check_if_slab_drained(struct vdo_slab *slab)
{
	if (vdo_is_state_draining(&slab->state) &&
	    !vdo_is_slab_journal_active(slab->journal) &&
	    ((slab->reference_counts == NULL) ||
	     !vdo_are_ref_counts_active(slab->reference_counts))) {
		int result = (vdo_is_read_only(slab->allocator->read_only_notifier)
				      ? VDO_READ_ONLY
				      : VDO_SUCCESS);
		vdo_finish_draining_with_result(&slab->state, result);
	}
}

/**
 * vdo_notify_slab_ref_counts_are_drained() - Inform a slab that its
 *                                            ref_counts have finished
 *                                            draining.
 * @slab: The slab whose ref_counts object has been drained.
 * @result: The result of the drain operation.
 */
void vdo_notify_slab_ref_counts_are_drained(struct vdo_slab *slab, int result)
{
	vdo_finish_draining_with_result(&slab->state, result);
}

/**
 * vdo_is_slab_resuming() - Check whether a slab is currently resuming.
 * @slab: The slab to check.
 *
 * Return: true if the slab is performing a resume operation.
 */
bool vdo_is_slab_resuming(struct vdo_slab *slab)
{
	return vdo_is_state_resuming(&slab->state);
}

/**
 * vdo_finish_scrubbing_slab() - Finish scrubbing a slab.
 * @slab: The slab whose reference counts have been rebuilt from its journal.
 *
 * Finishes scrubbing a slab now that it has been rebuilt by updating its
 * status, queueing it for allocation, and reopening its journal.
 */
void vdo_finish_scrubbing_slab(struct vdo_slab *slab)
{
	slab->status = VDO_SLAB_REBUILT;
	vdo_queue_slab(slab);
	vdo_reopen_slab_journal(slab->journal);
}

static const char *status_to_string(enum slab_rebuild_status status)
{
	switch (status) {
	case VDO_SLAB_REBUILT:
		return "REBUILT";
	case VDO_SLAB_REQUIRES_SCRUBBING:
		return "SCRUBBING";
	case VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING:
		return "PRIORITY_SCRUBBING";
	case VDO_SLAB_REBUILDING:
		return "REBUILDING";
	case VDO_SLAB_REPLAYING:
		return "REPLAYING";
	default:
		return "UNKNOWN";
	}
}

/**
 * vdo_dump_slab() - Dump information about a slab to the log for debugging.
 * @slab: The slab to dump.
 */
void vdo_dump_slab(const struct vdo_slab *slab)
{
	if (slab->reference_counts != NULL) {
		/*
		 * Terse because there are a lot of slabs to dump and syslog is
		 * lossy.
		 */
		uds_log_info("slab %u: P%u, %llu free",
			     slab->slab_number,
			     slab->priority,
			     (unsigned long long) get_slab_free_block_count(slab));
	} else {
		uds_log_info("slab %u: status %s", slab->slab_number,
			     status_to_string(slab->status));
	}

	vdo_dump_slab_journal(slab->journal);

	if (slab->reference_counts != NULL) {
		vdo_dump_ref_counts(slab->reference_counts);
	} else {
		uds_log_info("refCounts is null");
	}
}
