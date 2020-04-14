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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/extent.h#10 $
 */

#ifndef EXTENT_H
#define EXTENT_H

#include "permassert.h"

#include "completion.h"
#include "types.h"
#include "vio.h"

/**
 * A chain of vios which are part of the same request. An extent contains
 * a chain of at least 'count' vios. The 'next' pointer of the last vio
 * in the extent (as indicated by the count) may not be NULL, but it is not
 * part of the extent. A vio may belong to a single extent.
 **/
struct vdo_extent {
	// The completion for asynchronous extent processing
	struct vdo_completion completion;
	// The number of vios in the extent
	block_count_t count;
	// The number of completed vios in the extent
	block_count_t complete_count;
	// The vios in the extent
	struct vio *vios[];
};

/**
 * Convert a generic vdo_completion to a vdo_extent.
 *
 * @param completion The completion to convert
 *
 * @return The completion as an extent
 **/
static inline struct vdo_extent *
as_vdo_extent(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, VDO_EXTENT_COMPLETION);
	return container_of(completion, struct vdo_extent, completion);
}

/**
 * Convert a vdo_extent to a vdo_completion.
 *
 * @param extent The extent to convert
 *
 * @return The extent as a vdo_completion
 **/
static inline struct vdo_completion *
extent_as_completion(struct vdo_extent *extent)
{
	return &extent->completion;
}

/**
 * Create vdo_extent.
 *
 * @param [in]  layer        The layer
 * @param [in]  vio_type     The usage type to assign to the vios in the extent
 *                           (data / block map / journal)
 * @param [in]  priority     The relative priority to assign to the vios
 * @param [in]  block_count  The number of blocks in the buffer
 * @param [in]  data         The buffer
 * @param [out] extent_ptr   A pointer to hold the new extent
 *
 * @return VDO_SUCCESS or an error
 **/
int create_extent(PhysicalLayer *layer, vio_type vio_type,
		  vio_priority priority,
		  block_count_t block_count, char *data,
		  struct vdo_extent **extent_ptr)
	__attribute__((warn_unused_result));

/**
 * Free an extent and null out the reference to it.
 *
 * @param [in,out] extent_ptr   The reference to the extent to free
 **/
void free_extent(struct vdo_extent **extent_ptr);

/**
 * Read metadata from the underlying storage.
 *
 * @param extent       The extent to read
 * @param start_block  The physical block number of the first block
 *                     in the extent
 * @param count        The number of blocks to read (must be less than or
 *                     equal to the length of the extent)
 **/
void read_partial_metadata_extent(struct vdo_extent *extent,
				  PhysicalBlockNumber start_block,
				  block_count_t count);

/**
 * Read metadata from the underlying storage.
 *
 * @param extent       The extent to read
 * @param start_block  The physical block number of the first block
 *                     in the extent
 **/
static inline void read_metadata_extent(struct vdo_extent *extent,
					PhysicalBlockNumber start_block)
{
	read_partial_metadata_extent(extent, start_block, extent->count);
}

/**
 * Write metadata to the underlying storage.
 *
 * @param extent       The extent to write
 * @param start_block  The physical block number of the first block in the
 *                     extent
 * @param count        The number of blocks to read (must be less than or
 *                     equal to the length of the extent)
 **/
void write_partial_metadata_extent(struct vdo_extent *extent,
				   PhysicalBlockNumber start_block,
				   block_count_t count);
/**
 * Write metadata to the underlying storage.
 *
 * @param extent       The extent to write
 * @param start_block  The physical block number of the first block in the
 *                     extent
 **/
static inline void write_metadata_extent(struct vdo_extent *extent,
					 PhysicalBlockNumber start_block)
{
	write_partial_metadata_extent(extent, start_block, extent->count);
}

/**
 * Notify an extent that one of its vios has completed. If the signaling vio
 * is the last of the extent's vios to complete, the extent will finish. This
 * function is set as the vio callback in completeVIO().
 *
 * @param completion  The completion of the vio which has just finished
 **/
void handle_vio_completion(struct vdo_completion *completion);

#endif /* EXTENT_H */
