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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapFormat.c#3 $
 */

#include "blockMapFormat.h"

#include "buffer.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "types.h"

const struct header BLOCK_MAP_HEADER_2_0 = {
	.id = BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

/**
 * Decode block map component state version 2.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
int decode_block_map_state_2_0(struct buffer *buffer,
			       struct block_map_state_2_0 *state)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&BLOCK_MAP_HEADER_2_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	physical_block_number_t flat_page_origin;
	result = get_uint64_le_from_buffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(flat_page_origin == BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			BLOCK_MAP_FLAT_PAGE_ORIGIN,
			state->flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t flat_page_count;
	result = get_uint64_le_from_buffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(flat_page_count == 0,
			"Flat page count must be 0 (recorded as %llu)",
			state->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	physical_block_number_t root_origin;
	result = get_uint64_le_from_buffer(buffer, &root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t root_count;
	result = get_uint64_le_from_buffer(buffer, &root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t decoded_size = initial_length - content_length(buffer);
	result = ASSERT(BLOCK_MAP_HEADER_2_0.size == decoded_size,
			"decoded block map component size must match header size");
	if (result != VDO_SUCCESS) {
		return result;
	}

	*state = (struct block_map_state_2_0) {
		.flat_page_origin = flat_page_origin,
		.flat_page_count = flat_page_count,
		.root_origin = root_origin,
		.root_count = root_count,
	};

	return VDO_SUCCESS;
}

/**********************************************************************/
size_t get_block_map_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct block_map_state_2_0);
}

/**********************************************************************/
int encode_block_map_state_2_0(struct block_map_state_2_0 state,
			       struct buffer *buffer)
{
	int result = encode_header(&BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = put_uint64_le_into_buffer(buffer, state.flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**********************************************************************/
page_count_t compute_block_map_page_count(block_count_t entries)
{
	return compute_bucket_count(entries, BLOCK_MAP_ENTRIES_PER_PAGE);
}

/**********************************************************************/
block_count_t compute_new_forest_pages(root_count_t root_count,
				       struct boundary *old_sizes,
				       block_count_t entries,
				       struct boundary *new_sizes)
{
	page_count_t leaf_pages
		= max_page_count(compute_block_map_page_count(entries), 1);
	page_count_t level_size = compute_bucket_count(leaf_pages, root_count);
	block_count_t total_pages = 0;
	height_t height;
	for (height = 0; height < BLOCK_MAP_TREE_HEIGHT; height++) {
		level_size = compute_bucket_count(level_size,
						  BLOCK_MAP_ENTRIES_PER_PAGE);
		new_sizes->levels[height] = level_size;
		block_count_t new_pages = level_size;
		if (old_sizes != NULL) {
			new_pages -= old_sizes->levels[height];
		}
		total_pages += (new_pages * root_count);
	}

	return total_pages;
}

