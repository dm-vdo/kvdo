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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/partitionCopy.c#18 $
 */

#include "partitionCopy.h"

#include "memoryAlloc.h"
#include "permassert.h"

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
	physical_block_number_t current_index;
	/** the last block to copy */
	physical_block_number_t ending_index;
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
static inline struct copy_completion * __must_check
as_copy_completion(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type,
				   VDO_PARTITION_COPY_COMPLETION);
	return container_of(completion, struct copy_completion, completion);
}

/**
 * Free a copy completion.
 *
 * @param copy  The copy completion to free
 **/
static void free_copy_completion(struct copy_completion *copy)
{
	free_vdo_extent(UDS_FORGET(copy->extent));
	UDS_FREE(copy->data);
	UDS_FREE(copy);
}

/**********************************************************************/
int make_vdo_copy_completion(struct vdo *vdo,
			     struct vdo_completion **completion_ptr)
{
	struct copy_completion *copy;
	int result = UDS_ALLOCATE(1, struct copy_completion, __func__, &copy);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&copy->completion, vdo,
				  VDO_PARTITION_COPY_COMPLETION);

	result = UDS_ALLOCATE((VDO_BLOCK_SIZE * STRIDE_LENGTH),
			      char,
			      "partition copy extent",
			      &copy->data);
	if (result != VDO_SUCCESS) {
		free_copy_completion(UDS_FORGET(copy));
		return result;
	}

	result = create_vdo_extent(vdo,
				   VIO_TYPE_PARTITION_COPY,
				   VIO_PRIORITY_HIGH,
				   STRIDE_LENGTH,
				   copy->data,
				   &copy->extent);
	if (result != VDO_SUCCESS) {
		free_copy_completion(copy);
		return result;
	}

	*completion_ptr = &copy->completion;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_copy_completion(struct vdo_completion *completion)
{
	if (completion == NULL) {
		return;
	}

	free_copy_completion(as_copy_completion(UDS_FORGET(completion)));
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
static inline block_count_t get_stride_size(struct copy_completion *copy)
{
	return min((block_count_t) STRIDE_LENGTH,
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
		finish_vdo_completion(completion->parent, VDO_SUCCESS);
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
static void complete_read_for_copy(struct vdo_completion *completion)
{
	struct copy_completion *copy = as_copy_completion(completion->parent);
	physical_block_number_t layer_start_block;
	int result = vdo_translate_to_pbn(copy->target, copy->current_index,
					  &layer_start_block);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(completion->parent, result);
		return;
	}

	completion->callback = complete_write_for_copy;
	write_partial_vdo_metadata_extent(vdo_completion_as_extent(completion),
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
	physical_block_number_t layer_start_block;
	int result = vdo_translate_to_pbn(copy->source, copy->current_index,
					  &layer_start_block);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(&copy->completion, result);
		return;
	}

	prepare_vdo_completion(&copy->extent->completion,
			       complete_read_for_copy,
			       finish_vdo_completion_parent_callback,
			       copy->completion.callback_thread_id,
			       &copy->completion);
	read_partial_vdo_metadata_extent(copy->extent, layer_start_block,
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
	block_count_t source_size = get_vdo_fixed_layout_partition_size(source);
	block_count_t target_size = get_vdo_fixed_layout_partition_size(target);

	physical_block_number_t source_start =
		get_vdo_fixed_layout_partition_offset(source);
	physical_block_number_t source_end = source_start + source_size;
	physical_block_number_t target_start =
		get_vdo_fixed_layout_partition_offset(target);
	physical_block_number_t target_end = target_start + target_size;

	int result = ASSERT(source_size <= target_size,
			    "target partition must be not smaller than source partition");
	if (result != UDS_SUCCESS) {
		return result;
	}

	return ASSERT(((source_end <= target_start) ||
		       (target_end <= source_start)),
		      "target partition must not overlap source partition");
}

/**********************************************************************/
void copy_vdo_partition(struct vdo_completion *completion,
			struct partition *source,
			struct partition *target,
			struct vdo_completion *parent)
{
	struct copy_completion *copy = as_copy_completion(completion);

	int result = validate_partition_copy(source, target);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	prepare_vdo_completion_to_finish_parent(&copy->completion, parent);
	copy->source = source;
	copy->target = target;
	copy->current_index = 0;
	copy->ending_index = get_vdo_fixed_layout_partition_size(source);
	copy_partition_stride(copy);
}
