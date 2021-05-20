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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLayout.c#26 $
 */

#include "vdoLayout.h"
#include "vdoLayoutInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "partitionCopy.h"
#include "slab.h"
#include "slabSummary.h"
#include "statusCodes.h"
#include "types.h"
#include "vdoInternal.h"

static const enum partition_id REQUIRED_PARTITIONS[] = {
	BLOCK_MAP_PARTITION,
	BLOCK_ALLOCATOR_PARTITION,
	RECOVERY_JOURNAL_PARTITION,
	SLAB_SUMMARY_PARTITION,
};

static const uint8_t REQUIRED_PARTITION_COUNT = 4;

/**
 * Get the offset of a given partition.
 *
 * @param layout  The layout containing the partition
 * @param id      The ID of the partition whose offset is desired
 *
 * @return The offset of the partition (in blocks)
 **/
static block_count_t __must_check
get_partition_offset(struct vdo_layout *layout, enum partition_id id)
{
	return get_fixed_layout_partition_offset(get_vdo_partition(layout,
								   id));
}

/**********************************************************************/
int decode_vdo_layout(struct fixed_layout *layout,
		      struct vdo_layout **vdo_layout_ptr)
{
	// Check that all the expected partitions exist
	struct vdo_layout *vdo_layout;
	struct partition *partition;
	uint8_t i;
	int result;
	for (i = 0; i < REQUIRED_PARTITION_COUNT; i++) {
		result = get_partition(layout, REQUIRED_PARTITIONS[i],
				       &partition);
		if (result != VDO_SUCCESS) {
			return log_error_strerror(result,
						  "VDO layout is missing required partition %u",
						  REQUIRED_PARTITIONS[i]);
		}
	}

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

	free_vdo_copy_completion(FORGET(vdo_layout->copy_completion));
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
static struct partition * __must_check
retrieve_partition(struct fixed_layout *layout, enum partition_id id)
{
	struct partition *partition;
	int result = get_partition(layout, id, &partition);
	ASSERT_LOG_ONLY(result == VDO_SUCCESS,
			"vdo_layout has expected partition");
	return partition;
}

/**********************************************************************/
struct partition *get_vdo_partition(struct vdo_layout *vdo_layout,
				    enum partition_id id)
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
static struct partition * __must_check
get_partition_from_next_layout(struct vdo_layout *vdo_layout,
			       enum partition_id id)
{
	ASSERT_LOG_ONLY(vdo_layout->next_layout != NULL,
			"vdo_layout is prepared to grow");
	return retrieve_partition(vdo_layout->next_layout, id);
}

/**
 * Get the size of a given partition.
 *
 * @param layout  The layout containing the partition
 * @param id      The partition ID whose size to find
 *
 * @return The size of the partition (in blocks)
 **/
static block_count_t __must_check
get_partition_size(struct vdo_layout *layout, enum partition_id id)
{
	return get_fixed_layout_partition_size(get_vdo_partition(layout, id));
}

/**********************************************************************/
int prepare_to_grow_vdo_layout(struct vdo_layout *vdo_layout,
			       block_count_t old_physical_blocks,
			       block_count_t new_physical_blocks,
			       struct vdo *vdo)
{
	int result;
	struct partition *slab_summary_partition, *recovery_journal_partition;
	block_count_t min_new_size;

	if (get_next_vdo_layout_size(vdo_layout) == new_physical_blocks) {
		// We are already prepared to grow to the new size, so we're
		// done.
		return VDO_SUCCESS;
	}

	// Make a copy completion if there isn't one
	if (vdo_layout->copy_completion == NULL) {
		int result =
			make_vdo_copy_completion(vdo,
						 &vdo_layout->copy_completion);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Free any unused preparation.
	free_fixed_layout(&vdo_layout->next_layout);

	// Make a new layout with the existing partition sizes for everything
	// but the block allocator partition.
	result = make_vdo_fixed_layout(new_physical_blocks,
				       vdo_layout->starting_offset,
				       get_partition_size(vdo_layout, BLOCK_MAP_PARTITION),
				       get_partition_size(vdo_layout, RECOVERY_JOURNAL_PARTITION),
				       get_partition_size(vdo_layout, SLAB_SUMMARY_PARTITION),
				       &vdo_layout->next_layout);
	if (result != VDO_SUCCESS) {
		free_vdo_copy_completion(FORGET(vdo_layout->copy_completion));
		return result;
	}

	// Ensure the new journal and summary are entirely within the added
	// blocks.
	slab_summary_partition =
		get_partition_from_next_layout(vdo_layout,
					       SLAB_SUMMARY_PARTITION);
	recovery_journal_partition =
		get_partition_from_next_layout(vdo_layout,
					       RECOVERY_JOURNAL_PARTITION);
	min_new_size =
		(old_physical_blocks +
		 get_fixed_layout_partition_size(slab_summary_partition) +
		 get_fixed_layout_partition_size(recovery_journal_partition));
	if (min_new_size > new_physical_blocks) {
		// Copying the journal and summary would destroy some old
		// metadata.
		free_fixed_layout(&vdo_layout->next_layout);
		free_vdo_copy_completion(FORGET(vdo_layout->copy_completion));
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
static block_count_t __must_check
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
vdo_get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout)
{
	struct partition *partition;
	if (vdo_layout->next_layout == NULL) {
		return 0;
	}

	partition = get_partition_from_next_layout(vdo_layout,
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
void finish_vdo_layout_growth(struct vdo_layout *vdo_layout)
{
	if (vdo_layout->layout != vdo_layout->previous_layout) {
		free_fixed_layout(&vdo_layout->previous_layout);
	}

	if (vdo_layout->layout != vdo_layout->next_layout) {
		free_fixed_layout(&vdo_layout->next_layout);
	}

	free_vdo_copy_completion(FORGET(vdo_layout->copy_completion));
}

/**********************************************************************/
void copy_vdo_layout_partition(struct vdo_layout *layout,
			       enum partition_id id,
			       struct vdo_completion *parent)
{
	copy_vdo_partition(layout->copy_completion,
			   get_vdo_partition(layout, id),
			   get_partition_from_next_layout(layout, id),
			   parent);
}

/**********************************************************************/
struct fixed_layout *get_layout(const struct vdo_layout *vdo_layout)
{
	return vdo_layout->layout;
}
