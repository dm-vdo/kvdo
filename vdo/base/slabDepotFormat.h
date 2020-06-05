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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotFormat.h#1 $
 */

#ifndef SLAB_DEPOT_FORMAT_H
#define SLAB_DEPOT_FORMAT_H

#include "buffer.h"

#include "header.h"
#include "types.h"

struct slab_depot_state_2_0 {
	struct slab_config slab_config;
	physical_block_number_t first_block;
	physical_block_number_t last_block;
	zone_count_t zone_count;
} __attribute__((packed));

extern const struct header SLAB_DEPOT_HEADER_2_0;

/**
 * Get the size of the encoded state of a slab depot.
 *
 * @return The encoded size of the depot's state
 **/
size_t __must_check get_slab_depot_encoded_size(void);

/**
 * Encode the state of a slab depot into a buffer.
 *
 * @param state   The state to encode
 * @param buffer  The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check
encode_slab_depot_state_2_0(struct slab_depot_state_2_0 state,
			    struct buffer *buffer);

/**
 * Decode slab depot component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
decode_slab_depot_state_2_0(struct buffer *buffer,
			    struct slab_depot_state_2_0 *state);

#endif // SLAB_DEPOT_FORMAT_H
