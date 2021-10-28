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

#ifndef BLOCK_MAP_FORMAT_H
#define BLOCK_MAP_FORMAT_H

#include "buffer.h"

#include "constants.h"
#include "header.h"
#include "types.h"

struct block_map_state_2_0 {
	physical_block_number_t flat_page_origin;
	block_count_t flat_page_count;
	physical_block_number_t root_origin;
	block_count_t root_count;
} __packed;

struct boundary {
	page_number_t levels[VDO_BLOCK_MAP_TREE_HEIGHT];
};

extern const struct header VDO_BLOCK_MAP_HEADER_2_0;

/**
 * Compute the number of the block map page on which the entry for a given
 * logical block resides.
 *
 * @param lbn  The logical block number whose page is desired
 *
 * @return The number of the block map page containing the entry for
 *         the given logical block number
 **/
static inline page_number_t __must_check
vdo_compute_page_number(logical_block_number_t lbn)
{
	return (lbn / VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**
 * Find the block map page slot in which the entry for a given logical
 * block resides.
 *
 * @param lbn  The logical block number whose slot
 *
 * @return The slot containing the entry for the given logical block number
 **/
static inline slot_number_t __must_check
vdo_compute_slot(logical_block_number_t lbn)
{
	return (lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
decode_vdo_block_map_state_2_0(struct buffer *buffer,
			       struct block_map_state_2_0 *state);

/**
 * Get the size of the encoded state of a block map.
 *
 * @return The encoded size of the map's state
 **/
size_t __must_check get_vdo_block_map_encoded_size(void);

/**
 * Encode the state of a block map into a buffer.
 *
 * @param state   The block map state to encode
 * @param buffer  The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check
encode_vdo_block_map_state_2_0(struct block_map_state_2_0 state,
			       struct buffer *buffer);

/**
 * Compute the number of pages required for a block map with the specified
 * parameters.
 *
 * @param entries   The number of block map entries
 *
 * @return The number of pages required
 **/
page_count_t compute_vdo_block_map_page_count(block_count_t entries);

/**
 * Compute the number of pages which must be allocated at each level in order
 * to grow the forest to a new number of entries.
 *
 * @param [in]  root_count       The number of roots
 * @param [in]  old_sizes        The current size of the forest at each level
 * @param [in]  entries          The new number of entries the block map must
 *                               address
 * @param [out] new_sizes        The new size of the forest at each level
 *
 * @return The total number of non-leaf pages required
 **/
block_count_t __must_check
vdo_compute_new_forest_pages(root_count_t root_count,
			     struct boundary *old_sizes,
			     block_count_t entries,
			     struct boundary *new_sizes);

#endif // BLOCK_MAP_FORMAT_H
