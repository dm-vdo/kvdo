// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "read-only-rebuild.h"

#include "logger.h"
#include "memory-alloc.h"

#include "block-map.h"
#include "block-map-recovery.h"
#include "completion.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal.h"
#include "recovery-utils.h"
#include "reference-count-rebuild.h"
#include "slab-depot.h"
#include "thread-config.h"
#include "vdo.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vdo-page-cache.h"

struct read_only_rebuild_completion {
	/** The completion header */
	struct vdo_completion completion;
	/** A sub task completion */
	struct vdo_completion sub_task_completion;
	/** The vdo in question */
	struct vdo *vdo;
	/** A buffer to hold the data read off disk */
	char *journal_data;
	/** The entry data for the block map rebuild */
	struct numbered_block_mapping *entries;
	/** The number of entries in the entry array */
	size_t entry_count;
	/**
	 * The sequence number of the first valid block of the journal (if
	 * known)
	 */
	sequence_number_t head;
	/**
	 * The sequence number of the last valid block of the journal (if
	 * known)
	 */
	sequence_number_t tail;
	/** The number of logical blocks in use */
	block_count_t logical_blocks_used;
	/** The number of allocated block map pages */
	block_count_t block_map_data_blocks;
};

/**
 * as_read_only_rebuild_completion() - Convert a generic completion to
 *                                     a read_only_rebuild_completion.
 * @completion: The completion to convert.
 *
 * Return: The journal rebuild completion.
 */
static inline struct read_only_rebuild_completion * __must_check
as_read_only_rebuild_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_READ_ONLY_REBUILD_COMPLETION);
	return container_of(completion, struct read_only_rebuild_completion,
			    completion);
}

/**
 * free_rebuild_completion() - Free a rebuild completion and all underlying
 *                             structures.
 * @rebuild: The rebuild completion to free.
 */
static void
free_rebuild_completion(struct read_only_rebuild_completion *rebuild)
{
	if (rebuild == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(rebuild->journal_data));
	UDS_FREE(UDS_FORGET(rebuild->entries));
	UDS_FREE(rebuild);
}

/**
 * make_rebuild_completion() - Allocate and initialize a read only rebuild
 *                             completion.
 * @vdo: The vdo in question.
 * @rebuild_ptr: A pointer to return the created rebuild completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int
make_rebuild_completion(struct vdo *vdo,
			struct read_only_rebuild_completion **rebuild_ptr)
{
	struct read_only_rebuild_completion *rebuild;
	int result = UDS_ALLOCATE(1, struct read_only_rebuild_completion,
				  __func__, &rebuild);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_initialize_completion(&rebuild->completion, vdo,
				  VDO_READ_ONLY_REBUILD_COMPLETION);
	vdo_initialize_completion(&rebuild->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);

	rebuild->vdo = vdo;
	*rebuild_ptr = rebuild;
	return VDO_SUCCESS;
}

/**
 * complete_rebuild() - Clean up the rebuild process.
 * @completion: The rebuild completion.
 *
 * Cleans up the rebuild process, whether or not it succeeded, by freeing the
 * rebuild completion and notifying the parent of the outcome.
 */
static void complete_rebuild(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(UDS_FORGET(completion));
	struct block_map *block_map = rebuild->vdo->block_map;

	vdo_set_page_cache_rebuild_mode(block_map->zones[0].page_cache, false);
	free_rebuild_completion(UDS_FORGET(rebuild));
	vdo_finish_completion(parent, result);
}

/**
 * finish_rebuild() - Finish rebuilding, free the rebuild completion and
 *                    notify the parent.
 * @completion: The rebuild completion.
 */
static void finish_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion);
	struct vdo *vdo = rebuild->vdo;

	vdo_initialize_recovery_journal_post_rebuild(vdo->recovery_journal,
						     vdo->states.vdo.complete_recoveries,
						     rebuild->tail,
						     rebuild->logical_blocks_used,
						     rebuild->block_map_data_blocks);
	uds_log_info("Read-only rebuild complete");
	complete_rebuild(completion);
}

/**
 * abort_rebuild() - Handle a rebuild error.
 * @completion: The rebuild completion.
 */
static void abort_rebuild(struct vdo_completion *completion)
{
	uds_log_info("Read-only rebuild aborted");
	complete_rebuild(completion);
}

/**
 * abort_rebuild_on_error() - Abort a rebuild if there is an error.
 * @result: The result to check.
 * @rebuild: The journal rebuild completion.
 *
 * Return: true if the result was an error.
 */
static bool __must_check
abort_rebuild_on_error(int result,
		       struct read_only_rebuild_completion *rebuild)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	vdo_finish_completion(&rebuild->completion, result);
	return true;
}

/**
 * finish_reference_count_rebuild() - Clean up after finishing the reference
 *                                    count rebuild.
 * @completion: The sub-task completion.
 *
 * This callback is registered in launch_reference_count_rebuild().
 */
static void finish_reference_count_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild = completion->parent;
	struct vdo *vdo = rebuild->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);
	if (vdo->load_state != VDO_REBUILD_FOR_UPGRADE) {
		/* A "rebuild" for upgrade should not increment this count. */
		vdo->states.vdo.complete_recoveries++;
	}

	uds_log_info("Saving rebuilt state");
	vdo_prepare_completion_to_finish_parent(completion, &rebuild->completion);
	vdo_drain_slab_depot(vdo->depot, VDO_ADMIN_STATE_REBUILDING, completion);
}

/**
 * launch_reference_count_rebuild() - Rebuild the reference counts from the
 *                                    block map now that all journal entries
 *                                    have been applied to the block map.
 * @completion: The sub-task completion.
 *
 * This callback is registered in apply_journal_entries().
 */
static void launch_reference_count_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild = completion->parent;
	struct vdo *vdo = rebuild->vdo;

	/* We must allocate ref_counts before we can rebuild them. */
	int result = vdo_allocate_slab_ref_counts(vdo->depot);

	if (abort_rebuild_on_error(result, rebuild)) {
		return;
	}

	vdo_prepare_completion(completion,
			       finish_reference_count_rebuild,
			       vdo_finish_completion_parent_callback,
			       vdo->thread_config->admin_thread,
			       completion->parent);
	vdo_rebuild_reference_counts(vdo,
				     completion,
				     &rebuild->logical_blocks_used,
				     &rebuild->block_map_data_blocks);
}

/**
 * append_sector_entries() - Append an array of recovery journal entries from
 *                           a journal block sector to the array of numbered
 *                           mappings in the rebuild completion, numbering
 *                           each entry in the order they are appended.
 * @rebuild: The journal rebuild completion.
 * @sector: The recovery journal sector with entries.
 * @entry_count: The number of entries to append.
 */
static void append_sector_entries(struct read_only_rebuild_completion *rebuild,
				  struct packed_journal_sector *sector,
				  journal_entry_count_t entry_count)
{
	journal_entry_count_t i;

	for (i = 0; i < entry_count; i++) {
		struct recovery_journal_entry entry =
			vdo_unpack_recovery_journal_entry(&sector->entries[i]);
		int result = vdo_validate_recovery_journal_entry(rebuild->vdo,
								 &entry);
		if (result != VDO_SUCCESS) {
			/*
			 * When recovering from read-only mode, ignore damaged
			 * entries.
			 */
			continue;
		}

		if (vdo_is_journal_increment_operation(entry.operation)) {
			rebuild->entries[rebuild->entry_count] =
				(struct numbered_block_mapping) {
					.block_map_slot = entry.slot,
					.block_map_entry =
						vdo_pack_pbn(entry.mapping.pbn,
							     entry.mapping.state),
					.number = rebuild->entry_count,
				};
			rebuild->entry_count++;
		}
	}
}

/**
 * extract_journal_entries() - Create an array of all valid journal entries,
 *                             in order, and store it in the rebuild
 *                             completion.
 * @rebuild: The journal rebuild completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int extract_journal_entries(struct read_only_rebuild_completion *rebuild)
{
	sequence_number_t i;

	struct vdo *vdo = rebuild->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	sequence_number_t first = rebuild->head;
	sequence_number_t last = rebuild->tail;
	block_count_t max_count = ((last - first + 1) *
				   journal->entries_per_block);

	/*
	 * Allocate an array of numbered_block_mapping structures large
	 * enough to transcribe every packed_recovery_journal_entry from every
	 * valid journal block.
	 */
	int result = UDS_ALLOCATE(max_count,
				  struct numbered_block_mapping,
				  __func__,
				  &rebuild->entries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (i = first; i <= last; i++) {
		struct packed_journal_header *packed_header =
			vdo_get_recovery_journal_block_header(journal,
							      rebuild->journal_data,
							      i);
		struct recovery_block_header header;
		journal_entry_count_t block_entries;
		uint8_t j;

		vdo_unpack_recovery_block_header(packed_header, &header);

		if (!vdo_is_exact_recovery_journal_block(journal, &header, i)) {
			/* This block is invalid, so skip it. */
			continue;
		}

		/*
		 * Don't extract more than the expected maximum entries per
		 * block.
		 */
		block_entries = min(journal->entries_per_block,
				    header.entry_count);
		for (j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
			journal_entry_count_t sector_entries;

			struct packed_journal_sector *sector =
				vdo_get_journal_block_sector(packed_header, j);
			/*
			 * Stop when all entries counted in the header are
			 * applied or skipped.
			 */
			if (block_entries == 0) {
				break;
			}

			if (!vdo_is_valid_recovery_journal_sector(&header, sector)) {
				block_entries -=
					min(block_entries,
					    (journal_entry_count_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
				continue;
			}

			/*
			 * Don't extract more than the expected maximum entries
			 * per sector.
			 */
			sector_entries =
				min(sector->entry_count,
				    (uint8_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
			/* Only extract as many as the block header calls for. */
			sector_entries = min(sector_entries, block_entries);
			append_sector_entries(rebuild, sector, sector_entries);
			/*
			 * Even if the sector wasn't full, count it as full
			 * when counting up to the entry count the block
			 * header claims.
			 */
			block_entries -=
				min(block_entries,
				    (journal_entry_count_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
		}
	}

	return VDO_SUCCESS;
}

/**
 * apply_journal_entries() - Determine the limits of the valid recovery
 *                           journal and apply all valid entries to the block
 *                           map.
 * @completion: The sub-task completion.
 *
 * This callback is registered in load_journal_callback().
 */
static void apply_journal_entries(struct vdo_completion *completion)
{
	bool found_entries;

	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion->parent);
	struct vdo *vdo = rebuild->vdo;

	uds_log_info("Finished reading recovery journal");
	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	found_entries =
		vdo_find_recovery_journal_head_and_tail(vdo->recovery_journal,
							rebuild->journal_data,
							&rebuild->tail,
							&rebuild->head,
							NULL);
	if (found_entries) {
		int result = extract_journal_entries(rebuild);

		if (abort_rebuild_on_error(result, rebuild)) {
			return;
		}
	}

	/* Suppress block map errors. */
	vdo_set_page_cache_rebuild_mode(vdo->block_map->zones[0].page_cache,
					true);

	/* Play the recovery journal into the block map. */
	vdo_prepare_completion(completion,
			       launch_reference_count_rebuild,
			       vdo_finish_completion_parent_callback,
			       completion->callback_thread_id,
			       completion->parent);
	vdo_recover_block_map(vdo, rebuild->entry_count, rebuild->entries,
			      completion);
}

/**
 * load_journal_callback() - Begin loading the journal.
 * @completion: The sub task completion.
 */
static void load_journal_callback(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion->parent);
	struct vdo *vdo = rebuild->vdo;

	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	vdo_prepare_completion(completion,
			       apply_journal_entries,
			       vdo_finish_completion_parent_callback,
			       completion->callback_thread_id,
			       completion->parent);
	vdo_load_recovery_journal(vdo->recovery_journal, completion,
				  &rebuild->journal_data);
}

/**
 * vdo_launch_rebuild() - Construct a read_only_rebuild_completion and launch
 *                        it.
 * @vdo: The vdo to rebuild.
 * @parent: The completion to notify when the rebuild is complete.
 *
 * Apply all valid journal block entries to all vdo structures.
 *
 * Context: Must be launched from logical zone 0.
 */
void vdo_launch_rebuild(struct vdo *vdo, struct vdo_completion *parent)
{
	struct read_only_rebuild_completion *rebuild;
	struct vdo_completion *completion, *sub_task_completion;
	int result;

	/* Note: These messages must be recognizable by Permabit::VDODeviceBase. */
	if (vdo->load_state == VDO_REBUILD_FOR_UPGRADE) {
		uds_log_warning("Rebuilding reference counts for upgrade");
	} else {
		uds_log_warning("Rebuilding reference counts to clear read-only mode");
		vdo->states.vdo.read_only_recoveries++;
	}

	result = make_rebuild_completion(vdo, &rebuild);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	completion = &rebuild->completion;
	vdo_prepare_completion(completion,
			       finish_rebuild,
			       abort_rebuild,
			       parent->callback_thread_id,
			       parent);

	sub_task_completion = &rebuild->sub_task_completion;
	vdo_prepare_completion(sub_task_completion,
			       load_journal_callback,
			       vdo_finish_completion_parent_callback,
			       vdo_get_logical_zone_thread(vdo->thread_config, 0),
			       completion);
	vdo_load_slab_depot(vdo->depot,
			    VDO_ADMIN_STATE_LOADING_FOR_REBUILD,
			    sub_task_completion,
			    NULL);
}
