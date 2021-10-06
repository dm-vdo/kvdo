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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo-layout.h#1 $
 */

/**
 * vdo_layout is an object which manages the layout of a VDO. It wraps
 * fixed_layout, but includes the knowledge of exactly which partitions a VDO
 * is expected to have. Because of this knowledge, the vdo_layout validates
 * the fixed_layout encoded in the super block at load time, obviating the
 * need for subsequent error checking when other modules need to get
 * partitions from the layout.
 *
 * The vdo_layout also manages the preparation and growth of the layout for
 * grow physical operations.
 **/

#ifndef VDO_LAYOUT_H
#define VDO_LAYOUT_H

#include "fixed-layout.h"
#include "kernel-types.h"
#include "types.h"

struct vdo_layout {
	// The current layout of the VDO
	struct fixed_layout *layout;
	// The next layout of the VDO
	struct fixed_layout *next_layout;
	// The previous layout of the VDO
	struct fixed_layout *previous_layout;
	// The first block in the layouts
	physical_block_number_t starting_offset;
	// A pointer to the copy completion (if there is one)
	struct vdo_completion *copy_completion;
};

/**
 * Make a vdo_layout from the fixed_layout decoded from the super block.
 *
 * @param [in]  layout          The fixed_layout from the super block
 * @param [out] vdo_layout_ptr  A pointer to hold the vdo_layout
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check decode_vdo_layout(struct fixed_layout *layout,
				   struct vdo_layout **vdo_layout_ptr);

/**
 * Free a vdo_layout.
 *
 * @param vdo_layout  The vdo_layout to free
 **/
void free_vdo_layout(struct vdo_layout *vdo_layout);

/**
 * Get a partition from a vdo_layout. Because the layout's fixed_layout has
 * already been validated, this can not fail.
 *
 * @param vdo_layout  The vdo_layout from which to get the partition
 * @param id          The ID of the desired partition
 *
 * @return The requested partition
 **/
struct partition * __must_check
get_vdo_partition(struct vdo_layout *vdo_layout, enum partition_id id);

/**
 * Prepare the layout to be grown.
 *
 * @param vdo_layout           The layout to grow
 * @param old_physical_blocks  The current size of the VDO
 * @param new_physical_blocks  The size to which the VDO will be grown
 * @param vdo                  The VDO being grown
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
prepare_to_grow_vdo_layout(struct vdo_layout *vdo_layout,
			   block_count_t old_physical_blocks,
			   block_count_t new_physical_blocks,
			   struct vdo *vdo);

/**
 * Get the size of the next layout.
 *
 * @param vdo_layout  The layout to check
 *
 * @return The size which was specified when the layout was prepared for growth
 *         or 0 if the layout is not prepared to grow
 **/
block_count_t __must_check
get_next_vdo_layout_size(struct vdo_layout *vdo_layout);

/**
 * Get the size of the next block allocator partition.
 *
 * @param vdo_layout  The vdo_layout which has been prepared to grow
 *
 * @return The size of the block allocator partition in the next layout or 0
 *         if the layout is not prepared to grow
 **/
block_count_t __must_check
vdo_get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout);

/**
 * Grow the layout by swapping in the prepared layout.
 *
 * @param vdo_layout  The layout to grow
 *
 * @return The new size of the VDO
 **/
block_count_t __must_check grow_vdo_layout(struct vdo_layout *vdo_layout);

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
 * @param layout  The vdo_layout which is prepared to grow
 * @param id      The ID of the partition to copy
 * @param parent  The completion to notify when the copy is complete
 **/
void copy_vdo_layout_partition(struct vdo_layout *layout,
			       enum partition_id id,
			       struct vdo_completion *parent);

/**
 * Get the current fixed layout of the vdo.
 *
 * @param vdo_layout  The layout
 *
 * @return The layout's current fixed layout
 **/
struct fixed_layout * __must_check
get_vdo_fixed_layout(const struct vdo_layout *vdo_layout);

#endif // VDO_LAYOUT_H
