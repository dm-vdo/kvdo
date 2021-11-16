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

int __must_check
vdo_decode_block_map_state_2_0(struct buffer *buffer,
			       struct block_map_state_2_0 *state);

size_t __must_check vdo_get_block_map_encoded_size(void);

int __must_check
vdo_encode_block_map_state_2_0(struct block_map_state_2_0 state,
			       struct buffer *buffer);

page_count_t vdo_compute_block_map_page_count(block_count_t entries);

block_count_t __must_check
vdo_compute_new_forest_pages(root_count_t root_count,
			     struct boundary *old_sizes,
			     block_count_t entries,
			     struct boundary *new_sizes);

#endif /* BLOCK_MAP_FORMAT_H */
