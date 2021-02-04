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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyRebuild.c#45 $
 */

#include "readOnlyRebuild.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMapInternals.h"
#include "blockMapRecovery.h"
#include "completion.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalInternals.h"
#include "recoveryUtils.h"
#include "referenceCountRebuild.h"
#include "slabDepot.h"
#include "vdoComponent.h"
#include "vdoComponentStates.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

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
 * Convert a generic completion to a read_only_rebuild_completion.
 *
 * @param completion    The completion to convert
 *
 * @return the journal rebuild completion
 **/
static inline struct read_only_rebuild_completion * __must_check
as_read_only_rebuild_completion(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, READ_ONLY_REBUILD_COMPLETION);
	return container_of(completion, struct read_only_rebuild_completion,
			    completion);
}

/**
 * Free a rebuild completion and all underlying structures.
 *
 * @param rebuild_ptr  A pointer to the rebuild completion to free
 */
static void
free_rebuild_completion(struct read_only_rebuild_completion **rebuild_ptr)
{
	struct read_only_rebuild_completion *rebuild = *rebuild_ptr;
	if (rebuild == NULL) {
		return;
	}

	FREE(rebuild->journal_data);
	FREE(rebuild->entries);
	FREE(rebuild);
	*rebuild_ptr = NULL;
}

/**
 * Allocate and initialize a read only rebuild completion.
 *
 * @param [in]  vdo          The vdo in question
 * @param [out] rebuild_ptr  A pointer to return the created rebuild completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int
make_rebuild_completion(struct vdo *vdo,
			struct read_only_rebuild_completion **rebuild_ptr)
{
	struct read_only_rebuild_completion *rebuild;
	int result = ALLOCATE(
		1, struct read_only_rebuild_completion, __func__, &rebuild);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_completion(&rebuild->completion,
			      READ_ONLY_REBUILD_COMPLETION,
			      vdo->layer);
	initialize_completion(&rebuild->sub_task_completion,
			      SUB_TASK_COMPLETION, vdo->layer);

	rebuild->vdo = vdo;
	*rebuild_ptr = rebuild;
	return VDO_SUCCESS;
}

/**
 * Clean up the rebuild process, whether or not it succeeded, by freeing the
 * rebuild completion and notifying the parent of the outcome.
 *
 * @param completion  The rebuild completion
 **/
static void complete_rebuild(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion);
	struct vdo *vdo = rebuild->vdo;
	set_vdo_page_cache_rebuild_mode(get_block_map(vdo)->zones[0].page_cache,
					false);
	free_rebuild_completion(&rebuild);
	finish_completion(parent, result);
}

/**
 * Finish rebuilding, free the rebuild completion and notify the parent.
 *
 * @param completion  The rebuild completion
 **/
static void finish_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion);
	struct vdo *vdo = rebuild->vdo;
	initialize_recovery_journal_post_rebuild(vdo->recovery_journal,
						 vdo->states.vdo.complete_recoveries,
						 rebuild->tail,
						 rebuild->logical_blocks_used,
						 rebuild->block_map_data_blocks);
	log_info("Read-only rebuild complete");
	complete_rebuild(completion);
}

/**
 * Handle a rebuild error.
 *
 * @param completion  The rebuild completion
 **/
static void abort_rebuild(struct vdo_completion *completion)
{
	log_info("Read-only rebuild aborted");
	complete_rebuild(completion);
}

/**
 * Abort a rebuild if there is an error.
 *
 * @param result   The result to check
 * @param rebuild  The journal rebuild completion
 *
 * @return <code>true</code> if the result was an error
 **/
static bool __must_check
abort_rebuild_on_error(int result,
		       struct read_only_rebuild_completion *rebuild)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	finish_completion(&rebuild->completion, result);
	return true;
}

/**
 * Clean up after finishing the reference count rebuild. This callback is
 * registered in launch_reference_count_rebuild().
 *
 * @param completion  The sub-task completion
 **/
static void finish_reference_count_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild = completion->parent;
	struct vdo *vdo = rebuild->vdo;
	assert_on_admin_thread(vdo, __func__);
	if (vdo->load_state != VDO_REBUILD_FOR_UPGRADE) {
		// A "rebuild" for upgrade should not increment this count.
		vdo->states.vdo.complete_recoveries++;
	}

	log_info("Saving rebuilt state");
	prepare_to_finish_parent(completion, &rebuild->completion);
	drain_slab_depot(vdo->depot, ADMIN_STATE_REBUILDING, completion);
}

/**
 * Rebuild the reference counts from the block map now that all journal entries
 * have been applied to the block map. This callback is registered in
 * apply_journal_entries().
 *
 * @param completion  The sub-task completion
 **/
static void launch_reference_count_rebuild(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild = completion->parent;
	struct vdo *vdo = rebuild->vdo;

	// We must allocate ref_counts before we can rebuild them.
	int result = allocate_slab_ref_counts(vdo->depot);
	if (abort_rebuild_on_error(result, rebuild)) {
		return;
	}

	prepare_completion(completion,
			   finish_reference_count_rebuild,
			   finish_parent_callback,
			   get_admin_thread(get_thread_config(vdo)),
			   completion->parent);
	rebuild_reference_counts(vdo,
				 completion,
				 &rebuild->logical_blocks_used,
				 &rebuild->block_map_data_blocks);
}

/**
 * Append an array of recovery journal entries from a journal block sector to
 * the array of numbered mappings in the rebuild completion, numbering each
 * entry in the order they are appended.
 *
 * @param rebuild      The journal rebuild completion
 * @param sector       The recovery journal sector with entries
 * @param entry_count  The number of entries to append
 **/
static void append_sector_entries(struct read_only_rebuild_completion *rebuild,
				  struct packed_journal_sector *sector,
				  journal_entry_count_t entry_count)
{
	journal_entry_count_t i;
	for (i = 0; i < entry_count; i++) {
		struct recovery_journal_entry entry =
			unpack_recovery_journal_entry(&sector->entries[i]);
		int result = validate_recovery_journal_entry(rebuild->vdo,
							     &entry);
		if (result != VDO_SUCCESS) {
			// When recovering from read-only mode, ignore damaged
			// entries.
			continue;
		}

		if (is_increment_operation(entry.operation)) {
			rebuild->entries[rebuild->entry_count] =
				(struct numbered_block_mapping) {
					.block_map_slot = entry.slot,
					.block_map_entry =
						pack_pbn(entry.mapping.pbn,
							 entry.mapping.state),
					.number = rebuild->entry_count,
				};
			rebuild->entry_count++;
		}
	}
}

/**
 * Create an array of all valid journal entries, in order, and store
 * it in the rebuild completion.
 *
 * @param rebuild  The journal rebuild completion
 *
 * @return VDO_SUCCESS or an error code
 **/
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
	int result = ALLOCATE(max_count,
			      struct numbered_block_mapping,
			      __func__,
			      &rebuild->entries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (i = first; i <= last; i++) {
		struct packed_journal_header *packed_header =
			get_journal_block_header(journal,
						 rebuild->journal_data,
						 i);
		struct recovery_block_header header;
		journal_entry_count_t block_entries;
		uint8_t j;

		unpack_recovery_block_header(packed_header, &header);

		if (!is_exact_recovery_journal_block(journal, &header, i)) {
			// This block is invalid, so skip it.
			continue;
		}

		// Don't extract more than the expected maximum entries per
		// block.
		block_entries = min(journal->entries_per_block,
				    header.entry_count);
		for (j = 1; j < SECTORS_PER_BLOCK; j++) {
			journal_entry_count_t sector_entries;

			struct packed_journal_sector *sector =
				get_journal_block_sector(packed_header, j);
			// Stop when all entries counted in the header are
			// applied or skipped.
			if (block_entries == 0) {
				break;
			}

			if (!is_valid_recovery_journal_sector(&header, sector)) {
				block_entries -=
					min(block_entries,
					    (journal_entry_count_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
				continue;
			}

			// Don't extract more than the expected maximum entries
			// per sector.
			sector_entries =
				min(sector->entry_count,
				    (uint8_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
			// Only extract as many as the block header calls for.
			sector_entries = min(sector_entries, block_entries);
			append_sector_entries(rebuild, sector, sector_entries);
			// Even if the sector wasn't full, count it as full
			// when counting up to the entry count the block
			// header claims.
			block_entries -=
				min(block_entries,
				    (journal_entry_count_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
		}
	}

	return VDO_SUCCESS;
}

/**
 * Determine the limits of the valid recovery journal and apply all
 * valid entries to the block map. This callback is registered in
 * load_journal_callback().
 *
 * @param completion   The sub-task completion
 **/
static void apply_journal_entries(struct vdo_completion *completion)
{
	bool found_entries;

	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion->parent);
	struct vdo *vdo = rebuild->vdo;

	log_info("Finished reading recovery journal");
	assert_on_logical_zone_thread(vdo, 0, __func__);

	found_entries = find_head_and_tail(vdo->recovery_journal,
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

	// Suppress block map errors.
	set_vdo_page_cache_rebuild_mode(get_block_map(vdo)->zones[0].page_cache,
					true);

	// Play the recovery journal into the block map.
	prepare_completion(completion,
			   launch_reference_count_rebuild,
			   finish_parent_callback,
			   completion->callback_thread_id,
			   completion->parent);
	recover_block_map(vdo, rebuild->entry_count, rebuild->entries,
			  completion);
}

/**
 * Begin loading the journal.
 *
 * @param completion    The sub task completion
 **/
static void load_journal_callback(struct vdo_completion *completion)
{
	struct read_only_rebuild_completion *rebuild =
		as_read_only_rebuild_completion(completion->parent);
	struct vdo *vdo = rebuild->vdo;
	assert_on_logical_zone_thread(vdo, 0, __func__);

	prepare_completion(completion,
			   apply_journal_entries,
			   finish_parent_callback,
			   completion->callback_thread_id,
			   completion->parent);
	load_journal(vdo->recovery_journal, completion,
		     &rebuild->journal_data);
}

/**********************************************************************/
void launch_rebuild(struct vdo *vdo, struct vdo_completion *parent)
{
	struct read_only_rebuild_completion *rebuild;
	struct vdo_completion *completion, *sub_task_completion;
	int result;

	// Note: These messages must be recognizable by Permabit::VDODeviceBase.
	if (vdo->load_state == VDO_REBUILD_FOR_UPGRADE) {
		log_warning("Rebuilding reference counts for upgrade");
	} else {
		log_warning("Rebuilding reference counts to clear read-only mode");
		vdo->states.vdo.read_only_recoveries++;
	}

	result = make_rebuild_completion(vdo, &rebuild);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	completion = &rebuild->completion;
	prepare_completion(completion,
			   finish_rebuild,
			   abort_rebuild,
			   parent->callback_thread_id,
			   parent);

	sub_task_completion = &rebuild->sub_task_completion;
	prepare_completion(sub_task_completion,
			   load_journal_callback,
			   finish_parent_callback,
			   get_logical_zone_thread(get_thread_config(vdo), 0),
			   completion);
	load_slab_depot(vdo->depot,
			ADMIN_STATE_LOADING_FOR_REBUILD,
			sub_task_completion,
			NULL);
}
