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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/extent.c#17 $
 */

#include "extent.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "logger.h"
#include "physicalLayer.h"
#include "types.h"
#include "vdo.h"
#include "vioRead.h"
#include "vioWrite.h"

/**********************************************************************/
int create_extent(PhysicalLayer *layer, vio_type type,
		  vio_priority priority,
		  block_count_t block_count, char *data,
		  struct vdo_extent **extent_ptr)
{
	int result = ASSERT(is_metadata_vio_type(type),
			    "create_extent() called for metadata");
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct vdo_extent *extent;
	result = ALLOCATE_EXTENDED(struct vdo_extent, block_count, struct vio *,
				   __func__, &extent);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_completion(&extent->completion, VDO_EXTENT_COMPLETION,
			      layer);
	for (; extent->count < block_count; extent->count++) {
		result = layer->createMetadataVIO(layer, type, priority,
						  &extent->completion, data,
						  &extent->vios[extent->count]);
		if (result != VDO_SUCCESS) {
			free_extent(&extent);
			return result;
		}

		data += VDO_BLOCK_SIZE;
	}

	*extent_ptr = extent;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_extent(struct vdo_extent **extent_ptr)
{
	struct vdo_extent *extent = *extent_ptr;
	if (extent == NULL) {
		return;
	}

	block_count_t i;
	for (i = 0; i < extent->count; i++) {
		free_vio(&extent->vios[i]);
	}

	FREE(extent);
	*extent_ptr = NULL;
}

/**
 * Launch a metadata extent.
 *
 * @param extent       The extent
 * @param start_block  The absolute physical block at which the extent should
 *                     begin its I/O
 * @param count        The number of blocks to write
 * @param operation    The operation to perform on the extent
 **/
static void launch_metadata_extent(struct vdo_extent *extent,
				   physical_block_number_t start_block,
				   block_count_t count,
				   vio_operation operation)
{
	reset_completion(&extent->completion);
	if (count > extent->count) {
		finish_completion(&extent->completion, VDO_OUT_OF_RANGE);
		return;
	}

	extent->complete_count = extent->count - count;
	block_count_t i;
	for (i = 0; i < count; i++) {
		struct vio *vio = extent->vios[i];
		vio->completion.callback_thread_id =
			extent->completion.callback_thread_id;
		launch_metadata_vio(vio, start_block++, handle_vio_completion,
				    handle_vio_completion, operation);
	}
}

/**********************************************************************/
void read_partial_metadata_extent(struct vdo_extent *extent,
				  physical_block_number_t start_block,
				  block_count_t count)
{
	launch_metadata_extent(extent, start_block, count, VIO_READ);
}

/**********************************************************************/
void write_partial_metadata_extent(struct vdo_extent *extent,
				   physical_block_number_t start_block,
				   block_count_t count)
{
	launch_metadata_extent(extent, start_block, count, VIO_WRITE);
}

/**********************************************************************/
void handle_vio_completion(struct vdo_completion *completion)
{
	struct vdo_extent *extent = as_vdo_extent(completion->parent);
	if (++extent->complete_count != extent->count) {
		set_completion_result(extent_as_completion(extent),
				      completion->result);
		return;
	}

	finish_completion(extent_as_completion(extent), completion->result);
}
