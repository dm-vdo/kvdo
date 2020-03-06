/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryUtils.c#13 $
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
#include "vdoInternal.h"

/**
 * Finish loading the journal by freeing the extent and notifying the parent.
 * This callback is registered in load_journal_async().
 *
 * @param completion  The load extent
 **/
static void finish_journal_load(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;
	struct vdo_extent *extent = as_vdo_extent(completion);
	free_extent(&extent);
	finishCompletion(parent, result);
}

/**********************************************************************/
void load_journal_async(struct recovery_journal *journal,
			struct vdo_completion *parent,
			char **journal_data_ptr)
{
	int result = ALLOCATE(journal->size * VDO_BLOCK_SIZE, char, __func__,
			      journal_data_ptr);
	if (result != VDO_SUCCESS) {
		finishCompletion(parent, result);
		return;
	}

	struct vdo_extent *extent;
	result = create_extent(parent->layer, VIO_TYPE_RECOVERY_JOURNAL,
			       VIO_PRIORITY_METADATA, journal->size,
			       *journal_data_ptr, &extent);
	if (result != VDO_SUCCESS) {
		finishCompletion(parent, result);
		return;
	}

	prepareCompletion(&extent->completion, finish_journal_load,
			  finish_journal_load, parent->callbackThreadID,
			  parent);
	read_metadata_extent(extent,
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
__attribute__((warn_unused_result)) static bool
is_congruent_recovery_journal_block(struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    PhysicalBlockNumber offset)
{
	PhysicalBlockNumber expected_offset =
		get_recovery_journal_block_number(journal,
						  header->sequenceNumber);
	return ((expected_offset == offset)
		&& is_valid_recovery_journal_block(journal, header));
}

/**********************************************************************/
bool find_head_and_tail(struct recovery_journal *journal,
			char *journal_data,
			SequenceNumber *tail_ptr,
			SequenceNumber *block_map_head_ptr,
			SequenceNumber *slab_journal_head_ptr)
{
	SequenceNumber highest_tail = journal->tail;
	SequenceNumber block_map_head_max = 0;
	SequenceNumber slab_journal_head_max = 0;
	bool found_entries = false;
	PhysicalBlockNumber i;
	for (i = 0; i < journal->size; i++) {
		PackedJournalHeader *packed_header =
			get_journal_block_header(journal, journal_data, i);
		struct recovery_block_header header;
		unpackRecoveryBlockHeader(packed_header, &header);

		if (!is_congruent_recovery_journal_block(journal, &header, i)) {
			// This block is old, unformatted, or doesn't belong at
			// this location.
			continue;
		}

		if (header.sequenceNumber >= highest_tail) {
			found_entries = true;
			highest_tail = header.sequenceNumber;
		}
		if (header.blockMapHead > block_map_head_max) {
			block_map_head_max = header.blockMapHead;
		}
		if (header.slabJournalHead > slab_journal_head_max) {
			slab_journal_head_max = header.slabJournalHead;
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
	if ((entry->slot.pbn >= vdo->config.physicalBlocks)
	    || (entry->slot.slot >= BLOCK_MAP_ENTRIES_PER_PAGE)
	    || !is_valid_location(&entry->mapping)
	    || !is_physical_data_block(vdo->depot, entry->mapping.pbn)) {
		return logErrorWithStringError(VDO_CORRUPT_JOURNAL,
					       "Invalid entry:"
					       " (%llu, %" PRIu16 ") to %" PRIu64
					       " (%s) is not within bounds",
					       entry->slot.pbn, entry->slot.slot, entry->mapping.pbn,
					       get_journal_operation_name(entry->operation));
	}

	if ((entry->operation == BLOCK_MAP_INCREMENT)
	    && (isCompressed(entry->mapping.state)
		|| (entry->mapping.pbn == ZERO_BLOCK))) {
		return logErrorWithStringError(VDO_CORRUPT_JOURNAL,
					       "Invalid entry:"
					       " (%llu, %" PRIu16 ") to %" PRIu64
					       " (%s) is not a valid tree mapping",
					       entry->slot.pbn,
					       entry->slot.slot,
					       entry->mapping.pbn,
					       get_journal_operation_name(entry->operation));
	}

	return VDO_SUCCESS;
}
