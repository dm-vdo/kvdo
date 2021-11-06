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

#include "vio-read.h"

#include <linux/bio.h>

#include "logger.h"

#include "block-map.h"
#include "data-vio.h"
#include "kernel-types.h"
#include "vdo.h"
#include "vio-write.h"

/**
 * Do the modify-write part of a read-modify-write cycle. This callback is
 * registered in read_block().
 *
 * @param completion  The data_vio which has just finished its read
 **/
static void modify_for_partial_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vio *vio = data_vio_as_vio(data_vio);

	assert_data_vio_in_logical_zone(data_vio);

	if (completion->result != VDO_SUCCESS) {
		complete_data_vio(completion);
		return;
	}

	vdo_apply_partial_write(data_vio);
	vio->operation = VIO_WRITE | (vio->operation & ~VIO_READ_WRITE_MASK);
	data_vio->is_partial_write = true;
	launch_write_data_vio(data_vio);
}

/**
 * Read a block asynchronously. This is the callback registered in
 * read_block_mapping().
 *
 * @param completion  The data_vio to read
 **/
static void read_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vio *vio = as_vio(completion);

	if (completion->result != VDO_SUCCESS) {
		complete_data_vio(completion);
		return;
	}

	completion->callback =
		(is_read_vio(vio) ? complete_data_vio
				  : modify_for_partial_write);

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		if (data_vio->is_partial) {
			memset(data_vio->data_block, 0, VDO_BLOCK_SIZE);
		} else {
			zero_fill_bio(data_vio->user_bio);
		}

		invoke_vdo_completion_callback(completion);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_READ_DATA_VIO;
	read_data_vio(data_vio);
}

/**
 * Read the data_vio's mapping from the block map. This callback is registered
 * in launch_read_data_vio().
 *
 * @param completion  The data_vio to be read
 **/
static void read_block_mapping(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if (completion->result != VDO_SUCCESS) {
		complete_data_vio(completion);
		return;
	}

	assert_data_vio_in_logical_zone(data_vio);
	set_data_vio_logical_callback(data_vio, read_block);
	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ;
	vdo_get_mapped_block(data_vio);
}

/**
 * Start the asynchronous processing of the data_vio for a read or
 * read-modify-write request which has acquired a lock on its logical block.
 * The first step is to perform a block map lookup.
 *
 * @param data_vio  The data_vio doing the read
 **/
void launch_read_data_vio(struct data_vio *data_vio)
{
	assert_data_vio_in_logical_zone(data_vio);
	data_vio->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
	/* Go find the block map slot for the LBN mapping. */
	vdo_find_block_map_slot(data_vio,
				read_block_mapping,
				get_vdo_logical_zone_thread_id(data_vio->logical.zone));
}

/**
 * Release the logical block lock which a read data_vio obtained now that it
 * is done.
 *
 * @param completion  The data_vio
 **/
static void release_logical_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	vdo_release_logical_block_lock(data_vio);
	vio_done_callback(completion);
}

/**
 * Clean up a data_vio which has finished processing a read.
 *
 * @param data_vio  The data_vio to clean up
 **/
void cleanup_read_data_vio(struct data_vio *data_vio)
{
	launch_data_vio_logical_callback(data_vio, release_logical_lock);
}
