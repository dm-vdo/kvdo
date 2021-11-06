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

#include "extent.h"

#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "constants.h"
#include "logger.h"
#include "types.h"
#include "vdo.h"
#include "vio-read.h"
#include "vio-write.h"

/**
 * Create vdo_extent.
 *
 * @param [in]  vdo          The VDO
 * @param [in]  vio_type     The usage type to assign to the vios in the extent
 *                           (data / block map / journal)
 * @param [in]  priority     The relative priority to assign to the vios
 * @param [in]  block_count  The number of blocks in the buffer
 * @param [in]  data         The buffer
 * @param [out] extent_ptr   A pointer to hold the new extent
 *
 * @return VDO_SUCCESS or an error
 **/
int create_vdo_extent(struct vdo *vdo,
		      enum vio_type vio_type,
		      enum vio_priority priority,
		      block_count_t block_count,
		      char *data,
		      struct vdo_extent **extent_ptr)
{
	struct vdo_extent *extent;
	int result = ASSERT(is_vdo_metadata_vio_type(vio_type),
			    "create_vdo_extent() called for metadata");
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE_EXTENDED(struct vdo_extent, block_count,
				       struct vio *, __func__, &extent);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initialize_vdo_completion(&extent->completion, vdo,
				  VDO_EXTENT_COMPLETION);

	for (; extent->count < block_count; extent->count++) {
		result = create_metadata_vio(vdo,
					     vio_type,
					     priority,
					     &extent->completion,
					     data,
					     &extent->vios[extent->count]);
		if (result != VDO_SUCCESS) {
			free_vdo_extent(UDS_FORGET(extent));
			return result;
		}

		data += VDO_BLOCK_SIZE;
	}

	*extent_ptr = extent;
	return VDO_SUCCESS;
}

/**
 * Free an extent.
 *
 * @param extent  The extent to free
 **/
void free_vdo_extent(struct vdo_extent *extent)
{
	block_count_t i;

	if (extent == NULL) {
		return;
	}

	for (i = 0; i < extent->count; i++) {
		free_vio(UDS_FORGET(extent->vios[i]));
	}

	UDS_FREE(UDS_FORGET(extent));
}

/**
 * Notify an extent that one of its vios has completed. If the signaling vio
 * is the last of the extent's vios to complete, the extent will finish. This
 * function is set as the vio callback in launch_metadata_extent().
 *
 * @param completion  The completion of the vio which has just finished
 **/
static void handle_vio_completion(struct vdo_completion *completion)
{
	struct vdo_extent *extent = vdo_completion_as_extent(completion->parent);

	if (++extent->complete_count != extent->count) {
		set_vdo_completion_result(vdo_extent_as_completion(extent),
					  completion->result);
		return;
	}

	finish_vdo_completion(vdo_extent_as_completion(extent),
			      completion->result);
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
				   enum vio_operation operation)
{
	block_count_t i;

	reset_vdo_completion(&extent->completion);
	if (count > extent->count) {
		finish_vdo_completion(&extent->completion, VDO_OUT_OF_RANGE);
		return;
	}

	extent->complete_count = extent->count - count;
	for (i = 0; i < count; i++) {
		struct vio *vio = extent->vios[i];

		vio->completion.callback_thread_id =
			extent->completion.callback_thread_id;
		launch_metadata_vio(vio, start_block++, handle_vio_completion,
				    handle_vio_completion, operation);
	}
}

/**
 * Read metadata from the underlying storage.
 *
 * @param extent       The extent to read
 * @param start_block  The physical block number of the first block
 *                     in the extent
 * @param count        The number of blocks to read (must be less than or
 *                     equal to the length of the extent)
 **/
void read_partial_vdo_metadata_extent(struct vdo_extent *extent,
				      physical_block_number_t start_block,
				      block_count_t count)
{
	launch_metadata_extent(extent, start_block, count, VIO_READ);
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
void write_partial_vdo_metadata_extent(struct vdo_extent *extent,
				       physical_block_number_t start_block,
				       block_count_t count)
{
	launch_metadata_extent(extent, start_block, count, VIO_WRITE);
}
