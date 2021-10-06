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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/block-map.h#1 $
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

/**
 * Make a block map and configure it with the state read from the super block.
 *
 * @param [in]  state               The block map state from the super block
 * @param [in]  logical_blocks      The number of logical blocks for the VDO
 * @param [in]  thread_config       The thread configuration of the VDO
 * @param [in]  vdo                 The vdo
 * @param [in]  read_only_notifier  The read only mode context
 * @param [in]  journal             The recovery journal (may be NULL)
 * @param [in]  nonce               The nonce to distinguish initialized pages
 * @param [in]  cache_size          The block map cache size, in pages
 * @param [in]  maximum_age         The number of journal blocks before a
 *                                  dirtied page
 * @param [out] map_ptr             The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
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

/**
 * Quiesce all block map I/O, possibly writing out all dirty metadata.
 *
 * @param map        The block map to drain
 * @param operation  The type of drain to perform
 * @param parent     The completion to notify when the drain is complete
 **/
void drain_vdo_block_map(struct block_map *map,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent);

/**
 * Resume I/O for a quiescent block map.
 *
 * @param map     The block map to resume
 * @param parent  The completion to notify when the resume is complete
 **/
void resume_vdo_block_map(struct block_map *map, struct vdo_completion *parent);

/**
 * Prepare to grow the block map by allocating an expanded collection of trees.
 *
 * @param map                 The block map to grow
 * @param new_logical_blocks  The new logical size of the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_prepare_to_grow_block_map(struct block_map *map,
			      block_count_t new_logical_blocks);

/**
 * Get the logical size to which this block map is prepared to grow.
 *
 * @param map  The block map
 *
 * @return The new number of entries the block map will be grown to or 0 if
 *         the block map is not prepared to grow
 **/
block_count_t __must_check vdo_get_new_entry_count(struct block_map *map);

/**
 * Grow a block map on which vdo_prepare_to_grow_block_map() has already been
 *called.
 *
 * @param map     The block map to grow
 * @param parent  The object to notify when the growth is complete
 **/
void grow_vdo_block_map(struct block_map *map, struct vdo_completion *parent);

/**
 * Abandon any preparations which were made to grow this block map.
 *
 * @param map  The map which won't be grown
 **/
void vdo_abandon_block_map_growth(struct block_map *map);

/**
 * Free a block map.
 *
 * @param map  The block map to free
 **/
void free_vdo_block_map(struct block_map *map);

/**
 * Record the state of a block map for encoding in a super block.
 *
 * @param map  The block map to encode
 *
 * @return The state of the block map
 **/
struct block_map_state_2_0 __must_check
record_vdo_block_map(const struct block_map *map);

/**
 * Obtain any necessary state from the recovery journal that is needed for
 * normal block map operation.
 *
 * @param map      The map in question
 * @param journal  The journal to initialize from
 **/
void initialize_vdo_block_map_from_journal(struct block_map *map,
					   struct recovery_journal *journal);

/**
 * Get the portion of the block map for a given logical zone.
 *
 * @param map          The map
 * @param zone_number  The number of the zone
 *
 * @return The requested block map zone
 **/
struct block_map_zone * __must_check
vdo_get_block_map_zone(struct block_map *map, zone_count_t zone_number);

/**
 * Compute the logical zone on which the entry for a data_vio
 * resides
 *
 * @param data_vio  The data_vio
 *
 * @return The logical zone number for the data_vio
 **/
zone_count_t vdo_compute_logical_zone(struct data_vio *data_vio);

/**
 * Compute the block map slot in which the block map entry for a data_vio
 * resides, and cache that number in the data_vio.
 *
 * @param data_vio  The data_vio
 * @param callback  The function to call once the slot has been found
 * @param thread_id The thread on which to run the callback
 **/
void vdo_find_block_map_slot(struct data_vio *data_vio,
			     vdo_action *callback,
			     thread_id_t thread_id);

/**
 * Get number of block map entries.
 *
 * @param map  The block map
 *
 * @return The number of entries stored in the map
 **/
block_count_t __must_check
vdo_get_number_of_block_map_entries(const struct block_map *map);

/**
 * Notify the block map that the recovery journal has finished a new block.
 * This method must be called from the journal zone thread.
 *
 * @param map                    The block map
 * @param recovery_block_number  The sequence number of the finished recovery
 *                               journal block
 **/
void advance_vdo_block_map_era(struct block_map *map,
			       sequence_number_t recovery_block_number);

/**
 * Check whether a zone of the block map has drained, and if so, send a
 * notification thereof.
 *
 * @param zone  The zone to check
 **/
void vdo_block_map_check_for_drain_complete(struct block_map_zone *zone);

/**
 * Update an entry on a block map page.
 *
 * @param [in]     page           The page to update
 * @param [in]     data_vio       The data_vio making the update
 * @param [in]     pbn            The new PBN for the entry
 * @param [in]     mapping_state  The new mapping state for the entry
 * @param [in,out] recovery_lock  A reference to the current recovery sequence
 *                                number lock held by the page. Will be updated
 *                                if the lock changes to protect the new entry
 **/
void update_vdo_block_map_page(struct block_map_page *page,
			       struct data_vio *data_vio,
			       physical_block_number_t pbn,
			       enum block_mapping_state mapping_state,
			       sequence_number_t *recovery_lock);

/**
 * Get the block number of the physical block containing the data for the
 * specified logical block number. All blocks are mapped to physical block
 * zero by default, which is conventionally the zero block.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void vdo_get_mapped_block(struct data_vio *data_vio);

/**
 * Associate the logical block number for a block represented by a data_vio
 * with the physical block number in its new_mapped field.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void vdo_put_mapped_block(struct data_vio *data_vio);

/**
 * Get the stats for the block map page cache.
 *
 * @param map  The block map containing the cache
 *
 * @return The block map statistics
 **/
struct block_map_statistics __must_check
get_vdo_block_map_statistics(struct block_map *map);

#endif // BLOCK_MAP_H
