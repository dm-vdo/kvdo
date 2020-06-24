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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMap.h#27 $
 */

#ifndef BLOCK_MAP_H
#define BLOCK_MAP_H

#include "adminState.h"
#include "blockMapEntry.h"
#include "blockMapFormat.h"
#include "blockMapPage.h"
#include "completion.h"
#include "fixedLayout.h"
#include "statistics.h"
#include "types.h"

/**
 * Create a block map.
 *
 * @param [in]  logical_blocks     The number of logical blocks for the VDO
 * @param [in]  thread_config      The thread configuration of the VDO
 * @param [in]  root_origin        The absolute PBN of the first root page
 * @param [in]  root_count         The number of tree roots
 * @param [out] map_ptr            The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check make_block_map(block_count_t logical_blocks,
				const struct thread_config *thread_config,
				physical_block_number_t root_origin,
				block_count_t root_count,
				struct block_map **map_ptr);

/**
 * Quiesce all block map I/O, possibly writing out all dirty metadata.
 *
 * @param map        The block map to drain
 * @param operation  The type of drain to perform
 * @param parent     The completion to notify when the drain is complete
 **/
void drain_block_map(struct block_map *map,
		     AdminStateCode operation,
		     struct vdo_completion *parent);

/**
 * Resume I/O for a quiescent block map.
 *
 * @param map     The block map to resume
 * @param parent  The completion to notify when the resume is complete
 **/
void resume_block_map(struct block_map *map, struct vdo_completion *parent);

/**
 * Prepare to grow the block map by allocating an expanded collection of trees.
 *
 * @param map                 The block map to grow
 * @param new_logical_blocks  The new logical size of the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
prepare_to_grow_block_map(struct block_map *map,
			  block_count_t new_logical_blocks);

/**
 * Get the logical size to which this block map is prepared to grow.
 *
 * @param map  The block map
 *
 * @return The new number of entries the block map will be grown to or 0 if
 *         the block map is not prepared to grow
 **/
block_count_t __must_check get_new_entry_count(struct block_map *map);

/**
 * Grow a block map on which prepare_to_grow_block_map() has already been
 *called.
 *
 * @param map     The block map to grow
 * @param parent  The object to notify when the growth is complete
 **/
void grow_block_map(struct block_map *map, struct vdo_completion *parent);

/**
 * Abandon any preparations which were made to grow this block map.
 *
 * @param map  The map which won't be grown
 **/
void abandon_block_map_growth(struct block_map *map);

/**
 * Make a block map and configure it with the state read from the super block.
 * The caches are not constructed at this time.
 *
 * @param [in]  state           The block map state from the super block
 * @param [in]  logical_blocks  The number of logical blocks for the VDO
 * @param [in]  thread_config   The thread configuration of the VDO
 * @param [out] map_ptr         The pointer to hold the new block map
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check decode_block_map(struct block_map_state_2_0 state,
				  block_count_t logical_blocks,
				  const struct thread_config *thread_config,
				  struct block_map **map_ptr);

/**
 * Allocate the page caches for a block map.
 *
 * @param map                 The block map needing caches.
 * @param layer               The physical layer for the cache
 * @param read_only_notifier  The read only mode context
 * @param journal             The recovery journal (may be NULL)
 * @param nonce               The nonce to distinguish initialized pages
 * @param cache_size          The block map cache size, in pages
 * @param maximum_age         The number of journal blocks before a dirtied page
 *                            is considered old and must be written out
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
make_block_map_caches(struct block_map *map,
		      PhysicalLayer *layer,
		      struct read_only_notifier *read_only_notifier,
		      struct recovery_journal *journal,
		      nonce_t nonce,
		      page_count_t cache_size,
		      block_count_t maximum_age);

/**
 * Free a block map and null out the reference to it.
 *
 * @param map_ptr  A pointer to the block map to free
 **/
void free_block_map(struct block_map **map_ptr);

/**
 * Record the state of a block map for encoding in a super block.
 *
 * @param map  The block map to encode
 *
 * @return The state of the block map
 **/
struct block_map_state_2_0 __must_check
record_block_map(const struct block_map *map);

/**
 * Obtain any necessary state from the recovery journal that is needed for
 * normal block map operation.
 *
 * @param map      The map in question
 * @param journal  The journal to initialize from
 **/
void initialize_block_map_from_journal(struct block_map *map,
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
get_block_map_zone(struct block_map *map, zone_count_t zone_number);

/**
 * Compute the logical zone on which the entry for a data_vio
 * resides
 *
 * @param data_vio  The data_vio
 *
 * @return The logical zone number for the data_vio
 **/
zone_count_t compute_logical_zone(struct data_vio *data_vio);

/**
 * Compute the block map slot in which the block map entry for a data_vio
 * resides, and cache that number in the data_vio.
 *
 * @param data_vio  The data_vio
 * @param callback  The function to call once the slot has been found
 * @param thread_id The thread on which to run the callback
 **/
void find_block_map_slot_async(struct data_vio *data_vio,
			       vdo_action *callback,
			       thread_id_t thread_id);

/**
 * Get number of block map pages at predetermined locations.
 *
 * @param map  The block map
 *
 * @return The number of fixed pages used by the map
 **/
page_count_t __must_check
get_number_of_fixed_block_map_pages(const struct block_map *map);

/**
 * Get number of block map entries.
 *
 * @param map  The block map
 *
 * @return The number of entries stored in the map
 **/
block_count_t __must_check
get_number_of_block_map_entries(const struct block_map *map);

/**
 * Notify the block map that the recovery journal has finished a new block.
 * This method must be called from the journal zone thread.
 *
 * @param map                    The block map
 * @param recovery_block_number  The sequence number of the finished recovery
 *                               journal block
 **/
void advance_block_map_era(struct block_map *map,
			   sequence_number_t recovery_block_number);


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
void update_block_map_page(struct block_map_page *page,
			   struct data_vio *data_vio,
			   physical_block_number_t pbn,
			   BlockMappingState mapping_state,
			   sequence_number_t *recovery_lock);

/**
 * Get the block number of the physical block containing the data for the
 * specified logical block number. All blocks are mapped to physical block
 * zero by default, which is conventionally the zero block.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void get_mapped_block_async(struct data_vio *data_vio);

/**
 * Associate the logical block number for a block represented by a data_vio
 * with the physical block number in its newMapped field.
 *
 * @param data_vio  The data_vio of the block to map
 **/
void put_mapped_block_async(struct data_vio *data_vio);

/**
 * Get the stats for the block map page cache.
 *
 * @param map  The block map containing the cache
 *
 * @return The block map statistics
 **/
struct block_map_statistics __must_check
get_block_map_statistics(struct block_map *map);

#endif // BLOCK_MAP_H
