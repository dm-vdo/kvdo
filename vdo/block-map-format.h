/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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
