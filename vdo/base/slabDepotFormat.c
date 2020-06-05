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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotFormat.c#1 $
 */

#include "slabDepotFormat.h"

#include "permassert.h"

#include "buffer.h"
#include "header.h"
#include "types.h"

const struct header SLAB_DEPOT_HEADER_2_0 = {
	.id = SLAB_DEPOT,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct slab_depot_state_2_0),
};

/**********************************************************************/
size_t get_slab_depot_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct slab_depot_state_2_0);
}

/**
 * Encode a slab config into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encode_slab_config(const struct slab_config *config,
			      struct buffer *buffer)
{
	int result = put_uint64_le_into_buffer(buffer, config->slab_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->reference_count_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->slab_journal_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
				           config->slab_journal_flushing_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
				           config->slab_journal_blocking_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer,
				         config->slab_journal_scrubbing_threshold);
}

/**********************************************************************/
int encode_slab_depot_state_2_0(struct slab_depot_state_2_0 state,
				struct buffer *buffer)
{
	int result = encode_header(&SLAB_DEPOT_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = encode_slab_config(&state.slab_config, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_byte(buffer, state.zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(SLAB_DEPOT_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * Decode a slab config from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decode_slab_config(struct buffer *buffer,
			      struct slab_config *config)
{
	block_count_t count;
	int result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->data_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->reference_count_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_flushing_threshold = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocking_threshold = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_scrubbing_threshold = count;

	return UDS_SUCCESS;
}

/**********************************************************************/
int decode_slab_depot_state_2_0(struct buffer *buffer,
				struct slab_depot_state_2_0 *state)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&SLAB_DEPOT_HEADER_2_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);
	result = decode_slab_config(buffer, &state->slab_config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state->first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &state->last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_byte(buffer, &state->zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t decoded_size = initial_length - content_length(buffer);
	return ASSERT(SLAB_DEPOT_HEADER_2_0.size == decoded_size,
		      "decoded slab depot component size must match header size");
}
