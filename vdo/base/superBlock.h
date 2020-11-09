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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/superBlock.h#13 $
 */

#ifndef SUPER_BLOCK_H
#define SUPER_BLOCK_H

#include "buffer.h"

#include "completion.h"
#include "types.h"

struct vdo_super_block;

/**
 * Make a new super block.
 *
 * @param [in]  layer            The layer on which to write this super block
 * @param [out] super_block_ptr  A pointer to hold the new super block
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_super_block(PhysicalLayer *layer,
				  struct vdo_super_block **super_block_ptr);

/**
 * Free a super block and null out the reference to it.
 *
 * @param super_block_ptr the reference to the super block to free
 **/
void free_super_block(struct vdo_super_block **super_block_ptr);

/**
 * Save a super block.
 *
 * @param super_block         The super block to save
 * @param super_block_offset  The location at which to write the super block
 * @param parent              The object to notify when the save is complete
 **/
void save_super_block(struct vdo_super_block *super_block,
		      physical_block_number_t super_block_offset,
		      struct vdo_completion *parent);

/**
 * Allocate a super block and read its contents from storage. If a load error
 * occurs before the super block's own completion can be allocated, the parent
 * will be finished with the error.
 *
 * @param [in]  parent              The completion to finish after loading the
 *                                  super block
 * @param [in]  super_block_offset  The location from which to read the super
 *                                  block
 * @param [out] super_block_ptr     A pointer to hold the super block
 **/
void load_super_block(struct vdo_completion *parent,
		      physical_block_number_t super_block_offset,
		      struct vdo_super_block **super_block_ptr);

/**
 * Get the super block codec from a super block.
 *
 * @param super_block  The super block from which to get the component data
 *
 * @return the codec
 **/
struct super_block_codec * __must_check
get_super_block_codec(struct vdo_super_block *super_block);

#endif /* SUPER_BLOCK_H */
