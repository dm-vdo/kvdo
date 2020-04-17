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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLayout.h#9 $
 */

/**
 * vdo_layout is an object which manages the layout of a VDO. It wraps
 * FixedLayout, but includes the knowledge of exactly which partitions a VDO is
 * expected to have. Because of this knowledge, the vdo_layout validates the
 * FixedLayout encoded in the super block at load time, obviating the need for
 * subsequent error checking when other modules need to get partitions from the
 * layout.
 *
 * The vdo_layout also manages the preparation and growth of the layout for
 * grow physical operations.
 **/

#ifndef VDO_LAYOUT_H
#define VDO_LAYOUT_H

#include "fixedLayout.h"
#include "types.h"

/**
 * Make a VDO layout with the specified parameters.
 *
 * @param [in]  physical_blocks   The number of physical blocks in the VDO
 * @param [in]  starting_offset   The starting offset of the layout
 * @param [in]  block_map_blocks  The size of the block map partition
 * @param [in]  journal_blocks    The size of the journal partition
 * @param [in]  summary_blocks    The size of the slab summary partition
 * @param [out] vdo_layout_ptr    A pointer to hold the new vdo_layout
 *
 * @return VDO_SUCCESS or an error
 **/
int make_vdo_layout(block_count_t physical_blocks,
		    physical_block_number_t starting_offset,
		    block_count_t block_map_blocks,
		    block_count_t journal_blocks,
		    block_count_t summary_blocks,
		    struct vdo_layout **vdo_layout_ptr)
	__attribute__((warn_unused_result));

/**
 * Decode a vdo_layout from a buffer.
 *
 * @param [in]  buffer          The buffer from which to decode
 * @param [out] vdo_layout_ptr  A pointer to hold the vdo_layout
 *
 * @return VDO_SUCCESS or an error
 **/
int decode_vdo_layout(struct buffer *buffer,
		      struct vdo_layout **vdo_layout_ptr)
	__attribute__((warn_unused_result));

/**
 * Free a vdo_layout and NULL out the reference to it.
 *
 * @param vdo_layout_ptr  The pointer to a vdo_layout to free
 **/
void free_vdo_layout(struct vdo_layout **vdo_layout_ptr);

/**
 * Get a partition from a vdo_layout. Because the layout's FixedLayout has
 * already been validated, this can not fail.
 *
 * @param vdo_layout  The vdo_layout from which to get the partition
 * @param id          The ID of the desired partition
 *
 * @return The requested partition
 **/
struct partition *get_vdo_partition(struct vdo_layout *vdo_layout,
				    partition_id id)
	__attribute__((warn_unused_result));

/**
 * Prepare the layout to be grown.
 *
 * @param vdo_layout           The layout to grow
 * @param old_physical_blocks  The current size of the VDO
 * @param new_physical_blocks  The size to which the VDO will be grown
 * @param layer                The layer being grown
 *
 * @return VDO_SUCCESS or an error code
 **/
int prepare_to_grow_vdo_layout(struct vdo_layout *vdo_layout,
			       block_count_t old_physical_blocks,
			       block_count_t new_physical_blocks,
			       PhysicalLayer *layer)
	__attribute__((warn_unused_result));

/**
 * Get the size of the next layout.
 *
 * @param vdo_layout  The layout to check
 *
 * @return The size which was specified when the layout was prepared for growth
 *         or 0 if the layout is not prepared to grow
 **/
block_count_t get_next_vdo_layout_size(struct vdo_layout *vdo_layout)
	__attribute__((warn_unused_result));

/**
 * Get the size of the next block allocator partition.
 *
 * @param vdo_layout  The vdo_layout which has been prepared to grow
 *
 * @return The size of the block allocator partition in the next layout or 0
 *         if the layout is not prepared to grow
 **/
block_count_t
get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout)
	__attribute__((warn_unused_result));

/**
 * Grow the layout by swapping in the prepared layout.
 *
 * @param vdo_layout  The layout to grow
 *
 * @return The new size of the VDO
 **/
block_count_t grow_vdo_layout(struct vdo_layout *vdo_layout)
	__attribute__((warn_unused_result));

/**
 * Revert the last growth attempt.
 *
 * @param vdo_layout  The layout to revert
 *
 * @return The reverted size (in blocks) of the VDO
 **/
block_count_t revert_vdo_layout(struct vdo_layout *vdo_layout)
	__attribute__((warn_unused_result));

/**
 * Clean up any unused resources once an attempt to grow has completed.
 *
 * @param vdo_layout  The layout
 **/
void finish_vdo_layout_growth(struct vdo_layout *vdo_layout);

/**
 * Copy a partition from the location specified in the current layout to that in
 * the next layout.
 *
 * @param layout        The vdo_layout which is prepared to grow
 * @param partition_id  The ID of the partition to copy
 * @param parent        The completion to notify when the copy is complete
 **/
void copy_partition(struct vdo_layout *layout,
		    partition_id partition_id,
		    struct vdo_completion *parent);

/**
 * Get the size of an encoded vdo_layout.
 *
 * @param vdo_layout  The vdo_layout
 *
 * @return The encoded size of the vdo_layout
 **/
size_t get_vdo_layout_encoded_size(const struct vdo_layout *vdo_layout)
	__attribute__((warn_unused_result));

/**
 * Encode a vdo_layout into a buffer.
 *
 * @param vdo_layout  The vdo_layout to encode
 * @param buffer     The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encode_vdo_layout(const struct vdo_layout *vdo_layout,
		      struct buffer *buffer)
	__attribute__((warn_unused_result));

#endif // VDO_LAYOUT_H
