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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoRecoveryInternals.h#20 $
 */

#ifndef VDO_RECOVERY_INTERNALS_H
#define VDO_RECOVERY_INTERNALS_H

#include "vdoRecovery.h"

#include "blockMapRecovery.h"
#include "intMap.h"
#include "journalPoint.h"
#include "types.h"
#include "waitQueue.h"

/**
 * The absolute position of an entry in the recovery journal, including
 * the sector number and the entry number within the sector.
 **/
struct recovery_point {
	sequence_number_t sequence_number; // Block sequence number
	uint8_t sector_count; // Sector number
	journal_entry_count_t entry_count; // Entry number
};

struct recovery_completion {
	/** The completion header */
	struct vdo_completion completion;
	/** The sub-task completion */
	struct vdo_completion sub_task_completion;
	/** The struct vdo in question */
	struct vdo *vdo;
	/** The struct block_allocator whose journals are being recovered */
	struct block_allocator *allocator;
	/** A buffer to hold the data read off disk */
	char *journal_data;
	/** The number of increfs */
	size_t incref_count;

	/** The entry data for the block map recovery */
	struct numbered_block_mapping *entries;
	/** The number of entries in the entry array */
	size_t entry_count;
	/**
	 * The sequence number of the first valid block for block map recovery
	 */
	sequence_number_t block_map_head;
	/**
	 * The sequence number of the first valid block for slab journal replay
	 */
	sequence_number_t slab_journal_head;
	/**
	 * The sequence number of the last valid block of the journal (if
	 * known)
	 */
	sequence_number_t tail;
	/**
	 * The highest sequence number of the journal, not the same as the tail,
	 * since the tail ignores blocks after the first hole.
	 */
	sequence_number_t highest_tail;

	/** A location just beyond the last valid entry of the journal */
	struct recovery_point tail_recovery_point;
	/** The location of the next recovery journal entry to apply */
	struct recovery_point next_recovery_point;
	/** The number of logical blocks currently known to be in use */
	block_count_t logical_blocks_used;
	/** The number of block map data blocks known to be allocated */
	block_count_t block_map_data_blocks;
	/** The journal point to give to the next synthesized decref */
	struct journal_point next_journal_point;
	/** The number of entries played into slab journals */
	size_t entries_added_to_slab_journals;

	// Decref synthesis fields

	/** An int_map for use in finding which slots are missing decrefs */
	struct int_map *slot_entry_map;
	/** The number of synthesized decrefs */
	size_t missing_decref_count;
	/** The number of incomplete decrefs */
	size_t incomplete_decref_count;
	/** The fake journal point of the next missing decref */
	struct journal_point next_synthesized_journal_point;
	/** The queue of missing decrefs */
	struct wait_queue missing_decrefs[];
};

/**
 * Convert a generic completion to a recovery_completion.
 *
 * @param completion  The completion to convert
 *
 * @return The recovery_completion
 **/
static inline struct recovery_completion * __must_check
as_vdo_recovery_completion(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type, RECOVERY_COMPLETION);
	return container_of(completion, struct recovery_completion, completion);
}

/**
 * Allocate and initialize a recovery_completion.
 *
 * @param vdo           The vdo in question
 * @param recovery_ptr  A pointer to hold the new recovery_completion
 *
 * @return VDO_SUCCESS or a status code
 **/
int __must_check
make_vdo_recovery_completion(struct vdo *vdo,
			     struct recovery_completion **recovery_ptr);

/**
 * Free a recovery_completion and all underlying structures.
 *
 * @param recovery_ptr  A pointer to the recovery completion to free
 **/
void free_vdo_recovery_completion(struct recovery_completion **recovery_ptr);

#endif // VDO_RECOVERY_INTERNALS_H
