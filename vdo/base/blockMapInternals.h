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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapInternals.h#29 $
 */

#ifndef BLOCK_MAP_INTERNALS_H
#define BLOCK_MAP_INTERNALS_H

#include "adminState.h"
#include "blockMapEntry.h"
#include "blockMapTree.h"
#include "completion.h"
#include "dirtyLists.h"
#include "header.h"
#include "intMap.h"
#include "ringNode.h"
#include "types.h"
#include "vdoPageCache.h"
#include "vioPool.h"

/**
 * The per-zone fields used by the block map tree.
 **/
struct block_map_tree_zone {
	/** The struct block_map_zone which owns this tree zone */
	struct block_map_zone *map_zone;
	/** The lists of dirty tree pages */
	struct dirty_lists *dirty_lists;
	/** The number of tree lookups in progress */
	VIOCount active_lookups;
	/** The map of pages currently being loaded */
	struct int_map *loading_pages;
	/** The pool of vios for tree I/O */
	struct vio_pool *vio_pool;
	/** The tree page which has issued or will be issuing a flush */
	struct tree_page *flusher;
	/** The queue of pages waiting for a flush so they can be written out */
	struct wait_queue flush_waiters;
	/** The generation after the most recent flush */
	uint8_t generation;
	/** The oldest active generation */
	uint8_t oldest_generation;
	/** The counts of dirty pages in each generation */
	uint32_t dirty_page_counts[256];
};

/**
 * The per-zone fields of the block map.
 **/
struct block_map_zone {
	/** The number of the zone this is */
	ZoneCount zone_number;
	/** The ID of this zone's logical thread */
	ThreadID thread_id;
	/** The block_map which owns this block_map_zone */
	struct block_map *block_map;
	/** The read_only_notifier of the VDO */
	struct read_only_notifier *read_only_notifier;
	/** The page cache for this zone */
	struct vdo_page_cache *page_cache;
	/** The per-zone portion of the tree for this zone */
	struct block_map_tree_zone tree_zone;
	/** The administrative state of the zone */
	struct admin_state state;
};

struct block_map {
	/** The manager for block map actions */
	struct action_manager *action_manager;
	/** The count of pages in the linear part of the block map */
	block_count_t flat_page_count;
	/**
	 * The absolute PBN of the first root of the tree part of the block map
	 */
	PhysicalBlockNumber root_origin;
	/** The count of root pages of the tree part of the block map */
	block_count_t root_count;

	/** The era point we are currently distributing to the zones */
	SequenceNumber current_era_point;
	/** The next era point, not yet distributed to any zone */
	SequenceNumber pending_era_point;

	/** The number of entries in block map */
	block_count_t entry_count;
	/** The VDO's nonce, for the pages */
	Nonce nonce;
	/** The recovery journal for this map */
	struct recovery_journal *journal;

	/** The trees for finding block map pages */
	struct forest *forest;
	/** The expanded trees awaiting growth */
	struct forest *next_forest;
	/** The number of entries after growth */
	block_count_t next_entry_count;

	/** The number of logical zones */
	ZoneCount zone_count;
	/** The per zone block map structure */
	struct block_map_zone zones[];
};

/**
 * Compute the number of pages required for a block map with the specified
 * parameters.
 *
 * @param entries   The number of block map entries
 *
 * @return The number of pages required
 **/
page_count_t compute_block_map_page_count(block_count_t entries);

/**
 * Compute the number of the block map page on which the entry for a given
 * logical block resides.
 *
 * @param lbn  The logical block number whose page is desired
 *
 * @return The number of the block map page containing the entry for
 *         the given logical block number
 **/
__attribute__((warn_unused_result)) static inline page_number_t
compute_page_number(logical_block_number_t lbn)
{
	return (lbn / BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**
 * Find the block map page slot in which the entry for a given logical
 * block resides.
 *
 * @param lbn  The logical block number whose slot
 *
 * @return The slot containing the entry for the given logical block number
 **/
__attribute__((warn_unused_result)) static inline SlotNumber
compute_slot(logical_block_number_t lbn)
{
	return (lbn % BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**
 * Check whether a zone of the block map has drained, and if so, send a
 * notification thereof.
 *
 * @param zone  The zone to check
 **/
void check_for_drain_complete(struct block_map_zone *zone);

#endif // BLOCK_MAP_INTERNALS_H
