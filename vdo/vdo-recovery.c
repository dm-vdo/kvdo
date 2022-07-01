// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-recovery.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-allocator.h"
#include "block-map.h"
#include "block-map-page.h"
#include "block-map-recovery.h"
#include "completion.h"
#include "int-map.h"
#include "journal-point.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal.h"
#include "recovery-utils.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "wait-queue.h"

/*
 * The absolute position of an entry in the recovery journal, including
 * the sector number and the entry number within the sector.
 */
struct recovery_point {
	sequence_number_t sequence_number; /* Block sequence number */
	uint8_t sector_count; /* Sector number */
	journal_entry_count_t entry_count; /* Entry number */
};

struct recovery_completion {
	/* The completion header */
	struct vdo_completion completion;
	/* The sub-task completion */
	struct vdo_completion sub_task_completion;
	/* The struct vdo in question */
	struct vdo *vdo;
	/* The struct block_allocator whose journals are being recovered */
	struct block_allocator *allocator;
	/* A buffer to hold the data read off disk */
	char *journal_data;
	/* The number of increfs */
	size_t incref_count;

	/* The entry data for the block map recovery */
	struct numbered_block_mapping *entries;
	/* The number of entries in the entry array */
	size_t entry_count;
	/*
	 * The sequence number of the first valid block for block map recovery
	 */
	sequence_number_t block_map_head;
	/*
	 * The sequence number of the first valid block for slab journal replay
	 */
	sequence_number_t slab_journal_head;
	/*
	 * The sequence number of the last valid block of the journal (if
	 * known)
	 */
	sequence_number_t tail;
	/*
	 * The highest sequence number of the journal, not the same as the tail,
	 * since the tail ignores blocks after the first hole.
	 */
	sequence_number_t highest_tail;

	/* A location just beyond the last valid entry of the journal */
	struct recovery_point tail_recovery_point;
	/* The location of the next recovery journal entry to apply */
	struct recovery_point next_recovery_point;
	/* The number of logical blocks currently known to be in use */
	block_count_t logical_blocks_used;
	/* The number of block map data blocks known to be allocated */
	block_count_t block_map_data_blocks;
	/* The journal point to give to the next synthesized decref */
	struct journal_point next_journal_point;
	/* The number of entries played into slab journals */
	size_t entries_added_to_slab_journals;

	/* Decref synthesis fields */

	/* An int_map for use in finding which slots are missing decrefs */
	struct int_map *slot_entry_map;
	/* The number of synthesized decrefs */
	size_t missing_decref_count;
	/* The number of incomplete decrefs */
	size_t incomplete_decref_count;
	/* The fake journal point of the next missing decref */
	struct journal_point next_synthesized_journal_point;
	/* The queue of missing decrefs */
	struct wait_queue missing_decrefs[];
};

enum {
	/* The int map needs capacity of twice the number of VIOs in the system. */
	INT_MAP_CAPACITY = MAXIMUM_VDO_USER_VIOS * 2,
	/* There can be as many missing decrefs as there are VIOs in the system. */
	MAXIMUM_SYNTHESIZED_DECREFS = MAXIMUM_VDO_USER_VIOS,
};

struct missing_decref {
	/* A waiter for queueing this object */
	struct waiter waiter;
	/* The parent of this object */
	struct recovery_completion *recovery;
	/* Whether this decref is complete */
	bool complete;
	/* The slot for which the last decref was lost */
	struct block_map_slot slot;
	/* The penultimate block map entry for this LBN */
	struct data_location penultimate_mapping;
	/* The page completion used to fetch the block map page for this LBN */
	struct vdo_page_completion page_completion;
	/* The journal point which will be used for this entry */
	struct journal_point journal_point;
	/* The slab journal to which this entry will be applied */
	struct slab_journal *slab_journal;
};

/**
 * as_missing_decref() - Convert a waiter to the missing decref of which it is
 *                       a part.
 * @waiter: The waiter to convert.
 *
 * Return: The missing_decref wrapping the waiter.
 */
static inline struct missing_decref * __must_check
as_missing_decref(struct waiter *waiter)
{
	return container_of(waiter, struct missing_decref, waiter);
}

/**
 * enqueue_missing_decref() - Enqueue a missing_decref.
 * @queue: The queue on which to enqueue the decref.
 * @decref: The missing_decref to enqueue.
 *
 * If the enqueue fails, enter read-only mode.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int enqueue_missing_decref(struct wait_queue *queue,
				  struct missing_decref *decref)
{
	int result = enqueue_waiter(queue, &decref->waiter);

	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(decref->recovery->vdo->read_only_notifier,
					 result);
		vdo_set_completion_result(&decref->recovery->completion, result);
		UDS_FREE(decref);
	}

	return result;
}

/**
 * as_vdo_recovery_completion() - Convert a generic completion to a
 *                                recovery_completion.
 * @completion: The completion to convert.
 *
 * Return: The recovery_completion.
 */
static inline struct recovery_completion * __must_check
as_vdo_recovery_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_RECOVERY_COMPLETION);
	return container_of(completion, struct recovery_completion, completion);
}

/**
 * slot_as_number() - Convert a block_map_slot into a unique uint64_t.
 * @slot: The block map slot to convert..
 *
 * Return: A one-to-one mappable uint64_t.
 */
static uint64_t slot_as_number(struct block_map_slot slot)
{
	return (((uint64_t) slot.pbn << 10) + slot.slot);
}

/**
 * is_replaying() - Check whether a vdo was replaying the recovery journal
 *                  into the block map when it crashed.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo crashed while reconstructing the block map.
 */
static bool __must_check is_replaying(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_REPLAYING);
}

/**
 * make_missing_decref() - Create a missing_decref and enqueue it to wait for
 *                         a determination of its penultimate mapping.
 * @recovery: The parent recovery completion.
 * @entry: The recovery journal entry for the increment which is missing a
 *         decref.
 * @decref_ptr: A pointer to hold the new missing_decref.
 *
 * Return: VDO_SUCCESS or an error code.
 */
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
 * increment_recovery_point() - Move the given recovery point forward by one
 *                              entry.
 * @point: The recovery point to alter.
 */
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
 * decrement_recovery_point() - Move the given recovery point backwards by one
 *                              entry.
 * @point: The recovery point to alter.
 */
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
 * before_recovery_point() - Check whether the first point precedes the second
 *                           point.
 * @first: The first recovery point.
 * @second: The second recovery point.
 *
 * Return: true if the first point precedes the second point.
 */
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
 * prepare_sub_task() - Prepare the sub-task completion.
 * @recovery: The recovery_completion whose sub-task completion is to be
 *            prepared.
 * @callback: The callback to register for the next sub-task.
 * @error_handler: The error handler for the next sub-task.
 * @zone_type: The type of zone on which the callback or error_handler should
 *             run.
 */
static void prepare_sub_task(struct recovery_completion *recovery,
			     vdo_action callback,
			     vdo_action error_handler,
			     enum vdo_zone_type zone_type)
{
	const struct thread_config *thread_config =
		recovery->vdo->thread_config;
	thread_id_t thread_id;

	switch (zone_type) {
	case VDO_ZONE_TYPE_LOGICAL:
		/*
		 * All blockmap access is done on single thread, so use logical
		 * zone 0.
		 */
		thread_id = vdo_get_logical_zone_thread(thread_config, 0);
		break;

	case VDO_ZONE_TYPE_PHYSICAL:
		thread_id = recovery->allocator->thread_id;
		break;

	case VDO_ZONE_TYPE_ADMIN:
	default:
		thread_id = thread_config->admin_thread;
	}

	vdo_prepare_completion(&recovery->sub_task_completion,
			       callback,
			       error_handler,
			       thread_id,
			       &recovery->completion);
}

/**
 * free_missing_decref() - A waiter callback to free missing_decrefs.
 *
 * Implements waiter_callback.
 */
static void free_missing_decref(struct waiter *waiter,
				void *context __always_unused)
{
	UDS_FREE(as_missing_decref(waiter));
}

/**
 * free_vdo_recovery_completion() - Free a recovery_completion and all
 * underlying structures.
 * @recovery: The recovery completion to free.
 */
static void free_vdo_recovery_completion(struct recovery_completion *recovery)
{
	zone_count_t zone, zone_count;

	if (recovery == NULL) {
		return;
	}

	free_int_map(UDS_FORGET(recovery->slot_entry_map));
	zone_count = recovery->vdo->thread_config->physical_zone_count;
	for (zone = 0; zone < zone_count; zone++) {
		notify_all_waiters(&recovery->missing_decrefs[zone],
				   free_missing_decref, NULL);
	}

	UDS_FREE(UDS_FORGET(recovery->journal_data));
	UDS_FREE(UDS_FORGET(recovery->entries));
	UDS_FREE(recovery);
}

/**
 * vdo_make_recovery_completion() - Allocate and initialize a
 *                                  recovery_completion.
 * @vdo: The vdo in question.
 * @recovery_ptr: A pointer to hold the new recovery_completion.
 *
 * Return: VDO_SUCCESS or a status code.
 */
static int __must_check
vdo_make_recovery_completion(struct vdo *vdo,
			     struct recovery_completion **recovery_ptr)
{
	struct recovery_completion *recovery;
	zone_count_t zone;
	zone_count_t zone_count = vdo->thread_config->physical_zone_count;
	int result = UDS_ALLOCATE_EXTENDED(struct recovery_completion,
					   zone_count,
					   struct list_head,
					   __func__,
					   &recovery);
	if (result != VDO_SUCCESS) {
		return result;
	}

	recovery->vdo = vdo;
	for (zone = 0; zone < zone_count; zone++) {
		initialize_wait_queue(&recovery->missing_decrefs[zone]);
	}

	vdo_initialize_completion(&recovery->completion, vdo,
				  VDO_RECOVERY_COMPLETION);
	vdo_initialize_completion(&recovery->sub_task_completion, vdo,
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
 * finish_recovery() - Finish recovering, free the recovery completion and
 *                     notify the parent.
 * @completion: The recovery completion.
 */
static void finish_recovery(struct vdo_completion *completion)
{
	int result;
	struct vdo_completion *parent = completion->parent;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion);
	struct vdo *vdo = recovery->vdo;
	uint64_t recovery_count = ++vdo->states.vdo.complete_recoveries;

	vdo_initialize_recovery_journal_post_recovery(vdo->recovery_journal,
						      recovery_count,
						      recovery->highest_tail);
	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_info("Rebuild complete");

	/*
	 * Now that we've freed the recovery completion and its vast array of
	 * journal entries, we can allocate refcounts.
	 */
	result = vdo_allocate_slab_ref_counts(vdo->depot);
	vdo_finish_completion(parent, result);
}

/**
 * abort_recovery() - Handle a recovery error.
 * @completion: The recovery completion.
 */
static void abort_recovery(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion);
	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_warning("Recovery aborted");
	vdo_finish_completion(parent, result);
}

/**
 * abort_recovery_on_error() - Abort a recovery if there is an error.
 * @result: The result to check.
 * @recovery: The recovery completion.
 *
 * Return: true if the result was an error.
 */
static bool __must_check
abort_recovery_on_error(int result, struct recovery_completion *recovery)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	vdo_finish_completion(&recovery->completion, result);
	return true;
}

/**
 * get_entry() - Unpack the recovery journal entry associated with the given
 *               recovery point.
 * @recovery: The recovery completion.
 * @point: The recovery point.
 *
 * Return: The unpacked contents of the matching recovery journal entry.
 */
static struct recovery_journal_entry
get_entry(const struct recovery_completion *recovery,
	  const struct recovery_point *point)
{
	struct recovery_journal *journal = recovery->vdo->recovery_journal;
	physical_block_number_t block_number =
		vdo_get_recovery_journal_block_number(journal,
						      point->sequence_number);
	off_t sector_offset = (block_number * VDO_BLOCK_SIZE) +
		(point->sector_count * VDO_SECTOR_SIZE);
	struct packed_journal_sector *sector =
		(struct packed_journal_sector *) &recovery->journal_data[sector_offset];
	return vdo_unpack_recovery_journal_entry(&sector->entries[point->entry_count]);
}

/**
 * extract_journal_entries() - Create an array of all valid journal entries,
 *                             in order, and store it in the recovery
 *                             completion.
 * @recovery: The recovery completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
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
		result = vdo_validate_recovery_journal_entry(recovery->vdo,
							     &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(recovery->vdo->read_only_notifier,
						 result);
			return result;
		}

		if (vdo_is_journal_increment_operation(entry.operation)) {
			recovery->entries[recovery->entry_count] =
				(struct numbered_block_mapping) {
					.block_map_slot = entry.slot,
					.block_map_entry =
						vdo_pack_pbn(entry.mapping.pbn,
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
 * launch_block_map_recovery() - Extract journal entries and recover the block
 *                               map.
 * @completion: The sub-task completion.
 *
 * This callback is registered in start_super_block_save().
 */
static void launch_block_map_recovery(struct vdo_completion *completion)
{
	int result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	/* Extract the journal entries for the block map recovery. */
	result = extract_journal_entries(recovery);
	if (abort_recovery_on_error(result, recovery)) {
		return;
	}

	vdo_prepare_completion_to_finish_parent(completion, &recovery->completion);
	vdo_recover_block_map(vdo, recovery->entry_count, recovery->entries,
			      completion);
}

/**
 * start_super_block_save() - Finish flushing all slab journals and start a
 *                            write of the super block.
 * @completion: The sub-task completion.
 *
 * This callback is registered in add_synthesized_entries().
 */
static void start_super_block_save(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

	uds_log_info("Saving recovery progress");
	vdo_set_state(vdo, VDO_REPLAYING);

	/*
	 * The block map access which follows the super block save must be done
	 * on a logical thread.
	 */
	prepare_sub_task(recovery,
			 launch_block_map_recovery,
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_LOGICAL);
	vdo_save_components(vdo, completion);
}

/**
 * finish_recovering_depot() - The callback from loading the slab depot.
 * @completion: The sub-task completion.
 *
 * Updates the logical blocks and block map data blocks counts in the recovery
 * journal and then drains the slab depot in order to commit the recovered
 * slab journals. It is registered in apply_to_depot().
 */
static void finish_recovering_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

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
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);
	vdo_drain_slab_depot(vdo->depot, VDO_ADMIN_STATE_RECOVERING, completion);
}

/**
 * handle_add_slab_journal_entry_error() - The error handler for recovering
 *                                         slab journals.
 * @completion: The completion of the block allocator being recovered.
 *
 * Skips any remaining recovery on the current zone and propagates the error.
 * It is registered in add_slab_journal_entries() and
 * add_synthesized_entries().
 */
static void
handle_add_slab_journal_entry_error(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	vdo_notify_slab_journals_are_recovered(recovery->allocator,
					       completion->result);
}

/**
 * add_synthesized_entries() - Add synthesized entries into slab journals,
 *                             waiting when necessary.
 * @completion: The allocator completion.
 */
static void add_synthesized_entries(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct wait_queue *missing_decrefs =
		&recovery->missing_decrefs[recovery->allocator->zone_number];

	/* Get ready in case we need to enqueue again */
	vdo_prepare_completion(completion,
			       add_synthesized_entries,
			       handle_add_slab_journal_entry_error,
			       completion->callback_thread_id,
			       &recovery->completion);
	while (has_waiters(missing_decrefs)) {
		struct missing_decref *decref =
			as_missing_decref(get_first_waiter(missing_decrefs));
		if (!vdo_attempt_replay_into_slab_journal(decref->slab_journal,
							  decref->penultimate_mapping.pbn,
							  VDO_JOURNAL_DATA_DECREMENT,
							  &decref->journal_point,
							  completion)) {
			return;
		}

		dequeue_next_waiter(missing_decrefs);
		UDS_FREE(decref);
	}

	vdo_notify_slab_journals_are_recovered(recovery->allocator,
					       VDO_SUCCESS);
}

/**
 * compute_usages() - Determine the LBNs used count as of the end of the
 *                    journal.
 * @recovery: The recovery completion.
 *
 * Does not include any changes to that count from entries that will be
 * synthesized later).
 *
 * Return: VDO_SUCCESS or an error.
 */
__attribute__((__noinline__))
static int compute_usages(struct recovery_completion *recovery)
{
	/*
	 * XXX VDO-5182: function is declared noinline to avoid what is likely
	 * a spurious valgrind error about this structure being uninitialized.
	 */
	struct recovery_point recovery_point = {
		.sequence_number = recovery->tail,
		.sector_count = 1,
		.entry_count = 0,
	};
	struct recovery_journal *journal = recovery->vdo->recovery_journal;
	struct packed_journal_header *tail_header =
		vdo_get_recovery_journal_block_header(journal,
						      recovery->journal_data,
						      recovery->tail);

	struct recovery_block_header unpacked;

	vdo_unpack_recovery_block_header(tail_header, &unpacked);
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
 * advance_points() - Advance the current recovery and journal points.
 * @recovery: The recovery_completion whose points are to be advanced.
 * @entries_per_block: The number of entries in a recovery journal block.
 */
static void advance_points(struct recovery_completion *recovery,
			   journal_entry_count_t entries_per_block)
{
	increment_recovery_point(&recovery->next_recovery_point);
	vdo_advance_journal_point(&recovery->next_journal_point,
				  entries_per_block);
}

/**
 * add_slab_journal_entries() - Replay recovery journal entries into the slab
 *                              journals of the allocator currently being
 *                              recovered.
 * @completion: The allocator completion.
 *
 * Waits for slab journal tailblock space when necessary. This method is its
 * own callback.
 */
static void add_slab_journal_entries(struct vdo_completion *completion)
{
	struct recovery_point *recovery_point;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;

	/* Get ready in case we need to enqueue again. */
	vdo_prepare_completion(completion,
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
		int result = vdo_validate_recovery_journal_entry(vdo, &entry);

		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(journal->read_only_notifier,
						 result);
			vdo_finish_completion(completion, result);
			return;
		}

		if (entry.mapping.pbn == VDO_ZERO_BLOCK) {
			continue;
		}

		slab = vdo_get_slab(vdo->depot, entry.mapping.pbn);
		if (slab->allocator != recovery->allocator) {
			continue;
		}

		if (!vdo_attempt_replay_into_slab_journal(slab->journal,
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

/**
 * vdo_replay_into_slab_journals() - Replay recovery journal entries in the
 *                                   slab journals of slabs owned by a given
 *                                   block_allocator.
 * @allocator: The allocator whose slab journals are to be recovered.
 * @completion: The completion to use for waiting on slab journal space.
 * @context: The slab depot load context supplied by a recovery when it loads
 *           the depot.
 */
void vdo_replay_into_slab_journals(struct block_allocator *allocator,
				   struct vdo_completion *completion,
				   void *context)
{
	struct recovery_completion *recovery = context;

	vdo_assert_on_physical_zone_thread(recovery->vdo,
					   allocator->zone_number,
					   __func__);
	if ((recovery->journal_data == NULL) || is_replaying(recovery->vdo)) {
		/* there's nothing to replay */
		vdo_notify_slab_journals_are_recovered(allocator, VDO_SUCCESS);
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
 * queue_on_physical_zone() - A waiter callback to enqueue a missing_decref on
 *                            the queue for the physical zone in which it will
 *                            be applied.
 *
 * Implements waiter_callback.
 */
static void queue_on_physical_zone(struct waiter *waiter, void *context)
{
	zone_count_t zone_number;
	struct missing_decref *decref = as_missing_decref(waiter);
	struct data_location mapping = decref->penultimate_mapping;

	if (vdo_is_mapped_location(&mapping)) {
		decref->recovery->logical_blocks_used--;
	}

	if (mapping.pbn == VDO_ZERO_BLOCK) {
		/* Decrefs of zero are not applied to slab journals. */
		UDS_FREE(decref);
		return;
	}

	decref->slab_journal =
		vdo_get_slab_journal((struct slab_depot *) context, mapping.pbn);
	zone_number = decref->slab_journal->slab->allocator->zone_number;
	enqueue_missing_decref(&decref->recovery->missing_decrefs[zone_number],
			     decref);
}

/**
 * apply_to_depot() - Queue each missing decref on the slab journal to which
 *                    it is to be applied then load the slab depot.
 * @completion: The sub-task completion.
 *
 * This callback is registered in find_slab_journal_entries().
 */
static void apply_to_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct slab_depot *depot = recovery->vdo->depot;

	vdo_assert_on_admin_thread(recovery->vdo, __func__);
	prepare_sub_task(recovery,
			 finish_recovering_depot,
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);

	notify_all_waiters(&recovery->missing_decrefs[0],
			   queue_on_physical_zone, depot);
	if (abort_recovery_on_error(recovery->completion.result, recovery)) {
		return;
	}

	vdo_load_slab_depot(depot, VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
			    completion, recovery);
}

/**
 * record_missing_decref() - Validate the location of the penultimate mapping
 *                           for a missing_decref.
 * @decref: The decref whose penultimate mapping has just been found.
 * @location: The penultimate mapping.
 * @error_code: The error code to use if the location is invalid.
 *
 * If it is valid, enqueue it for the appropriate physical zone or account for
 * it. Otherwise, dispose of it and signal an error.
 */
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

	/* The location was invalid */
	vdo_enter_read_only_mode(recovery->vdo->read_only_notifier, error_code);
	vdo_set_completion_result(&recovery->completion, error_code);
	uds_log_error_strerror(error_code,
			       "Invalid mapping for pbn %llu with state %u",
			       (unsigned long long) location.pbn,
			       location.state);
	return error_code;
}

/**
 * find_missing_decrefs() - Find the block map slots with missing decrefs.
 * @recovery: The recovery completion.
 *
 * To find the slots missing decrefs, we iterate through the journal in
 * reverse so we see decrefs before increfs; if we see an incref before its
 * paired decref, we instantly know this incref is missing its decref.
 *
 * Simultaneously, we attempt to determine the missing decref. If there is a
 * missing decref, and at least two increfs for that slot, we know we should
 * decref the PBN from the penultimate incref. Otherwise, there is only one
 * incref for that slot: we must synthesize the decref out of the block map
 * instead of the recovery journal.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check
find_missing_decrefs(struct recovery_completion *recovery)
{
	/*
	 * This placeholder decref is used to mark lbns for which we have
	 * observed a decref but not the paired incref (going backwards through
	 * the journal).
	 */
	struct missing_decref found_decref;

	int result;
	struct recovery_journal_entry entry;
	struct missing_decref *decref;
	struct recovery_point recovery_point;

	struct int_map *slot_entry_map = recovery->slot_entry_map;
	/*
	 * A buffer is allocated based on the number of incref entries found, so
	 * use the earliest head.
	 */
	sequence_number_t head = min(recovery->block_map_head,
				     recovery->slab_journal_head);
	struct recovery_point head_point = {
		.sequence_number = head,
		.sector_count = 1,
		.entry_count = 0,
	};

	/*
	 * Set up for the first fake journal point that will be used for a
	 * synthesized entry.
	 */
	recovery->next_synthesized_journal_point = (struct journal_point) {
		.sequence_number = recovery->tail,
		.entry_count =
			recovery->vdo->recovery_journal->entries_per_block,
	};

	recovery_point = recovery->tail_recovery_point;
	while (before_recovery_point(&head_point, &recovery_point)) {
		decrement_recovery_point(&recovery_point);
		entry = get_entry(recovery, &recovery_point);

		if (!vdo_is_journal_increment_operation(entry.operation)) {
			/*
			 * Observe that we've seen a decref before its incref,
			 * but only if the int_map does not contain an unpaired
			 * incref for this lbn.
			 */
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

			/*
			 * There are no decrefs for block map pages, so they
			 * can't be missing.
			 */
			continue;
		}

		if (decref == &found_decref) {
			/*
			 * This incref already had a decref in the intmap, so
			 * we know it is not missing its decref.
			 */
			continue;
		}

		if (decref == NULL) {
			/*
			 * This incref is missing a decref. Add a missing
			 * decref object.
			 */
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
 * process_fetched_page() - Process a fetched block map page for a missing
 *                          decref.
 * @completion: The page completion which has just finished loading.
 *
 * This callback is registered in find_slab_journal_entries().
 */
static void process_fetched_page(struct vdo_completion *completion)
{
	struct missing_decref *current_decref = completion->parent;
	struct recovery_completion *recovery = current_decref->recovery;
	const struct block_map_page *page;
	struct data_location location;

	vdo_assert_on_logical_zone_thread(recovery->vdo, 0, __func__);

	page = vdo_dereference_readable_page(completion);
	location =
		vdo_unpack_block_map_entry(&page->entries[current_decref->slot.slot]);
	vdo_release_page_completion(completion);
	record_missing_decref(current_decref, location, VDO_BAD_MAPPING);
	if (recovery->incomplete_decref_count == 0) {
		vdo_complete_completion(&recovery->sub_task_completion);
	}
}

/**
 * handle_fetch_error() - Handle an error fetching a block map page for a
 *                        missing decref.
 * @completion: The page completion which has just finished loading.
 *
 * This error handler is registered in find_slab_journal_entries().
 */
static void handle_fetch_error(struct vdo_completion *completion)
{
	struct missing_decref *decref = completion->parent;
	struct recovery_completion *recovery = decref->recovery;

	vdo_assert_on_logical_zone_thread(recovery->vdo, 0, __func__);

	/*
	 * If we got a VDO_OUT_OF_RANGE error, it is because the pbn we read
	 * from the journal was bad, so convert the error code
	 */
	vdo_set_completion_result(&recovery->sub_task_completion,
				  ((completion->result == VDO_OUT_OF_RANGE) ?
				       VDO_CORRUPT_JOURNAL :
				       completion->result));
	vdo_release_page_completion(completion);
	if (--recovery->incomplete_decref_count == 0) {
		vdo_complete_completion(&recovery->sub_task_completion);
	}
}

/**
 * launch_fetch() - The waiter callback to requeue a missing decref and launch
 *                  its page fetch.
 *
 * Implements waiter_callback.
 */
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
		/*
		 * We've already found the mapping for this decref, no fetch
		 * needed.
		 */
		return;
	}

	vdo_init_page_completion(&decref->page_completion,
				 zone->page_cache,
				 decref->slot.pbn,
				 false,
				 decref,
				 process_fetched_page,
				 handle_fetch_error);
	vdo_get_page(&decref->page_completion.completion);
}

/**
 * find_slab_journal_entries() - Find all entries which need to be replayed
 *                               into the slab journals.
 *
 * @completion: The sub-task completion.
 */
static void find_slab_journal_entries(struct vdo_completion *completion)
{
	int result;
	struct recovery_completion *recovery =
		as_vdo_recovery_completion(completion->parent);
	struct vdo *vdo = recovery->vdo;

	/*
	 * We need to be on logical zone 0's thread since we are going to use
	 * its page cache.
	 */
	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);
	result = find_missing_decrefs(recovery);
	if (abort_recovery_on_error(result, recovery)) {
		return;
	}

	prepare_sub_task(recovery,
			 apply_to_depot,
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);

	/*
	 * Increment the incomplete_decref_count so that the fetch callback
	 * can't complete the sub-task while we are still processing the queue
	 * of missing decrefs.
	 */
	if (recovery->incomplete_decref_count++ > 0) {
		/*
		 * Fetch block map pages to fill in the incomplete missing
		 * decrefs.
		 */
		notify_all_waiters(&recovery->missing_decrefs[0],
				   launch_fetch,
				   &vdo->block_map->zones[0]);
	}

	if (--recovery->incomplete_decref_count == 0) {
		vdo_complete_completion(completion);
	}
}

/**
 * find_contiguous_range() - Find the contiguous range of journal blocks.
 * @recovery: The recovery completion.
 *
 * Return: true if there were valid journal blocks.
 */
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
			vdo_get_recovery_journal_block_header(journal,
							      recovery->journal_data,
							      i);
		vdo_unpack_recovery_block_header(packed_header, &header);

		if (!vdo_is_exact_recovery_journal_block(journal, &header, i) ||
		    (header.entry_count > journal->entries_per_block)) {
			/*
			 * A bad block header was found so this must be the end
			 * of the journal.
			 */
			break;
		}

		block_entries = header.entry_count;
		/*
		 * Examine each sector in turn to determine the last valid
		 * sector.
		 */
		for (j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
			struct packed_journal_sector *sector =
				vdo_get_journal_block_sector(packed_header, j);
			journal_entry_count_t sector_entries =
				min((journal_entry_count_t) sector->entry_count,
				    block_entries);

			/* A bad sector means that this block was torn. */
			if (!vdo_is_valid_recovery_journal_sector(&header,
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

			/*
			 * If this sector is short, the later sectors can't
			 * matter.
			 */
			if ((sector_entries <
			     RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) ||
			    (block_entries == 0)) {
				break;
			}
		}

		/*
		 * If this block was not filled, or if it tore, no later block
		 * can matter.
		 */
		if ((header.entry_count != journal->entries_per_block) ||
		    (block_entries > 0)) {
			break;
		}
	}

	/* Set the tail to the last valid tail block, if there is one. */
	if (found_entries &&
	    (recovery->tail_recovery_point.sector_count == 0)) {
		recovery->tail--;
	}

	return found_entries;
}

/**
 * count_increment_entries() - Count the number of increment entries in the
 *                             journal.
 * @recovery: The recovery completion.
 */
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
			vdo_validate_recovery_journal_entry(recovery->vdo,
							    &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(recovery->vdo->read_only_notifier,
						 result);
			return result;
		}
		if (vdo_is_journal_increment_operation(entry.operation)) {
			recovery->incref_count++;
		}
		increment_recovery_point(&recovery_point);
	}

	return VDO_SUCCESS;
}

/**
 * prepare_to_apply_journal_entries() - Determine the limits of the valid
 *                                      recovery journal and prepare to replay
 *                                      into the slab journals and block map.
 * @completion: The sub-task completion.
 */
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
		vdo_find_recovery_journal_head_and_tail(journal,
							recovery->journal_data,
							&recovery->highest_tail,
							&recovery->block_map_head,
							&recovery->slab_journal_head);
	if (found_entries) {
		found_entries = find_contiguous_range(recovery);
	}

	/* Both reap heads must be behind the tail. */
	if ((recovery->block_map_head > recovery->tail) ||
	    (recovery->slab_journal_head > recovery->tail)) {
		result = uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						"Journal tail too early. block map head: %llu, slab journal head: %llu, tail: %llu",
						(unsigned long long) recovery->block_map_head,
						(unsigned long long) recovery->slab_journal_head,
						(unsigned long long) recovery->tail);
		vdo_finish_completion(&recovery->completion, result);
		return;
	}

	if (!found_entries) {
		/* This message must be recognizable by VDOTest::RebuildBase. */
		uds_log_info("Replaying 0 recovery entries into block map");
		/* We still need to load the slab_depot. */
		UDS_FREE(recovery->journal_data);
		recovery->journal_data = NULL;
		prepare_sub_task(recovery,
				 vdo_finish_completion_parent_callback,
				 vdo_finish_completion_parent_callback,
				 VDO_ZONE_TYPE_ADMIN);
		vdo_load_slab_depot(vdo->depot,
				    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
				    completion,
				    recovery);
		return;
	}

	uds_log_info("Highest-numbered recovery journal block has sequence number %llu, and the highest-numbered usable block is %llu",
		     (unsigned long long) recovery->highest_tail,
		     (unsigned long long) recovery->tail);

	if (is_replaying(vdo)) {
		/*
		 * We need to know how many entries the block map rebuild
		 * completion will need to hold.
		 */
		result = count_increment_entries(recovery);
		if (result != VDO_SUCCESS) {
			vdo_finish_completion(&recovery->completion, result);
			return;
		}

		/* We need to access the block map from a logical zone. */
		prepare_sub_task(recovery,
				 launch_block_map_recovery,
				 vdo_finish_completion_parent_callback,
				 VDO_ZONE_TYPE_LOGICAL);
		vdo_load_slab_depot(vdo->depot,
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
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_LOGICAL);
	vdo_invoke_completion_callback(completion);
}

/**
 * vdo_launch_recovery() - Construct a recovery completion and launch it.
 * @vdo: The vdo to recover.
 * @parent: The completion to notify when the offline portion of the recovery
 *          is complete.
 * 
 * Applies all valid journal block entries to all vdo structures. This
 * function performs the offline portion of recovering a vdo from a crash.
 */

void vdo_launch_recovery(struct vdo *vdo, struct vdo_completion *parent)
{
	struct recovery_completion *recovery;
	int result;

	/* Note: This message must be recognizable by Permabit::VDODeviceBase. */
	uds_log_warning("Device was dirty, rebuilding reference counts");

	result = vdo_make_recovery_completion(vdo, &recovery);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	vdo_prepare_completion(&recovery->completion,
			       finish_recovery,
			       abort_recovery,
			       parent->callback_thread_id,
			       parent);
	prepare_sub_task(recovery,
			 prepare_to_apply_journal_entries,
			 vdo_finish_completion_parent_callback,
			 VDO_ZONE_TYPE_ADMIN);
	vdo_load_recovery_journal(vdo->recovery_journal,
				  &recovery->sub_task_completion,
				  &recovery->journal_data);
}
