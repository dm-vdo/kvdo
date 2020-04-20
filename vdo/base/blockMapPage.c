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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapPage.c#17 $
 */

#include "blockMapPage.h"

#include "permassert.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapTree.h"
#include "constants.h"
#include "dataVIO.h"
#include "recoveryJournal.h"
#include "statusCodes.h"
#include "types.h"

enum {
	PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const struct version_number BLOCK_MAP_4_1 = {
	.major_version = 4,
	.minor_version = 1,
};

/**********************************************************************/
bool is_current_block_map_page(const struct block_map_page *page)
{
	return are_same_version(BLOCK_MAP_4_1,
				unpack_version_number(page->version));
}

/**********************************************************************/
struct block_map_page *format_block_map_page(void *buffer,
					     nonce_t nonce,
					     physical_block_number_t pbn,
					     bool initialized)
{
	memset(buffer, 0, VDO_BLOCK_SIZE);
	struct block_map_page *page = (struct block_map_page *)buffer;
	page->version = pack_version_number(BLOCK_MAP_4_1);
	storeUInt64LE(page->header.fields.nonce, nonce);
	storeUInt64LE(page->header.fields.pbn, pbn);
	page->header.fields.initialized = initialized;
	return page;
}

/**********************************************************************/
block_map_page_validity validate_block_map_page(struct block_map_page *page,
						nonce_t nonce,
						physical_block_number_t pbn)
{
	// Make sure the page layout isn't accidentally changed by changing the
	// length of the page header.
	STATIC_ASSERT_SIZEOF(PageHeader, PAGE_HEADER_4_1_SIZE);

	if (!are_same_version(BLOCK_MAP_4_1,
			      unpack_version_number(page->version)) ||
	    !is_block_map_page_initialized(page) ||
	    (nonce != getUInt64LE(page->header.fields.nonce))) {
		return BLOCK_MAP_PAGE_INVALID;
	}

	if (pbn != get_block_map_page_pbn(page)) {
		return BLOCK_MAP_PAGE_BAD;
	}

	return BLOCK_MAP_PAGE_VALID;
}

/**********************************************************************/
void update_block_map_page(struct block_map_page *page,
			   struct data_vio *data_vio,
			   physical_block_number_t pbn,
			   BlockMappingState mapping_state,
			   sequence_number_t *recovery_lock)
{
	// Encode the new mapping.
	struct tree_lock *tree_lock = &data_vio->treeLock;
	SlotNumber slot =
		tree_lock->treeSlots[tree_lock->height].blockMapSlot.slot;
	page->entries[slot] = pack_pbn(pbn, mapping_state);

	// Adjust references (locks) on the recovery journal blocks.
	struct block_map_zone *zone =
		get_block_map_for_zone(data_vio->logical.zone);
	struct block_map *block_map = zone->block_map;
	struct recovery_journal *journal = block_map->journal;
	sequence_number_t old_locked = *recovery_lock;
	sequence_number_t new_locked = data_vio->recoverySequenceNumber;

	if ((old_locked == 0) || (old_locked > new_locked)) {
		// Acquire a lock on the newly referenced journal block.
		acquire_recovery_journal_block_reference(journal,
							 new_locked,
							 ZONE_TYPE_LOGICAL,
							 zone->zone_number);

		// If the block originally held a newer lock, release it.
		if (old_locked > 0) {
			release_recovery_journal_block_reference(journal,
								 old_locked,
								 ZONE_TYPE_LOGICAL,
								 zone->zone_number);
		}

		*recovery_lock = new_locked;
	}

	// Release the transferred lock from the data_vio.
	release_per_entry_lock_from_other_zone(journal, new_locked);
	data_vio->recoverySequenceNumber = 0;
}
