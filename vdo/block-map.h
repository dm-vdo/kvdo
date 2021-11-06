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

#ifndef BLOCK_MAP_H
#define BLOCK_MAP_H

#include "admin-state.h"
#include "block-map-entry.h"
#include "block-map-format.h"
#include "block-map-page.h"
#include "block-map-tree.h"
#include "completion.h"
#include "dirty-lists.h"
#include "fixed-layout.h"
#include "header.h"
#include "int-map.h"
#include "statistics.h"
#include "types.h"
#include "vdo-page-cache.h"
#include "vio-pool.h"

/**
 * The per-zone fields used by the block map tree.
 **/
struct block_map_tree_zone {
	/** The struct block_map_zone which owns this tree zone */
	struct block_map_zone *map_zone;
	/** The lists of dirty tree pages */
	struct dirty_lists *dirty_lists;
	/** The number of tree lookups in progress */
	vio_count_t active_lookups;
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
	zone_count_t zone_number;
	/** The ID of this zone's logical thread */
	thread_id_t thread_id;
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
	/**
	 * The absolute PBN of the first root of the tree part of the block map
	 */
	physical_block_number_t root_origin;
	/** The count of root pages of the tree part of the block map */
	block_count_t root_count;

	/** The era point we are currently distributing to the zones */
	sequence_number_t current_era_point;
	/** The next era point, not yet distributed to any zone */
	sequence_number_t pending_era_point;

	/** The number of entries in block map */
	block_count_t entry_count;
	/** The VDO's nonce, for the pages */
	nonce_t nonce;
	/** The recovery journal for this map */
	struct recovery_journal *journal;

	/** The trees for finding block map pages */
	struct forest *forest;
	/** The expanded trees awaiting growth */
	struct forest *next_forest;
	/** The number of entries after growth */
	block_count_t next_entry_count;

	/** The number of logical zones */
	zone_count_t zone_count;
	/** The per zone block map structure */
	struct block_map_zone zones[];
};

int __must_check
decode_vdo_block_map(struct block_map_state_2_0 state,
		     block_count_t logical_blocks,
		     const struct thread_config *thread_config,
		     struct vdo *vdo,
		     struct read_only_notifier *read_only_notifier,
		     struct recovery_journal *journal,
		     nonce_t nonce,
		     page_count_t cache_size,
		     block_count_t maximum_age,
		     struct block_map **map_ptr);

void drain_vdo_block_map(struct block_map *map,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent);

void resume_vdo_block_map(struct block_map *map, struct vdo_completion *parent);

int __must_check
vdo_prepare_to_grow_block_map(struct block_map *map,
			      block_count_t new_logical_blocks);

block_count_t __must_check vdo_get_new_entry_count(struct block_map *map);

void grow_vdo_block_map(struct block_map *map, struct vdo_completion *parent);

void vdo_abandon_block_map_growth(struct block_map *map);

void free_vdo_block_map(struct block_map *map);

struct block_map_state_2_0 __must_check
record_vdo_block_map(const struct block_map *map);

void initialize_vdo_block_map_from_journal(struct block_map *map,
					   struct recovery_journal *journal);

struct block_map_zone * __must_check
vdo_get_block_map_zone(struct block_map *map, zone_count_t zone_number);

zone_count_t vdo_compute_logical_zone(struct data_vio *data_vio);

void vdo_find_block_map_slot(struct data_vio *data_vio,
			     vdo_action *callback,
			     thread_id_t thread_id);

block_count_t __must_check
vdo_get_number_of_block_map_entries(const struct block_map *map);

void advance_vdo_block_map_era(struct block_map *map,
			       sequence_number_t recovery_block_number);

void vdo_block_map_check_for_drain_complete(struct block_map_zone *zone);

void update_vdo_block_map_page(struct block_map_page *page,
			       struct data_vio *data_vio,
			       physical_block_number_t pbn,
			       enum block_mapping_state mapping_state,
			       sequence_number_t *recovery_lock);

void vdo_get_mapped_block(struct data_vio *data_vio);

void vdo_put_mapped_block(struct data_vio *data_vio);

struct block_map_statistics __must_check
get_vdo_block_map_statistics(struct block_map *map);

#endif /* BLOCK_MAP_H */
