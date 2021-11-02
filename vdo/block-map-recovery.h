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
 */

#ifndef BLOCK_MAP_RECOVERY_H
#define BLOCK_MAP_RECOVERY_H

#include "block-map.h"
#include "block-mapping-state.h"
#include "types.h"

/**
 * An explicitly numbered block mapping. Numbering the mappings allows them to
 * be sorted by logical block number during recovery while still preserving
 * the relative order of journal entries with the same logical block number.
 **/
struct numbered_block_mapping {
	/* Block map slot to map */
	struct block_map_slot block_map_slot;
	/* The encoded block map entry for the LBN */
	struct block_map_entry block_map_entry;
	/* The serial number to use during replay */
	uint32_t number;
} __packed;

/**
 * Recover the block map (normal rebuild).
 *
 * @param vdo              The vdo
 * @param entry_count      The number of journal entries
 * @param journal_entries  An array of journal entries to process
 * @param parent           The completion to notify when the rebuild
 *                         is complete
 **/
void recover_vdo_block_map(struct vdo *vdo,
			   block_count_t entry_count,
			   struct numbered_block_mapping *journal_entries,
			   struct vdo_completion *parent);

#endif /* BLOCK_MAP_RECOVERY_H */
