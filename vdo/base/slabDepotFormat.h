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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotFormat.h#3 $
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
} __packed;

extern const struct header SLAB_DEPOT_HEADER_2_0;

/**
 * Compute the number of slabs a depot with given parameters would have.
 *
 * @param first_block      PBN of the first data block
 * @param last_block       PBN of the last data block
 * @param slab_size_shift  Exponent for the number of blocks per slab
 *
 * @return The number of slabs
 **/
slab_count_t __must_check
compute_slab_count(physical_block_number_t first_block,
		   physical_block_number_t last_block,
		   unsigned int slab_size_shift);

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

/**
 * Configure the slab_depot for the specified storage capacity, finding the
 * number of data blocks that will fit and still leave room for the depot
 * metadata, then return the saved state for that configuration.
 *
 * @param [in]  block_count  The number of blocks in the underlying storage
 * @param [in]  first_block  The number of the first block that may be allocated
 * @param [in]  slab_config  The configuration of a single slab
 * @param [in]  zone_count   The number of zones the depot will use
 * @param [out] state        The state structure to be configured
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check configure_slab_depot(block_count_t block_count,
				      physical_block_number_t first_block,
				      struct slab_config slab_config,
				      zone_count_t zone_count,
				      struct slab_depot_state_2_0 *state);

/**
 * Measure and initialize the configuration to use for each slab.
 *
 * @param [in]  slab_size            The number of blocks per slab
 * @param [in]  slab_journal_blocks  The number of blocks for the slab journal
 * @param [out] slab_config          The slab configuration to initialize
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check configure_slab(block_count_t slab_size,
				block_count_t slab_journal_blocks,
				struct slab_config *slab_config);

/**
 * Get the number of blocks required to save a reference counts state covering
 * the specified number of data blocks.
 *
 * @param block_count  The number of physical data blocks that can be referenced
 *
 * @return The number of blocks required to save reference counts with the
 *         given block count
 **/
block_count_t __must_check
get_saved_reference_count_size(block_count_t block_count);

#endif // SLAB_DEPOT_FORMAT_H
