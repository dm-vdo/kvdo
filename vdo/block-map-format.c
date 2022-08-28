// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "block-map-format.h"

#include "buffer.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "num-utils.h"
#include "status-codes.h"
#include "types.h"

const struct header VDO_BLOCK_MAP_HEADER_2_0 = {
	.id = VDO_BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

int vdo_decode_block_map_state_2_0(struct buffer *buffer,
				   struct block_map_state_2_0 *state)
{
	size_t initial_length, decoded_size;
	block_count_t flat_page_count, root_count;
	physical_block_number_t flat_page_origin, root_origin;
	struct header header;
	int result = vdo_decode_header(buffer, &header);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_validate_header(&VDO_BLOCK_MAP_HEADER_2_0, &header, true,
				     __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = get_uint64_le_from_buffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(flat_page_origin == VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			(unsigned long long) state->flat_page_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(flat_page_count == 0,
			"Flat page count must be 0 (recorded as %llu)",
			(unsigned long long) state->flat_page_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &root_origin);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &root_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == decoded_size,
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

size_t vdo_get_block_map_encoded_size(void)
{
	return VDO_ENCODED_HEADER_SIZE + sizeof(struct block_map_state_2_0);
}

int vdo_encode_block_map_state_2_0(struct block_map_state_2_0 state,
				   struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result = vdo_encode_header(&VDO_BLOCK_MAP_HEADER_2_0, buffer);

	if (result != UDS_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

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

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

page_count_t vdo_compute_block_map_page_count(block_count_t entries)
{
	return DIV_ROUND_UP(entries, VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
}

/*
 * Compute the number of pages which must be allocated at each level in order
 * to grow the forest to a new number of entries.
 * @entries: The new number of entries the block map must address
 *
 * @return: The total number of non-leaf pages required
 */
block_count_t vdo_compute_new_forest_pages(root_count_t root_count,
					   struct boundary *old_sizes,
					   block_count_t entries,
					   struct boundary *new_sizes)
{
	page_count_t leaf_pages
		= max(vdo_compute_block_map_page_count(entries), 1U);
	page_count_t level_size = DIV_ROUND_UP(leaf_pages, root_count);
	block_count_t total_pages = 0;
	height_t height;

	for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT; height++) {
		block_count_t new_pages;

		level_size = DIV_ROUND_UP(level_size,
					  VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
		new_sizes->levels[height] = level_size;
		new_pages = level_size;
		if (old_sizes != NULL) {
			new_pages -= old_sizes->levels[height];
		}
		total_pages += (new_pages * root_count);
	}

	return total_pages;
}

