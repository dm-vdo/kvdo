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

#ifndef FIXED_LAYOUT_H
#define FIXED_LAYOUT_H

#include "buffer.h"

#include "types.h"

enum partition_direction {
	VDO_PARTITION_FROM_BEGINNING,
	VDO_PARTITION_FROM_END,
};

extern const block_count_t VDO_ALL_FREE_BLOCKS;

/**
 * A fixed layout is like a traditional disk partitioning scheme.  In the
 * beginning there is one large unused area, of which parts are carved off.
 * Each carved off section has its own internal offset and size.
 **/
struct fixed_layout;
struct partition;

int __must_check make_vdo_fixed_layout(block_count_t total_blocks,
				       physical_block_number_t start_offset,
				       struct fixed_layout **layout_ptr);

void free_vdo_fixed_layout(struct fixed_layout *layout);

block_count_t __must_check
get_total_vdo_fixed_layout_size(const struct fixed_layout *layout);

int __must_check
vdo_get_fixed_layout_partition(struct fixed_layout *layout,
			       enum partition_id id,
			       struct partition **partition_ptr);

int __must_check
vdo_translate_to_pbn(const struct partition *partition,
		     physical_block_number_t partition_block_number,
		     physical_block_number_t *layer_block_number);

int __must_check
vdo_translate_from_pbn(const struct partition *partition,
		       physical_block_number_t layer_block_number,
		       physical_block_number_t *partition_block_number);

block_count_t __must_check
get_vdo_fixed_layout_blocks_available(const struct fixed_layout *layout);

int __must_check
make_vdo_fixed_layout_partition(struct fixed_layout *layout,
				enum partition_id id,
				block_count_t block_count,
				enum partition_direction direction,
				physical_block_number_t base);

block_count_t __must_check
get_vdo_fixed_layout_partition_size(const struct partition *partition);

physical_block_number_t __must_check
get_vdo_fixed_layout_partition_offset(const struct partition *partition);

physical_block_number_t __must_check
get_vdo_fixed_layout_partition_base(const struct partition *partition);

size_t __must_check
get_vdo_fixed_layout_encoded_size(const struct fixed_layout *layout);

int __must_check
encode_vdo_fixed_layout(const struct fixed_layout *layout, struct buffer *buffer);

int __must_check
decode_vdo_fixed_layout(struct buffer *buffer, struct fixed_layout **layout_ptr);

int __must_check
make_partitioned_vdo_fixed_layout(block_count_t physical_blocks,
				  physical_block_number_t starting_offset,
				  block_count_t block_map_blocks,
				  block_count_t journal_blocks,
				  block_count_t summary_blocks,
				  struct fixed_layout **layout_ptr);

#endif /* FIXED_LAYOUT_H */
