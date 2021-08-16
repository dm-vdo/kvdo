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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoRecovery.c#112 $
 */

#include "vdoRecoveryInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockAllocator.h"
#include "blockAllocatorInternals.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapRecovery.h"
#include "completion.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournal.h"
#include "recoveryUtils.h"
#include "slab.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "slabJournalInternals.h"
#include "threadConfig.h"
#include "vdoInternal.h"
#include "waitQueue.h"

enum {
	// The int map needs capacity of twice the number of VIOs in the system.
	INT_MAP_CAPACITY = MAXIMUM_VDO_USER_VIOS * 2,
	// There can be as many missing decrefs as there are VIOs in the system.
	MAXIMUM_SYNTHESIZED_DECREFS = MAXIMUM_VDO_USER_VIOS,
};

struct missing_decref {
	/** A waiter for queueing this object */
	struct waiter waiter;
	/** The parent of this object */
	struct recovery_completion *recovery;
	/** Whether this decref is complete */
	bool complete;
	/** The slot for which the last decref was lost */
	struct block_map_slot slot;
	/** The penultimate block map entry for this LBN */
	struct data_location penultimate_mapping;
	/** The page completion used to fetch the block map page for this LBN */
	struct vdo_page_completion page_completion;
	/** The journal point which will be used for this entry */
	struct journal_point journal_point;
	/** The slab journal to which this entry will be applied */
	struct slab_journal *slab_journal;
};

/**
 * Convert a waiter to the missing decref of which it is a part.
 *
 * @param waiter  The waiter to convert
 *
 * @return The missing_decref wrapping the waiter
 **/
static inline struct missing_decref * __must_check
as_missing_decref(struct waiter *waiter)
{
	return container_of(waiter, struct missing_decref, waiter);
}

/**
 * Enqueue a missing_decref. If the enqueue fails, enter read-only mode.
 *
 * @param queue   The queue on which to enqueue the decref
 * @param decref  The missing_decref to enqueue
 *
 * @return VDO_SUCCESS or an error
 **/
static int enqueue_missing_decref(struct wait_queue *queue,
				  struct missing_decref *decref)
{
	int result = enqueue_waiter(queue, &decref->waiter);

	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(decref->recovery->vdo->read_only_notifier,
					 result);
		set_vdo_completion_result(&decref->recovery->completion, result);
		UDS_FREE(decref);
	}

	return result;
}

/**
 * Convert a block_map_slot into a unique uint64_t.
 *
 * @param slot  The block map slot to convert.
 *
 * @return a one-to-one mappable uint64_t.
 **/
static uint64_t slot_as_number(struct block_map_slot slot)
{
	return (((uint64_t) slot.pbn << 10) + slot.slot);
}

/**
 * Create a missing_decref and enqueue it to wait for a determination of its
 * penultimate mapping.
 *
 * @param [in]  recovery    The parent recovery completion
 * @param [in]  entry       The recovery journal entry for the increment which
 *                          is missing a decref
 * @param [out] decref_ptr  A pointer to hold the new missing_decref
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
make_missing_decref(struct recovery_completion *recovery,
		    struct recovery_journal_entry entry,
		    struct missing_decref **decref_ptr)
{
	struct missing_decref *decref;
	int result = UDS_ALLOCATE(1, struct missing_decref, __func__, &decref);

	if (result != VDO_SUCCESS) {
		return result;
	}

	decref->recovery = recovery;
	result = enqueue_missing_decref(&recovery->missing_decrefs[0], decref);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * Each synthsized decref needs a unique journal point. Otherwise, in
	 * the event of a crash, we would be unable to tell which synthesized
	 * decrefs had already been committed in the slab journals. Instead of
	 * using real recovery journal space for this, we can use fake journal
	 * points between the last currently valid entry in the tail block and
	 * the first journal entry in the next block. We can't overflow the
	 * entry count since the number of synthesized decrefs is bounded by
	 * the data VIO limit.
	 *
	 * It is vital that any given missing decref always have the same fake
	 * journal point since a failed recovery may be retried with a
	 * different number of zones after having written out some slab
	 * journal blocks. Since the missing decrefs are always read out of
	 * the journal in the same order, we can assign them a journal point
	 * when they are read. Their subsequent use will ensure that, for any
	 * given slab journal, they are applied in the order dictated by these
	 * assigned journal points.
	 */
	decref->slot = entry.slot;
	decref->journal_point = recovery->next_synthesized_journal_point;
	recovery->next_synthesized_journal_point.entry_count++;
	recovery->missing_decref_count++;
	recovery->incomplete_decref_count++;

	*decref_ptr = decref;
	return VDO_SUCCESS;
}

/**
 * Move the given recovery point forward by one entry.
 *
 * @param point  The recovery point to alter
 **/
static void increment_recovery_point(struct recovery_point *point)
{
	point->entry_count++;
	if ((point->sector_count == (VDO_SECTORS_PER_BLOCK - 1)) &&
	    (point->entry_count == RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR)) {
		point->sequence_number++;
		point->sector_count = 1;
		point->entry_count = 0;
	}

	if (point->entry_count == RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) {
		point->sector_count++;
		point->entry_count = 0;
		return;
	}
}

/**
 * Move the given recovery point backwards by one entry.
 *
 * @param point  The recovery point to alter
 **/
static void decrement_recovery_point(struct recovery_point *point)
{
	STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR > 0);

	if ((point->sector_count <= 1) && (point->entry_count == 0)) {
		point->sequence_number--;
		point->sector_count = VDO_SECTORS_PER_BLOCK - 1;
		point->entry_count =
			RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR - 1;
		return;
	}

	if (point->entry_count == 0) {
		point->sector_count--;
		point->entry_count = RECOVERY_JOURNAL_ENTRIES_PER_SECTOR - 1;
		return;
	}

	point->entry_count--;
}

/**
 * Check whether the first point precedes the second point.
 *
 * @param first   The first recovery point
 * @param second  The second recovery point
 *
 * @return <code>true</code> if the first point precedes the second point
 **/
static bool __must_check
before_recovery_point(const struct recovery_point *first,
		      const struct recovery_point *second)
{
	if (first->sequence_number < second->sequence_number) {
		return true;
	}

	if (first->sequence_number > second->sequence_number) {
		return false;
	}

	if (first->sector_count < second->sector_count) {
		return true;
	}

	return ((first->sector_count == second->sector_count) &&
		(first->entry_count < second->entry_count));
}

/**
 * Prepare the sub-task completion.
 *
 * @param recovery       The recovery_completion whose sub-task completion is
 *                       to be prepared
 * @param callback       The callback to register for the next sub-task
 * @param error_handler  The error handler for the next sub-task
 * @param zone_type      The type of zone on which the callback or
 *                       error_handler should run
 **/
static void prepare_sub_task(struct recovery_completion *recovery,
			     vdo_action callback,
			     vdo_action error_handler,
			     enum vdo_zone_type zone_type)
{
	const struct thread_config *thread_config =
		get_vdo_thread_config(recovery->vdo);
	thread_id_t thread_id;

	switch (zone_type) {
	case VDO_ZONE_TYPE_LOGICAL:
		// All blockmap access is done on single thread, so use logical
		// zone 0.
		thread_id = vdo_get_logical_zone_thread(thread_config, 0);
		break;

	case VDO_ZONE_TYPE_PHYSICAL:
		thread_id = recovery->allocator->thread_id;
		break;

	case VDO_ZONE_TYPE_ADMIN:
	default:
		thread_id = thread_config->admin_thread;
	}

	prepare_vdo_completion(&recovery->sub_task_completion,
			       callback,
			       error_handler,
			       thread_id,
			       &recovery->completion);
}

/**********************************************************************/
int make_vdo_recovery_completion(struct vdo *vdo,
				 struct recovery_completion **recovery_ptr)
{
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);
	struct recovery_completion *recovery;
	zone_count_t z;
	int result = UDS_ALLOCATE_EXTENDED(struct recovery_completion,
					   thread_config->physical_zone_count,
					   struct list_head,
					   __func__,
					   &recovery);
	if (result != VDO_SUCCESS) {
		return result;
	}

	recovery->vdo = vdo;
	for (z = 0; z < thread_config->physical_zone_count; z++) {
		initialize_wait_queue(&recovery->missing_decrefs[z]);
	}

	initialize_vdo_completion(&recovery->completion, vdo,
				  VDO_RECOVERY_COMPLETION);
	initialize_vdo_completion(&recovery->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);

	result = make_int_map(INT_MAP_CAPACITY, 0, &recovery->slot_entry_map);
	if (result != VDO_SUCCESS) {
		free_vdo_recovery_completion(recovery);
		return result;
	}

	*recovery_ptr = recovery;
	return VDO_SUCCESS;
}

/**
 * A waiter callback to free missing_decrefs.
 *
 * Implements waiter_callback.
 **/
static void free_missing_decref(struct waiter *waiter,
				void *context __always_unused)
{
	UDS_FREE(as_missing_decref(waiter));
}

/**********************************************************************/
void free_vdo_recovery_completion(struct recovery_completion *recovery)
{
	const struct thread_config *thread_config;
	zone_count_t z;

	if (recovery == NULL) {
		return;
	}

	free_int_map(UDS_FORGET(recovery->slot_entry_map));
	thread_config = get_vdo_thread_config(recovery->vdo);
	for (z = 0; z < thread_config->physical_zone_count; z++) {
		notify_all_waiters(&recovery->missing_decrefs[z],
				   free_missing_decref, NULL);
	}

	UDS_FREE(UDS_FORGET(recovery->journal_data));
	UDS_FREE(UDS_FORGET(recovery->entries));
	UDS_FREE(recovery);
}

/**
 * Finish recovering, free the recovery completion and notify the parent.
 *
 * @param completion  The recovery completion
 **/
static void finish_recovery(struct vdo_completion *completion)
{
	int result;
	struct vdo_completion *parent = completion->parent;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion);
	struct vdo *vdo = recovery->vdo;
	uint64_t recovery_count = ++vdo->states.vdo.complete_recoveries;

	initialize_vdo_recovery_journal_post_recovery(vdo->recovery_journal,
						      recovery_count,
						      recovery->highest_tail);
	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_info("Rebuild complete");

	// Now that we've freed the recovery completion and its vast array of
	// journal entries, we can allocate refcounts.
	result = vdo_allocate_slab_ref_counts(vdo->depot);
	finish_vdo_completion(parent, result);
}

/**
 * Handle a recovery error.
 *
 * @param completion   The recovery completion
 **/
static void abort_recovery(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion);
	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_warning("Recovery aborted");
	finish_vdo_completion(parent, result);
}

/**
 * Abort a recovery if there is an error.
 *
 * @param result    The result to check
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if the result was an error
 **/
static bool __must_check
abort_recovery_on_error(int result, struct recovery_completion *recovery)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	finish_vdo_completion(&recovery->completion, result);
	return true;
}

/**
 * Unpack the recovery journal entry associated with the given recovery point.
 *
 * @param recovery  The recovery completion
 * @param point     The recovery point
 *
 * @return The unpacked contents of the matching recovery journal entry
 **/
static struct recovery_journal_entry
get_entry(const struct recovery_completion *recovery,
	  const struct recovery_point *point)
{
	struct recovery_journal *journal = recovery->vdo->recovery_journal;
	physical_block_number_t block_number =
		get_vdo_recovery_journal_block_number(journal,
						      point->sequence_number);
	off_t sector_offset = (block_number * VDO_BLOCK_SIZE) +
		(point->sector_count * VDO_SECTOR_SIZE);
	struct packed_journal_sector *sector =
		(struct packed_journal_sector *) &recovery->journal_data[sector_offset];
	return unpack_vdo_recovery_journal_entry(&sector->entries[point->entry_count]);
}

/**
 * Create an array of all valid journal entries, in order, and store it in the
 * recovery completion.
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int extract_journal_entries(struct recovery_completion *recovery)
{
	struct recovery_point recovery_point = {
		.sequence_number = recovery->block_map_head,
		.sector_count = 1,
		.entry_count = 0,
	};
	/*
	 * Allocate an array of numbered_block_mapping structs just large
	 * enough to transcribe every increment packed_recovery_journal_entry
	 * from every valid journal block.
	 */
	int result = UDS_ALLOCATE(recovery->incref_count,
				  struct numbered_block_mapping,
				  __func__,
				  &recovery->entries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	while (before_recovery_point(&recovery_point,
				     &recovery->tail_recovery_point)) {
		struct recovery_journal_entry entry =
			get_entry(recovery, &recovery_point);
		result = validate_vdo_recovery_journal_entry(recovery->vdo,
							     &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(recovery->vdo->read_only_notifier,
						 result);
			return result;
		}

		if (is_vdo_journal_increment_operation(entry.operation)) {
			recovery->entries[recovery->entry_count] =
				(struct numbered_block_mapping) {
					.block_map_slot = entry.slot,
					.block_map_entry =
						pack_vdo_pbn(entry.mapping.pbn,
							     entry.mapping.state),
					.number = recovery->entry_count,
				};
			recovery->entry_count++;
		}

		increment_recovery_point(&recovery_point);
	}

	result = ASSERT((recovery->entry_count <= recovery->incref_count),
			"approximate incref count is an upper bound");
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(recovery->vdo->read_only_notifier,
					 result);
	}

	return result;
}

/**
 * Extract journal entries and recover the block map. This callback is
 * registered in start_super_block_save().
 *
 * @param completion  The sub-task completion
 **/
static void launch_block_map_recovery(struct vdo_completion *completion)
{
	int result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	assert_on_logical_zone_thread(vdo, 0, __func__);

	// Extract the journal entries for the block map recovery.
	result = extract_journal_entries(recovery);
	if (abort_recovery_on_error(result, recovery)) {
		return;
	}

	prepare_vdo_completion_to_finish_parent(completion, &recovery->completion);
	recover_vdo_block_map(vdo, recovery->entry_count, recovery->entries,
			      completion);
}

/**
 * Finish flushing all slab journals and start a write of the super block.
 * This callback is registered in add_synthesized_entries().
 *
 * @param completion  The sub-task completion
 **/
static void start_super_block_save(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	assert_on_admin_thread(vdo, __func__);

	uds_log_info("Saving recovery progress");
	set_vdo_state(vdo, VDO_REPLAYING);

	// The block map access which follows the super block save must be done
	// on a logical thread.
	prepare_sub_task(recovery,
			 launch_block_map_recovery,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_LOGICAL);
	save_vdo_components(vdo, completion);
}

/**
 * The callback from loading the slab depot. It will update the logical blocks
 * and block map data blocks counts in the recovery journal and then drain the
 * slab depot in order to commit the recovered slab journals. It is registered
 * in apply_to_depot().
 *
 * @param completion  The sub-task completion
 **/
static void finish_recovering_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	assert_on_admin_thread(vdo, __func__);

	uds_log_info("Replayed %zu journal entries into slab journals",
		     recovery->entries_added_to_slab_journals);
	uds_log_info("Synthesized %zu missing journal entries",
		     recovery->missing_decref_count);
	vdo->recovery_journal->logical_blocks_used =
		recovery->logical_blocks_used;
	vdo->recovery_journal->block_map_data_blocks =
		recovery->block_map_data_blocks;

	prepare_sub_task(recovery,
			 start_super_block_save,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);
	drain_vdo_slab_depot(vdo->depot, VDO_ADMIN_STATE_RECOVERING, completion);
}

/**
 * The error handler for recovering slab journals. It will skip any remaining
 * recovery on the current zone and propagate the error. It is registered in
 * add_slab_journal_entries() and add_synthesized_entries().
 *
 * @param completion  The completion of the block allocator being recovered
 **/
static void
handle_add_slab_journal_entry_error(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	notify_vdo_slab_journals_are_recovered(recovery->allocator,
					       completion->result);
}

/**
 * Add synthesized entries into slab journals, waiting when necessary.
 *
 * @param completion  The allocator completion
 **/
static void add_synthesized_entries(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct wait_queue *missing_decrefs =
		&recovery->missing_decrefs[recovery->allocator->zone_number];

	// Get ready in case we need to enqueue again
	prepare_vdo_completion(completion,
			       add_synthesized_entries,
			       handle_add_slab_journal_entry_error,
			       completion->callback_thread_id,
			       &recovery->completion);
	while (has_waiters(missing_decrefs)) {
		struct missing_decref *decref =
			as_missing_decref(get_first_waiter(missing_decrefs));
		if (!attempt_replay_into_vdo_slab_journal(decref->slab_journal,
							  decref->penultimate_mapping.pbn,
							  VDO_JOURNAL_DATA_DECREMENT,
							  &decref->journal_point,
							  completion)) {
			return;
		}

		dequeue_next_waiter(missing_decrefs);
		UDS_FREE(decref);
	}

	notify_vdo_slab_journals_are_recovered(recovery->allocator,
					       VDO_SUCCESS);
}

/**
 * Determine the LBNs used count as of the end of the journal (but
 * not including any changes to that count from entries that will be
 * synthesized later).
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((__noinline__))
static int compute_usages(struct recovery_completion *recovery)
{
	// XXX VDO-5182: function is declared noinline to avoid what is likely
	// a spurious valgrind error about this structure being uninitialized.
	struct recovery_point recovery_point = {
		.sequence_number = recovery->tail,
		.sector_count = 1,
		.entry_count = 0,
	};
	struct recovery_journal *journal = recovery->vdo->recovery_journal;
	struct packed_journal_header *tail_header =
		get_vdo_recovery_journal_block_header(journal,
						      recovery->journal_data,
						      recovery->tail);

	struct recovery_block_header unpacked;

	unpack_vdo_recovery_block_header(tail_header, &unpacked);
	recovery->logical_blocks_used = unpacked.logical_blocks_used;
	recovery->block_map_data_blocks = unpacked.block_map_data_blocks;

	while (before_recovery_point(&recovery_point,
				     &recovery->tail_recovery_point)) {
		struct recovery_journal_entry entry =
			get_entry(recovery, &recovery_point);
		if (vdo_is_mapped_location(&entry.mapping)) {
			switch (entry.operation) {
			case VDO_JOURNAL_DATA_INCREMENT:
				recovery->logical_blocks_used++;
				break;

			case VDO_JOURNAL_DATA_DECREMENT:
				recovery->logical_blocks_used--;
				break;

			case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
				recovery->block_map_data_blocks++;
				break;

			default:
				return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
							      "Recovery journal entry at sequence number %llu, sector %u, entry %u had invalid operation %u",
							      (unsigned long long) recovery_point.sequence_number,
							      recovery_point.sector_count,
							      recovery_point.entry_count,
							      entry.operation);
			}
		}

		increment_recovery_point(&recovery_point);
	}

	return VDO_SUCCESS;
}

/**
 * Advance the current recovery and journal points.
 *
 * @param recovery           The recovery_completion whose points are to be
 *                           advanced
 * @param entries_per_block  The number of entries in a recovery journal block
 **/
static void advance_points(struct recovery_completion *recovery,
			   journal_entry_count_t entries_per_block)
{
	increment_recovery_point(&recovery->next_recovery_point);
	advance_vdo_journal_point(&recovery->next_journal_point,
				  entries_per_block);
}

/**
 * Replay recovery journal entries into the slab journals of the allocator
 * currently being recovered, waiting for slab journal tailblock space when
 * necessary. This method is its own callback.
 *
 * @param completion  The allocator completion
 **/
static void add_slab_journal_entries(struct vdo_completion *completion)
{
	struct recovery_point *recovery_point;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;

	// Get ready in case we need to enqueue again.
	prepare_vdo_completion(completion,
			       add_slab_journal_entries,
			       handle_add_slab_journal_entry_error,
			       completion->callback_thread_id,
			       &recovery->completion);
	for (recovery_point = &recovery->next_recovery_point;
	     before_recovery_point(recovery_point,
				   &recovery->tail_recovery_point);
	     advance_points(recovery, journal->entries_per_block)) {
		struct vdo_slab *slab;
		struct recovery_journal_entry entry =
			get_entry(recovery, recovery_point);
		int result = validate_vdo_recovery_journal_entry(vdo, &entry);

		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(journal->read_only_notifier,
						 result);
			finish_vdo_completion(completion, result);
			return;
		}

		if (entry.mapping.pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		slab = get_vdo_slab(vdo->depot, entry.mapping.pbn);
		if (slab->allocator != recovery->allocator) {
			continue;
		}

		if (!attempt_replay_into_vdo_slab_journal(slab->journal,
							 entry.mapping.pbn,
							 entry.operation,
							 &recovery->next_journal_point,
							 completion)) {
			return;
		}

		recovery->entries_added_to_slab_journals++;
	}

	uds_log_info("Recreating missing journal entries for zone %u",
		     recovery->allocator->zone_number);
	add_synthesized_entries(completion);
}

/**********************************************************************/
void vdo_replay_into_slab_journals(struct block_allocator *allocator,
				   struct vdo_completion *completion,
				   void *context)
{
	struct recovery_completion *recovery = context;

	assert_on_physical_zone_thread(recovery->vdo, allocator->zone_number,
				       __func__);
	if ((recovery->journal_data == NULL) || is_replaying(recovery->vdo)) {
		// there's nothing to replay
		notify_vdo_slab_journals_are_recovered(allocator, VDO_SUCCESS);
		return;
	}

	recovery->allocator = allocator;
	recovery->next_recovery_point = (struct recovery_point) {
		.sequence_number = recovery->slab_journal_head,
		.sector_count = 1,
		.entry_count = 0,
	};

	recovery->next_journal_point = (struct journal_point) {
		.sequence_number = recovery->slab_journal_head,
		.entry_count = 0,
	};

	uds_log_info("Replaying entries into slab journals for zone %u",
		     allocator->zone_number);
	completion->parent = &recovery->completion;
	add_slab_journal_entries(completion);
}

/**
 * A waiter callback to enqueue a missing_decref on the queue for the physical
 * zone in which it will be applied.
 *
 * Implements waiter_callback.
 **/
static void queue_on_physical_zone(struct waiter *waiter, void *context)
{
	zone_count_t zone_number;
	struct missing_decref *decref = as_missing_decref(waiter);
	struct data_location mapping = decref->penultimate_mapping;

	if (vdo_is_mapped_location(&mapping)) {
		decref->recovery->logical_blocks_used--;
	}

	if (mapping.pbn == VDO_ZERO_BLOCK) {
		// Decrefs of zero are not applied to slab journals.
		UDS_FREE(decref);
		return;
	}

	decref->slab_journal =
		get_vdo_slab_journal((struct slab_depot *) context, mapping.pbn);
	zone_number = decref->slab_journal->slab->allocator->zone_number;
	enqueue_missing_decref(&decref->recovery->missing_decrefs[zone_number],
			     decref);
}

/**
 * Queue each missing decref on the slab journal to which it is to be applied
 * then load the slab depot. This callback is registered in
 * find_slab_journal_entries().
 *
 * @param completion  The sub-task completion
 **/
static void apply_to_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct slab_depot *depot = get_slab_depot(recovery->vdo);

	assert_on_admin_thread(recovery->vdo, __func__);
	prepare_sub_task(recovery,
			 finish_recovering_depot,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);

	notify_all_waiters(&recovery->missing_decrefs[0],
			   queue_on_physical_zone, depot);
	if (abort_recovery_on_error(recovery->completion.result, recovery)) {
		return;
	}

	load_vdo_slab_depot(depot, VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
			    completion, recovery);
}

/**
 * Validate the location of the penultimate mapping for a missing_decref. If it
 * is valid, enqueue it for the appropriate physical zone or account for it.
 * Otherwise, dispose of it and signal an error.
 *
 * @param decref      The decref whose penultimate mapping has just been found
 * @param location    The penultimate mapping
 * @param error_code  The error code to use if the location is invalid
 **/
static int record_missing_decref(struct missing_decref *decref,
				 struct data_location location,
				 int error_code)
{
	struct recovery_completion *recovery = decref->recovery;

	recovery->incomplete_decref_count--;
	if (vdo_is_valid_location(&location) &&
	    vdo_is_physical_data_block(recovery->vdo->depot, location.pbn)) {
		decref->penultimate_mapping = location;
		decref->complete = true;
		return VDO_SUCCESS;
	}

	// The location was invalid
	vdo_enter_read_only_mode(recovery->vdo->read_only_notifier, error_code);
	set_vdo_completion_result(&recovery->completion, error_code);
	uds_log_error_strerror(error_code,
			       "Invalid mapping for pbn %llu with state %u",
			       (unsigned long long) location.pbn,
			       location.state);
	return error_code;
}

/**
 * Find the block map slots with missing decrefs.
 *
 * To find the slots missing decrefs, we iterate through the journal in reverse
 * so we see decrefs before increfs; if we see an incref before its paired
 * decref, we instantly know this incref is missing its decref.
 *
 * Simultaneously, we attempt to determine the missing decref. If there is a
 * missing decref, and at least two increfs for that slot, we know we should
 * decref the PBN from the penultimate incref. Otherwise, there is only one
 * incref for that slot: we must synthesize the decref out of the block map
 * instead of the recovery journal.
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
find_missing_decrefs(struct recovery_completion *recovery)
{
	// This placeholder decref is used to mark lbns for which we have
	// observed a decref but not the paired incref (going backwards through
	// the journal).
	struct missing_decref found_decref;

	int result;
	struct recovery_journal_entry entry;
	struct missing_decref *decref;
	struct recovery_point recovery_point;

	struct int_map *slot_entry_map = recovery->slot_entry_map;
	// A buffer is allocated based on the number of incref entries found, so
	// use the earliest head.
	sequence_number_t head = min(recovery->block_map_head,
				     recovery->slab_journal_head);
	struct recovery_point head_point = {
		.sequence_number = head,
		.sector_count = 1,
		.entry_count = 0,
	};

	// Set up for the first fake journal point that will be used for a
	// synthesized entry.
	recovery->next_synthesized_journal_point = (struct journal_point) {
		.sequence_number = recovery->tail,
		.entry_count =
			recovery->vdo->recovery_journal->entries_per_block,
	};

	recovery_point = recovery->tail_recovery_point;
	while (before_recovery_point(&head_point, &recovery_point)) {
		decrement_recovery_point(&recovery_point);
		entry = get_entry(recovery, &recovery_point);

		if (!is_vdo_journal_increment_operation(entry.operation)) {
			// Observe that we've seen a decref before its incref,
			// but only if the int_map does not contain an unpaired
			// incref for this lbn.
			result = int_map_put(slot_entry_map,
					     slot_as_number(entry.slot),
					     &found_decref,
					     false,
					     NULL);
			if (result != VDO_SUCCESS) {
				return result;
			}

			continue;
		}

		recovery->incref_count++;

		decref = int_map_remove(slot_entry_map,
					slot_as_number(entry.slot));
		if (entry.operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) {
			if (decref != NULL) {
				return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
							      "decref found for block map block %llu with state %u",
							      (unsigned long long) entry.mapping.pbn,
							      entry.mapping.state);
			}

			// There are no decrefs for block map pages, so they
			// can't be missing.
			continue;
		}

		if (decref == &found_decref) {
			// This incref already had a decref in the intmap, so
			// we know it is not missing its decref.
			continue;
		}

		if (decref == NULL) {
			// This incref is missing a decref. Add a missing
			// decref object.
			result = make_missing_decref(recovery, entry, &decref);
			if (result != VDO_SUCCESS) {
				return result;
			}

			result = int_map_put(slot_entry_map,
					     slot_as_number(entry.slot),
					     decref,
					     false,
					     NULL);
			if (result != VDO_SUCCESS) {
				return result;
			}

			continue;
		}

		/*
		 * This missing decref was left here by an incref without a
		 * decref. We now know what its penultimate mapping is, and all
		 * entries before here in the journal are paired, decref before
		 * incref, so we needn't remember it in the intmap any longer.
		 */
		result = record_missing_decref(decref, entry.mapping,
					       VDO_CORRUPT_JOURNAL);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**
 * Process a fetched block map page for a missing decref. This callback is
 * registered in find_slab_journal_entries().
 *
 * @param completion  The page completion which has just finished loading
 **/
static void process_fetched_page(struct vdo_completion *completion)
{
	struct missing_decref *current_decref = completion->parent;
	struct recovery_completion *recovery = current_decref->recovery;
	const struct block_map_page *page;
	struct data_location location;

	assert_on_logical_zone_thread(recovery->vdo, 0, __func__);

	page = dereference_readable_vdo_page(completion);
	location =
		unpack_vdo_block_map_entry(&page->entries[current_decref->slot.slot]);
	release_vdo_page_completion(completion);
	record_missing_decref(current_decref, location, VDO_BAD_MAPPING);
	if (recovery->incomplete_decref_count == 0) {
		complete_vdo_completion(&recovery->sub_task_completion);
	}
}

/**
 * Handle an error fetching a block map page for a missing decref.
 * This error handler is registered in find_slab_journal_entries().
 *
 * @param completion  The page completion which has just finished loading
 **/
static void handle_fetch_error(struct vdo_completion *completion)
{
	struct missing_decref *decref = completion->parent;
	struct recovery_completion *recovery = decref->recovery;

	assert_on_logical_zone_thread(recovery->vdo, 0, __func__);

	// If we got a VDO_OUT_OF_RANGE error, it is because the pbn we read
	// from the journal was bad, so convert the error code
	set_vdo_completion_result(&recovery->sub_task_completion,
				  ((completion->result == VDO_OUT_OF_RANGE) ?
				       VDO_CORRUPT_JOURNAL :
				       completion->result));
	release_vdo_page_completion(completion);
	if (--recovery->incomplete_decref_count == 0) {
		complete_vdo_completion(&recovery->sub_task_completion);
	}
}

/**
 * The waiter callback to requeue a missing decref and launch its page fetch.
 *
 * Implements waiter_callback.
 **/
static void launch_fetch(struct waiter *waiter, void *context)
{
	struct missing_decref *decref = as_missing_decref(waiter);
	struct recovery_completion *recovery = decref->recovery;
	struct block_map_zone *zone = context;

	if (enqueue_missing_decref(&recovery->missing_decrefs[0], decref) !=
	    VDO_SUCCESS) {
		return;
	}

	if (decref->complete) {
		// We've already found the mapping for this decref, no fetch
		// needed.
		return;
	}

	init_vdo_page_completion(&decref->page_completion,
				 zone->page_cache,
				 decref->slot.pbn,
				 false,
				 decref,
				 process_fetched_page,
				 handle_fetch_error);
	get_vdo_page(&decref->page_completion.completion);
}

/**
 * Find all entries which need to be replayed into the slab journals.
 *
 * @param completion  The sub-task completion
 **/
static void find_slab_journal_entries(struct vdo_completion *completion)
{
	int result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	// We need to be on logical zone 0's thread since we are going to use
	// its page cache.
	assert_on_logical_zone_thread(vdo, 0, __func__);
	result = find_missing_decrefs(recovery);
	if (abort_recovery_on_error(result, recovery)) {
		return;
	}

	prepare_sub_task(recovery,
			 apply_to_depot,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);

	/*
	 * Increment the incomplete_decref_count so that the fetch callback
	 * can't complete the sub-task while we are still processing the queue
	 * of missing decrefs.
	 */
	if (recovery->incomplete_decref_count++ > 0) {
		// Fetch block map pages to fill in the incomplete missing
		// decrefs.
		notify_all_waiters(&recovery->missing_decrefs[0],
				   launch_fetch,
				   vdo_get_block_map_zone(get_block_map(vdo), 0));
	}

	if (--recovery->incomplete_decref_count == 0) {
		complete_vdo_completion(completion);
	}
}

/**
 * Find the contiguous range of journal blocks.
 *
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if there were valid journal blocks
 **/
static bool find_contiguous_range(struct recovery_completion *recovery)
{
	struct recovery_journal *journal = recovery->vdo->recovery_journal;
	sequence_number_t head = min(recovery->block_map_head,
				     recovery->slab_journal_head);

	bool found_entries = false;
	sequence_number_t i;

	for (i = head; i <= recovery->highest_tail; i++) {
		struct packed_journal_header *packed_header;
		struct recovery_block_header header;
		journal_entry_count_t block_entries;
		uint8_t j;

		recovery->tail = i;
		recovery->tail_recovery_point = (struct recovery_point) {
			.sequence_number = i,
			.sector_count = 0,
			.entry_count = 0,
		};

		packed_header =
			get_vdo_recovery_journal_block_header(journal,
							      recovery->journal_data,
							      i);
		unpack_vdo_recovery_block_header(packed_header, &header);

		if (!is_exact_vdo_recovery_journal_block(journal, &header, i) ||
		    (header.entry_count > journal->entries_per_block)) {
			// A bad block header was found so this must be the end
			// of the journal.
			break;
		}

		block_entries = header.entry_count;
		// Examine each sector in turn to determine the last valid
		// sector.
		for (j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
			struct packed_journal_sector *sector =
				get_vdo_journal_block_sector(packed_header, j);
			journal_entry_count_t sector_entries =
				min((journal_entry_count_t) sector->entry_count,
				    block_entries);

			// A bad sector means that this block was torn.
			if (!is_valid_vdo_recovery_journal_sector(&header,
								  sector)) {
				break;
			}

			if (sector_entries > 0) {
				found_entries = true;
				recovery->tail_recovery_point.sector_count++;
				recovery->tail_recovery_point.entry_count =
					sector_entries;
				block_entries -= sector_entries;
			}

			// If this sector is short, the later sectors can't
			// matter.
			if ((sector_entries <
			     RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) ||
			    (block_entries == 0)) {
				break;
			}
		}

		// If this block was not filled, or if it tore, no later block
		// can matter.
		if ((header.entry_count != journal->entries_per_block) ||
		    (block_entries > 0)) {
			break;
		}
	}

	// Set the tail to the last valid tail block, if there is one.
	if (found_entries &&
	    (recovery->tail_recovery_point.sector_count == 0)) {
		recovery->tail--;
	}

	return found_entries;
}

/**
 * Count the number of increment entries in the journal.
 *
 * @param recovery  The recovery completion
 **/
static int count_increment_entries(struct recovery_completion *recovery)
{
	struct recovery_point recovery_point = {
		.sequence_number = recovery->block_map_head,
		.sector_count = 1,
		.entry_count = 0,
	};
	while (before_recovery_point(&recovery_point,
				     &recovery->tail_recovery_point)) {
		struct recovery_journal_entry entry =
			get_entry(recovery, &recovery_point);
		int result =
			validate_vdo_recovery_journal_entry(recovery->vdo,
							    &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(recovery->vdo->read_only_notifier,
						 result);
			return result;
		}
		if (is_vdo_journal_increment_operation(entry.operation)) {
			recovery->incref_count++;
		}
		increment_recovery_point(&recovery_point);
	}

	return VDO_SUCCESS;
}

/**
 * Determine the limits of the valid recovery journal and prepare to replay
 * into the slab journals and block map.
 *
 * @param completion  The sub-task completion
 **/
static void prepare_to_apply_journal_entries(struct vdo_completion *completion)
{
	bool found_entries;
	int result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;

	uds_log_info("Finished reading recovery journal");
	found_entries =
		find_vdo_recovery_journal_head_and_tail(journal,
							recovery->journal_data,
							&recovery->highest_tail,
							&recovery->block_map_head,
							&recovery->slab_journal_head);
	if (found_entries) {
		found_entries = find_contiguous_range(recovery);
	}

	// Both reap heads must be behind the tail.
	if ((recovery->block_map_head > recovery->tail) ||
	    (recovery->slab_journal_head > recovery->tail)) {
		result = uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						"Journal tail too early. block map head: %llu, slab journal head: %llu, tail: %llu",
						(unsigned long long) recovery->block_map_head,
						(unsigned long long) recovery->slab_journal_head,
						(unsigned long long) recovery->tail);
		finish_vdo_completion(&recovery->completion, result);
		return;
	}

	if (!found_entries) {
		// This message must be recognizable by VDOTest::RebuildBase.
		uds_log_info("Replaying 0 recovery entries into block map");
		// We still need to load the slab_depot.
		UDS_FREE(recovery->journal_data);
		recovery->journal_data = NULL;
		prepare_sub_task(recovery,
				 finish_vdo_completion_parent_callback,
				 finish_vdo_completion_parent_callback,
				 VDO_ZONE_TYPE_ADMIN);
		load_vdo_slab_depot(get_slab_depot(vdo),
				    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
				    completion,
				    recovery);
		return;
	}

	uds_log_info("Highest-numbered recovery journal block has sequence number %llu, and the highest-numbered usable block is %llu",
		     (unsigned long long) recovery->highest_tail,
		     (unsigned long long) recovery->tail);

	if (is_replaying(vdo)) {
		// We need to know how many entries the block map rebuild
		// completion will need to hold.
		result = count_increment_entries(recovery);
		if (result != VDO_SUCCESS) {
			finish_vdo_completion(&recovery->completion, result);
			return;
		}

		// We need to access the block map from a logical zone.
		prepare_sub_task(recovery,
				 launch_block_map_recovery,
				 finish_vdo_completion_parent_callback,
				 VDO_ZONE_TYPE_LOGICAL);
		load_vdo_slab_depot(vdo->depot,
				    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
				    completion,
				    recovery);
		return;
	}

	result = compute_usages(recovery);
	if (abort_recovery_on_error(result, recovery)) {
		return;
	}

	prepare_sub_task(recovery,
			 find_slab_journal_entries,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_LOGICAL);
	invoke_vdo_completion_callback(completion);
}

/**********************************************************************/
void vdo_launch_recovery(struct vdo *vdo, struct vdo_completion *parent)
{
	struct recovery_completion *recovery;
	int result;

	// Note: This message must be recognizable by Permabit::VDODeviceBase.
	uds_log_warning("Device was dirty, rebuilding reference counts");

	result = make_vdo_recovery_completion(vdo, &recovery);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	prepare_vdo_completion(&recovery->completion,
			       finish_recovery,
			       abort_recovery,
			       parent->callback_thread_id,
			       parent);
	prepare_sub_task(recovery,
			 prepare_to_apply_journal_entries,
			 finish_vdo_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);
	load_vdo_recovery_journal(vdo->recovery_journal,
				  &recovery->sub_task_completion,
				  &recovery->journal_data);
}
