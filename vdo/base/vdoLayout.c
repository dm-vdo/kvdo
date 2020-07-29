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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLayout.c#14 $
 */

#include "vdoLayout.h"
#include "vdoLayoutInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "partitionCopy.h"
#include "slab.h"
#include "slabSummary.h"
#include "types.h"
#include "vdoInternal.h"

#include "statusCodes.h"

static const partition_id REQUIRED_PARTITIONS[] = {
	BLOCK_MAP_PARTITION,
	BLOCK_ALLOCATOR_PARTITION,
	RECOVERY_JOURNAL_PARTITION,
	SLAB_SUMMARY_PARTITION,
};

static const uint8_t REQUIRED_PARTITION_COUNT = 4;

/**
 * Get the offset of a given partition.
 *
 * @param layout        The layout containing the partition
 * @param partition_id  The ID of the partition whose offset is desired
 *
 * @return The offset of the partition (in blocks)
 **/
__attribute__((warn_unused_result)) static block_count_t
get_partition_offset(struct vdo_layout *layout, partition_id partition_id)
{
	return get_fixed_layout_partition_offset(get_vdo_partition(layout,
								   partition_id));
}

/**********************************************************************/
int decode_vdo_layout(struct fixed_layout *layout,
		      struct vdo_layout **vdo_layout_ptr)
{
	// Check that all the expected partitions exist
	struct partition *partition;
	uint8_t i;
	int result;
	for (i = 0; i < REQUIRED_PARTITION_COUNT; i++) {
		result = get_partition(layout, REQUIRED_PARTITIONS[i],
				       &partition);
		if (result != VDO_SUCCESS) {
			return logErrorWithStringError(result,
						       "VDO layout is missing required partition %u",
						       REQUIRED_PARTITIONS[i]);
		}
	}

	struct vdo_layout *vdo_layout;
	result = ALLOCATE(1, struct vdo_layout, __func__, &vdo_layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_layout->layout = layout;

	// XXX Assert this is the same as where we loaded the super block.
	vdo_layout->starting_offset =
		get_partition_offset(vdo_layout, BLOCK_MAP_PARTITION);

	*vdo_layout_ptr = vdo_layout;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_layout(struct vdo_layout **vdo_layout_ptr)
{
	struct vdo_layout *vdo_layout = *vdo_layout_ptr;
	if (vdo_layout == NULL) {
		return;
	}

	free_copy_completion(&vdo_layout->copy_completion);
	free_fixed_layout(&vdo_layout->next_layout);
	free_fixed_layout(&vdo_layout->layout);
	free_fixed_layout(&vdo_layout->previous_layout);
	FREE(vdo_layout);
	*vdo_layout_ptr = NULL;
}

/**
 * Get a partition from a fixed_layout in conditions where we expect that it can
 * not fail.
 *
 * @param layout  The fixed_layout from which to get the partition
 * @param id      The ID of the partition to retrieve
 *
 * @return The desired partition
 **/
__attribute__((warn_unused_result)) static struct partition *
retrieve_partition(struct fixed_layout *layout, partition_id id)
{
	struct partition *partition;
	int result = get_partition(layout, id, &partition);
	ASSERT_LOG_ONLY(result == VDO_SUCCESS,
			"vdo_layout has expected partition");
	return partition;
}

/**********************************************************************/
struct partition *get_vdo_partition(struct vdo_layout *vdo_layout,
				    partition_id id)
{
	return retrieve_partition(vdo_layout->layout, id);
}

/**
 * Get a partition from a vdo_layout's next fixed_layout. This method should
 * only be called when the vdo_layout is prepared to grow.
 *
 * @param vdo_layout  The vdo_layout from which to get the partition
 * @param id          The ID of the desired partition
 *
 * @return The requested partition
 **/
__attribute__((warn_unused_result)) static struct partition *
get_partition_from_next_layout(struct vdo_layout *vdo_layout, partition_id id)
{
	ASSERT_LOG_ONLY(vdo_layout->next_layout != NULL,
			"vdo_layout is prepared to grow");
	return retrieve_partition(vdo_layout->next_layout, id);
}

/**
 * Get the size of a given partition.
 *
 * @param layout        The layout containing the partition
 * @param partition_id  The partition ID whose size to find
 *
 * @return The size of the partition (in blocks)
 **/
__attribute__((warn_unused_result)) static block_count_t
get_partition_size(struct vdo_layout *layout, partition_id partition_id)
{
	return get_fixed_layout_partition_size(get_vdo_partition(layout,
								 partition_id));
}

/**********************************************************************/
int prepare_to_grow_vdo_layout(struct vdo_layout *vdo_layout,
			       block_count_t old_physical_blocks,
			       block_count_t new_physical_blocks,
			       PhysicalLayer *layer)
{
	if (get_next_vdo_layout_size(vdo_layout) == new_physical_blocks) {
		// We are already prepared to grow to the new size, so we're
		// done.
		return VDO_SUCCESS;
	}

	// Make a copy completion if there isn't one
	if (vdo_layout->copy_completion == NULL) {
		int result =
			make_copy_completion(layer, &vdo_layout->copy_completion);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Free any unused preparation.
	free_fixed_layout(&vdo_layout->next_layout);

	// Make a new layout with the existing partition sizes for everything
	// but the block allocator partition.
	int result = make_vdo_fixed_layout(new_physical_blocks,
					   vdo_layout->starting_offset,
					   get_partition_size(vdo_layout, BLOCK_MAP_PARTITION),
					   get_partition_size(vdo_layout, RECOVERY_JOURNAL_PARTITION),
					   get_partition_size(vdo_layout, SLAB_SUMMARY_PARTITION),
					   &vdo_layout->next_layout);
	if (result != VDO_SUCCESS) {
		free_copy_completion(&vdo_layout->copy_completion);
		return result;
	}

	// Ensure the new journal and summary are entirely within the added
	// blocks.
	struct partition *slab_summary_partition =
		get_partition_from_next_layout(vdo_layout,
					       SLAB_SUMMARY_PARTITION);
	struct partition *recovery_journal_partition =
		get_partition_from_next_layout(vdo_layout,
					       RECOVERY_JOURNAL_PARTITION);
	block_count_t min_new_size =
		(old_physical_blocks +
		 get_fixed_layout_partition_size(slab_summary_partition) +
		 get_fixed_layout_partition_size(recovery_journal_partition));
	if (min_new_size > new_physical_blocks) {
		// Copying the journal and summary would destroy some old
		// metadata.
		free_fixed_layout(&vdo_layout->next_layout);
		free_copy_completion(&vdo_layout->copy_completion);
		return VDO_INCREMENT_TOO_SMALL;
	}

	return VDO_SUCCESS;
}

/**
 * Get the size of a VDO from the specified fixed_layout and the
 * starting offset thereof.
 *
 * @param layout           The fixed layout whose size to use
 * @param starting_offset  The starting offset of the layout
 *
 * @return The total size of a VDO (in blocks) with the given layout
 **/
__attribute__((warn_unused_result)) static block_count_t
get_vdo_size(struct fixed_layout *layout, block_count_t starting_offset)
{
	// The fixed_layout does not include the super block or any earlier
	// metadata; all that is captured in the vdo_layout's starting offset
	return get_total_fixed_layout_size(layout) + starting_offset;
}

/**********************************************************************/
block_count_t get_next_vdo_layout_size(struct vdo_layout *vdo_layout)
{
	return ((vdo_layout->next_layout == NULL) ?
			0 :
			get_vdo_size(vdo_layout->next_layout,
				     vdo_layout->starting_offset));
}

/**********************************************************************/
block_count_t
get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout)
{
	if (vdo_layout->next_layout == NULL) {
		return 0;
	}

	struct partition *partition =
		get_partition_from_next_layout(vdo_layout,
					       BLOCK_ALLOCATOR_PARTITION);
	return get_fixed_layout_partition_size(partition);
}

/**********************************************************************/
block_count_t grow_vdo_layout(struct vdo_layout *vdo_layout)
{
	ASSERT_LOG_ONLY(vdo_layout->next_layout != NULL,
			"VDO prepared to grow physical");
	vdo_layout->previous_layout = vdo_layout->layout;
	vdo_layout->layout = vdo_layout->next_layout;
	vdo_layout->next_layout = NULL;

	return get_vdo_size(vdo_layout->layout, vdo_layout->starting_offset);
}

/**********************************************************************/
block_count_t revert_vdo_layout(struct vdo_layout *vdo_layout)
{
	if ((vdo_layout->previous_layout != NULL) &&
	    (vdo_layout->previous_layout != vdo_layout->layout)) {
		// Only revert if there's something to revert to.
		free_fixed_layout(&vdo_layout->layout);
		vdo_layout->layout = vdo_layout->previous_layout;
		vdo_layout->previous_layout = NULL;
	}

	return get_vdo_size(vdo_layout->layout, vdo_layout->starting_offset);
}

/**********************************************************************/
void finish_vdo_layout_growth(struct vdo_layout *vdo_layout)
{
	if (vdo_layout->layout != vdo_layout->previous_layout) {
		free_fixed_layout(&vdo_layout->previous_layout);
	}

	if (vdo_layout->layout != vdo_layout->next_layout) {
		free_fixed_layout(&vdo_layout->next_layout);
	}

	free_copy_completion(&vdo_layout->copy_completion);
}

/**********************************************************************/
void copy_partition(struct vdo_layout *layout,
		    partition_id partition_id,
		    struct vdo_completion *parent)
{
	copy_partition_async(layout->copy_completion,
			     get_vdo_partition(layout, partition_id),
			     get_partition_from_next_layout(layout,
							    partition_id),
			     parent);
}

/**********************************************************************/
struct fixed_layout *get_layout(const struct vdo_layout *vdo_layout)
{
	return vdo_layout->layout;
}
