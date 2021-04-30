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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryUtils.c#33 $
 */

#include "recoveryUtils.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "completion.h"
#include "extent.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
#include "slabDepot.h"
#include "vdoComponent.h"
#include "vdoComponentStates.h"
#include "vdoInternal.h"

/**
 * Finish loading the journal by freeing the extent and notifying the parent.
 * This callback is registered in load_journal().
 *
 * @param completion  The load extent
 **/
static void finish_journal_load(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;
	struct vdo_extent *extent = vdo_completion_as_extent(completion);
	free_vdo_extent(&extent);
	finish_vdo_completion(parent, result);
}

/**********************************************************************/
void load_journal(struct recovery_journal *journal,
		  struct vdo_completion *parent,
		  char **journal_data_ptr)
{
	struct vdo_extent *extent;
	int result = ALLOCATE(journal->size * VDO_BLOCK_SIZE, char, __func__,
			      journal_data_ptr);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	result = create_vdo_extent(parent->vdo, VIO_TYPE_RECOVERY_JOURNAL,
				   VIO_PRIORITY_METADATA, journal->size,
				   *journal_data_ptr, &extent);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	prepare_vdo_completion(&extent->completion, finish_journal_load,
			       finish_journal_load, parent->callback_thread_id,
			       parent);
	read_vdo_metadata_extent(extent,
				 get_fixed_layout_partition_offset(journal->partition));
}

/**
 * Determine whether the given header describe a valid block for the
 * given journal that could appear at the given offset in the journal.
 *
 * @param journal  The journal to use
 * @param header   The unpacked block header to check
 * @param offset   An offset indicating where the block was in the journal
 *
 * @return <code>True</code> if the header matches
 **/
static bool __must_check
is_congruent_recovery_journal_block(struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    physical_block_number_t offset)
{
	physical_block_number_t expected_offset =
		get_recovery_journal_block_number(journal,
						  header->sequence_number);
	return ((expected_offset == offset)
		&& is_valid_recovery_journal_block(journal, header));
}

/**********************************************************************/
bool find_head_and_tail(struct recovery_journal *journal,
			char *journal_data,
			sequence_number_t *tail_ptr,
			sequence_number_t *block_map_head_ptr,
			sequence_number_t *slab_journal_head_ptr)
{
	sequence_number_t highest_tail = journal->tail;
	sequence_number_t block_map_head_max = 0;
	sequence_number_t slab_journal_head_max = 0;
	bool found_entries = false;
	physical_block_number_t i;
	for (i = 0; i < journal->size; i++) {
		struct packed_journal_header *packed_header =
			get_journal_block_header(journal, journal_data, i);
		struct recovery_block_header header;
		unpack_recovery_block_header(packed_header, &header);

		if (!is_congruent_recovery_journal_block(journal, &header, i)) {
			// This block is old, unformatted, or doesn't belong at
			// this location.
			continue;
		}

		if (header.sequence_number >= highest_tail) {
			found_entries = true;
			highest_tail = header.sequence_number;
		}
		if (header.block_map_head > block_map_head_max) {
			block_map_head_max = header.block_map_head;
		}
		if (header.slab_journal_head > slab_journal_head_max) {
			slab_journal_head_max = header.slab_journal_head;
		}
	}

	*tail_ptr = highest_tail;
	if (!found_entries) {
		return false;
	}

	*block_map_head_ptr = block_map_head_max;
	if (slab_journal_head_ptr != NULL) {
		*slab_journal_head_ptr = slab_journal_head_max;
	}
	return true;
}

/**********************************************************************/
int validate_recovery_journal_entry(const struct vdo *vdo,
				    const struct recovery_journal_entry *entry)
{
	if ((entry->slot.pbn >= vdo->states.vdo.config.physical_blocks) ||
 	    (entry->slot.slot >= BLOCK_MAP_ENTRIES_PER_PAGE) ||
 	    !is_valid_location(&entry->mapping) ||
 	    !is_physical_data_block(vdo->depot, entry->mapping.pbn)) {
		return log_error_strerror(VDO_CORRUPT_JOURNAL,
					  "Invalid entry: (%llu, %u) to %llu (%s) is not within bounds",
					  entry->slot.pbn,
					  entry->slot.slot,
					  entry->mapping.pbn,
					  get_journal_operation_name(entry->operation));
	}

	if ((entry->operation == BLOCK_MAP_INCREMENT) &&
	    (is_compressed(entry->mapping.state) ||
	    (entry->mapping.pbn == ZERO_BLOCK))) {
		return log_error_strerror(VDO_CORRUPT_JOURNAL,
					  "Invalid entry: (%llu, %u) to %llu (%s) is not a valid tree mapping",
					  entry->slot.pbn,
					  entry->slot.slot,
					  entry->mapping.pbn,
					  get_journal_operation_name(entry->operation));
	}

	return VDO_SUCCESS;
}
