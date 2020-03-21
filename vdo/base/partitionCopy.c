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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/partitionCopy.c#12 $
 */

#include "partitionCopy.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"
#include "numUtils.h"

enum {
	STRIDE_LENGTH = 2048
};

/**
 * A partition copy completion.
 **/
struct copy_completion {
	/** completion header */
	struct vdo_completion completion;
	/** the source partition to copy from */
	struct partition *source;
	/** the target partition to copy to */
	struct partition *target;
	/** the current in-partition PBN the copy is beginning at */
	PhysicalBlockNumber current_index;
	/** the last block to copy */
	PhysicalBlockNumber ending_index;
	/** the backing data used by the extent */
	char *data;
	/** the extent being used to copy */
	struct vdo_extent *extent;
};

/**
 * Convert a vdo_completion to a copy_completion.
 *
 * @param completion The completion to convert
 *
 * @return the completion as a copy_completion
 **/
__attribute__((warn_unused_result)) static inline struct copy_completion *
as_copy_completion(struct vdo_completion *completion)
{
	assert_completion_type(completion->type, PARTITION_COPY_COMPLETION);
	return container_of(completion, struct copy_completion, completion);
}

/**********************************************************************/
int make_copy_completion(PhysicalLayer *layer,
			 struct vdo_completion **completion_ptr)
{
	struct copy_completion *copy;
	int result = ALLOCATE(1, struct copy_completion, __func__, &copy);
	if (result != VDO_SUCCESS) {
		return result;
	}
	initialize_completion(&copy->completion, PARTITION_COPY_COMPLETION,
			      layer);

	result = ALLOCATE((VDO_BLOCK_SIZE * STRIDE_LENGTH),
			  char,
			  "partition copy extent",
			  &copy->data);
	if (result != VDO_SUCCESS) {
		struct vdo_completion *completion = &copy->completion;
		free_copy_completion(&completion);
		return result;
	}

	result = create_extent(layer,
			       VIO_TYPE_PARTITION_COPY,
			       VIO_PRIORITY_HIGH,
			       STRIDE_LENGTH,
			       copy->data,
			       &copy->extent);
	if (result != VDO_SUCCESS) {
		struct vdo_completion *completion = &copy->completion;
		free_copy_completion(&completion);
		return result;
	}

	*completion_ptr = &copy->completion;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_copy_completion(struct vdo_completion **completion_ptr)
{
	if (*completion_ptr == NULL) {
		return;
	}

	struct copy_completion *copy = as_copy_completion(*completion_ptr);
	free_extent(&copy->extent);
	FREE(copy->data);
	FREE(copy);
	*completion_ptr = NULL;
}

/**********************************************************************/
static void copy_partition_stride(struct copy_completion *copy);

/**
 * Determine the number of blocks to copy in the current stride.
 *
 * @param copy  The copy completion
 *
 * @return The number of blocks to copy in the current stride
 **/
static inline BlockCount get_stride_size(struct copy_completion *copy)
{
	return min_block_count(STRIDE_LENGTH,
			       copy->ending_index - copy->current_index);
}

/**
 * Process a completed write during a partition copy.
 *
 * @param completion  The extent which has just completed writing
 **/
static void complete_write_for_copy(struct vdo_completion *completion)
{
	struct copy_completion *copy = as_copy_completion(completion->parent);
	copy->current_index += get_stride_size(copy);
	if (copy->current_index >= copy->ending_index) {
		// We're done.
		finish_completion(completion->parent, VDO_SUCCESS);
		return;
	}
	copy_partition_stride(copy);
}

/**
 * Process a completed read during a partition copy, and launch the
 * corresponding write to the new partition.
 *
 * @param completion  The extent which has just completed reading
 **/
static void completeReadForCopy(struct vdo_completion *completion)
{
	struct copy_completion *copy = as_copy_completion(completion->parent);
	PhysicalBlockNumber layer_start_block;
	int result = translate_to_pbn(copy->target, copy->current_index,
				      &layer_start_block);
	if (result != VDO_SUCCESS) {
		finish_completion(completion->parent, result);
		return;
	}

	completion->callback = complete_write_for_copy;
	write_partial_metadata_extent(as_vdo_extent(completion),
				      layer_start_block,
				      get_stride_size(copy));
}

/**
 * Copy a stride from one partition to the new partition.
 *
 * @param copy  The copy_completion
 **/
static void copy_partition_stride(struct copy_completion *copy)
{
	PhysicalBlockNumber layer_start_block;
	int result = translate_to_pbn(copy->source, copy->current_index,
				      &layer_start_block);
	if (result != VDO_SUCCESS) {
		finish_completion(&copy->completion, result);
		return;
	}

	prepare_completion(&copy->extent->completion,
			   completeReadForCopy,
			   finish_parent_callback,
			   copy->completion.callbackThreadID,
			   &copy->completion);
	read_partial_metadata_extent(copy->extent, layer_start_block,
				     get_stride_size(copy));
}

/**
 * Verify that the source can be copied to the target safely.
 *
 * @param source        The source partition
 * @param target        The target partition
 *
 * @return VDO_SUCCESS or an error code
 **/
static int validate_partition_copy(struct partition *source,
				   struct partition *target)
{
	BlockCount sourceSize = get_fixed_layout_partition_size(source);
	BlockCount targetSize = get_fixed_layout_partition_size(target);

	PhysicalBlockNumber source_start =
		get_fixed_layout_partition_offset(source);
	PhysicalBlockNumber source_end = source_start + sourceSize;
	PhysicalBlockNumber target_start =
		get_fixed_layout_partition_offset(target);
	PhysicalBlockNumber target_end = target_start + targetSize;

	int result = ASSERT(sourceSize <= targetSize,
			    "target partition must be not smaller than source partition");
	if (result != UDS_SUCCESS) {
		return result;
	}

	return ASSERT(((source_end <= target_start) ||
		       (target_end <= source_start)),
		      "target partition must not overlap source partition");
}

/**********************************************************************/
void copy_partition_async(struct vdo_completion *completion,
			  struct partition *source,
			  struct partition *target,
			  struct vdo_completion *parent)
{
	int result = validate_partition_copy(source, target);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	struct copy_completion *copy = as_copy_completion(completion);
	prepare_to_finish_parent(&copy->completion, parent);
	copy->source = source;
	copy->target = target;
	copy->current_index = 0;
	copy->ending_index = get_fixed_layout_partition_size(source);
	copy_partition_stride(copy);
}
