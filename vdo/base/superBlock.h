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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/superBlock.h#7 $
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
int make_super_block(PhysicalLayer *layer,
		     struct vdo_super_block **super_block_ptr)
	__attribute__((warn_unused_result));

/**
 * Free a super block and null out the reference to it.
 *
 * @param super_block_ptr the reference to the super block to free
 **/
void free_super_block(struct vdo_super_block **super_block_ptr);

/**
 * Save a super block.
 *
 * @param layer               The physical layer on which to save the
 *                            super block
 * @param super_block         The super block to save
 * @param super_block_offset  The location of the super block
 *
 * @return VDO_SUCCESS or an error
 **/
int save_super_block(PhysicalLayer *layer, struct vdo_super_block *super_block,
		     physical_block_number_t super_block_offset)
	__attribute__((warn_unused_result));

/**
 * Save a super block asynchronously.
 *
 * @param super_block         The super block to save
 * @param super_block_offset  The location at which to write the super block
 * @param parent              The object to notify when the save is complete
 **/
void save_super_block_async(struct vdo_super_block *super_block,
			    physical_block_number_t super_block_offset,
			    struct vdo_completion *parent);

/**
 * Allocate a super block and read its contents from storage.
 *
 * @param [in]  layer               The layer from which to load the super block
 * @param [in]  super_block_offset  The location from which to read the super
 *                                  block
 * @param [out] super_block_ptr     A pointer to hold the loaded super block
 *
 * @return VDO_SUCCESS or an error
 **/
int load_super_block(PhysicalLayer *layer,
		     physical_block_number_t super_block_offset,
		     struct vdo_super_block **super_block_ptr)
	__attribute__((warn_unused_result));

/**
 * Allocate a super block and read its contents from storage asynchronously.
 * If a load error occurs before the super block's own completion can be
 * allocated, the parent will be finished with the error.
 *
 * @param [in]  parent              The completion to finish after loading the
 *                                  super block
 * @param [in]  super_block_offset  The location from which to read the super
 *                                  block
 * @param [out] super_block_ptr     A pointer to hold the super block
 **/
void load_super_block_async(struct vdo_completion *parent,
			    physical_block_number_t super_block_offset,
			    struct vdo_super_block **super_block_ptr);

/**
 * Get a buffer which contains the component data from a super block.
 *
 * @param super_block  The super block from which to get the component data
 *
 * @return the component data in a buffer
 **/
struct buffer *get_component_buffer(struct vdo_super_block *super_block)
	__attribute__((warn_unused_result));

/**
 * Get the release version number that was loaded from the volume when the
 * super_block was decoded.
 *
 * @param super_block  The super block to query
 *
 * @return the release version number that was decoded from the volume
 **/
ReleaseVersionNumber
get_loaded_release_version(const struct vdo_super_block *super_block)
	__attribute__((warn_unused_result));

/**
 * Get the encoded size of the fixed (non-component data) portion of a super
 * block (this is for unit testing).
 *
 * @return The encoded size of the fixed portion of the super block
 **/
size_t get_fixed_super_block_size(void) __attribute__((warn_unused_result));

#endif /* SUPER_BLOCK_H */
