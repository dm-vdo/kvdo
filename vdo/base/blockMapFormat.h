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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapFormat.h#1 $
 */

#ifndef BLOCK_MAP_FORMAT_H
#define BLOCK_MAP_FORMAT_H

#include "buffer.h"

#include "header.h"
#include "types.h"

struct block_map_state_2_0 {
	physical_block_number_t flat_page_origin;
	block_count_t flat_page_count;
	physical_block_number_t root_origin;
	block_count_t root_count;
} __attribute__((packed));

extern const struct header BLOCK_MAP_HEADER_2_0;

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check decode_block_map_state_2_0(struct buffer *buffer,
					    struct block_map_state_2_0 *state);

/**
 * Get the size of the encoded state of a block map.
 *
 * @return The encoded size of the map's state
 **/
size_t __must_check get_block_map_encoded_size(void);

/**
 * Encode the state of a block map into a buffer.
 *
 * @param state   The block map state to encode
 * @param buffer  The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check
encode_block_map_state_2_0(struct block_map_state_2_0 state,
			   struct buffer *buffer);

#endif // BLOCK_MAP_FORMAT_H
