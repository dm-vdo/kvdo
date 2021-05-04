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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoResize.h#1 $
 */

#ifndef VDO_RESIZE_H
#define VDO_RESIZE_H

#include "types.h"

/**
 * Make the completion for an asynchronous resize.
 *
 * @param vdo                	The vdo
 * @param new_physical_blocks 	The new physical size in blocks
 * @param completion_ptr      	A pointer to hold the completion
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_resize_vdo_completion(struct vdo *vdo,
			   block_count_t new_physical_blocks,
			   struct vdo_completion **completion_ptr);

/**
 * Free the completion for an asynchronous resize, and NULL out the
 * reference to it.
 *
 * @param completion_ptr  A reference to the completion to free
 **/
void free_resize_vdo_completion(struct vdo_completion **completion_ptr);

/**
 * Grow the physical size of the vdo. This method may only be called when the
 * vdo has been suspended and must not be called from a base thread.
 *
 * @param vdo                	The vdo to resize
 * @param new_physical_blocks	The new physical size in blocks
 *
 * @return VDO_SUCCESS or an error
 **/
int perform_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks);

/**
 * Prepare to resize the vdo, allocating memory as needed.
 *
 * @param vdo                	The vdo
 * @param new_physical_blocks	The new physical size in blocks
 **/
int __must_check
prepare_to_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks);

#endif /* VDO_RESIZE_H */
