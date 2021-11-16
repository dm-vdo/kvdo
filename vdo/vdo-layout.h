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

#include "kernel-types.h"
#include "types.h"

struct vdo_layout {
	/* The current layout of the VDO */
	struct fixed_layout *layout;
	/* The next layout of the VDO */
	struct fixed_layout *next_layout;
	/* The previous layout of the VDO */
	struct fixed_layout *previous_layout;
	/* The first block in the layouts */
	physical_block_number_t starting_offset;
	/* A pointer to the copy completion (if there is one) */
	struct vdo_completion *copy_completion;
};

int __must_check vdo_decode_layout(struct fixed_layout *layout,
				   struct vdo_layout **vdo_layout_ptr);

void vdo_free_layout(struct vdo_layout *vdo_layout);

struct partition * __must_check
vdo_get_partition(struct vdo_layout *vdo_layout, enum partition_id id);

int __must_check
prepare_to_vdo_grow_layout(struct vdo_layout *vdo_layout,
			   block_count_t old_physical_blocks,
			   block_count_t new_physical_blocks,
			   struct vdo *vdo);

block_count_t __must_check
vdo_get_next_layout_size(struct vdo_layout *vdo_layout);

block_count_t __must_check
vdo_get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout);

block_count_t __must_check vdo_grow_layout(struct vdo_layout *vdo_layout);

void vdo_finish_layout_growth(struct vdo_layout *vdo_layout);

void vdo_copy_layout_partition(struct vdo_layout *layout,
			       enum partition_id id,
			       struct vdo_completion *parent);

struct fixed_layout * __must_check
vdo_get_fixed_layout(const struct vdo_layout *vdo_layout);

#endif /* VDO_LAYOUT_H */
