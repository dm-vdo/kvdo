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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/fixedLayout.h#3 $
 */

#ifndef FIXED_LAYOUT_H
#define FIXED_LAYOUT_H

#include "buffer.h"

#include "types.h"

typedef enum {
	FROM_BEGINNING,
	FROM_END,
} partition_direction;

extern const BlockCount ALL_FREE_BLOCKS;

/**
 * A fixed layout is like a traditional disk partitioning scheme.  In the
 * beginning there is one large unused area, of which parts are carved off.
 * Each carved off section has its own internal offset and size.
 **/
struct fixed_layout;
struct partition;

/**
 * Make an unpartitioned fixed layout.
 *
 * @param [in]  total_blocks  The total size of the layout, in blocks
 * @param [in]  start_offset  The block offset in the underlying layer at which
 *                           the fixed layout begins
 * @param [out] layout_ptr    The pointer to hold the resulting layout
 *
 * @return a success or error code
 **/
int make_fixed_layout(BlockCount total_blocks,
		      PhysicalBlockNumber start_offset,
		      struct fixed_layout **layout_ptr)
	__attribute__((warn_unused_result));

/**
 * Free the fixed layout and null out the reference to it.
 *
 * @param layout_ptr  The reference to the layout to free
 *
 * @note all partitions created by this layout become invalid pointers
 **/
void free_fixed_layout(struct fixed_layout **layout_ptr);

/**
 * Get the total size of the layout in blocks.
 *
 * @param layout  The layout
 *
 * @return The size of the layout
 **/
BlockCount get_total_fixed_layout_size(const struct fixed_layout *layout)
	__attribute__((warn_unused_result));

/**
 * Get a partition by id.
 *
 * @param layout         The layout from which to get a partition
 * @param id             The id of the partition
 * @param partition_ptr  A pointer to hold the partition
 *
 * @return VDO_SUCCESS or an error
 **/
int get_partition(struct fixed_layout *layout,
		  PartitionID id,
		  struct partition **partition_ptr)
	__attribute__((warn_unused_result));

/**
 * Translate a block number from the partition's view to the layer's
 *
 * @param partition               The partition to use for translation
 * @param partition_block_number  The block number relative to the partition
 * @param layer_block_number      The block number relative to the layer
 *
 * @return  VDO_SUCCESS or an error code
 **/
int translate_to_pbn(const struct partition *partition,
		     PhysicalBlockNumber partition_block_number,
		     PhysicalBlockNumber *layer_block_number)
	__attribute__((warn_unused_result));

/**
 * Translate a block number from the layer's view to the partition's.
 * This is the inverse of translate_to_pbn().
 *
 * @param partition               The partition to use for translation
 * @param layer_block_number      The block number relative to the layer
 * @param partition_block_number  The block number relative to the partition
 *
 * @return  VDO_SUCCESS or an error code
 **/
int translate_from_pbn(const struct partition *partition,
		       PhysicalBlockNumber layer_block_number,
		       PhysicalBlockNumber *partition_block_number)
	__attribute__((warn_unused_result));

/**
 * Return the number of unallocated blocks available.
 *
 * @param layout        the fixed layout
 *
 * @return the number of blocks yet unallocated to partitions
 **/
BlockCount get_fixed_layout_blocks_available(const struct fixed_layout *layout)
	__attribute__((warn_unused_result));

/**
 * Create a new partition from the beginning or end of the unused space
 * within a fixed layout.
 *
 * @param   layout           the fixed layout
 * @param   id               the id of the partition to make
 * @param   block_count      the number of blocks to carve out, if set
 *                           to ALL_FREE_BLOCKS, all remaining blocks will
 *                           be used
 * @param   direction        whether to carve out from beginning or end
 * @param   base             the number of the first block in the partition
 *                           from the point of view of its users
 *
 * @return a success or error code, particularly
 *      VDO_NO_SPACE if there are less than block_count blocks remaining
 **/
int make_fixed_layout_partition(struct fixed_layout *layout,
				PartitionID id,
				BlockCount block_count,
				partition_direction direction,
				PhysicalBlockNumber base)
	__attribute__((warn_unused_result));

/**
 * Return the size in blocks of a partition.
 *
 * @param partition       a partition of the fixedLayout
 *
 * @return the size of the partition in blocks
 **/
BlockCount get_fixed_layout_partition_size(const struct partition *partition)
	__attribute__((warn_unused_result));

/**
 * Get the first block of the partition in the layout.
 *
 * @param partition       a partition of the fixedLayout
 *
 * @return the partition's offset in blocks
 **/
PhysicalBlockNumber
get_fixed_layout_partition_offset(const struct partition *partition)
	__attribute__((warn_unused_result));

/**
 * Get the number of the first block in the partition from the partition users
 * point of view.
 *
 * @param partition a partition of the fixedLayout
 *
 * @return the number of the first block in the partition
 **/
PhysicalBlockNumber
get_fixed_layout_partition_base(const struct partition *partition)
	__attribute__((warn_unused_result));

/**
 * Get the size of an encoded layout
 *
 * @param layout The layout
 *
 * @return The encoded size of the layout
 **/
size_t get_fixed_layout_encoded_size(const struct fixed_layout *layout)
	__attribute__((warn_unused_result));

/**
 * Encode a layout into a buffer.
 *
 * @param layout The layout to encode
 * @param buffer The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encode_fixed_layout(const struct fixed_layout *layout, Buffer *buffer)
	__attribute__((warn_unused_result));

/**
 * Decode a fixed layout from a buffer.
 *
 * @param [in]  buffer     The buffer from which to decode
 * @param [out] layout_ptr A pointer to hold the layout
 *
 * @return VDO_SUCCESS or an error
 **/
int decode_fixed_layout(Buffer *buffer, struct fixed_layout **layout_ptr)
	__attribute__((warn_unused_result));

#endif // FIXED_LAYOUT_H
