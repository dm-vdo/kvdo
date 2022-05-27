// SPDX-License-Identifier: GPL-2.0-only
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
#include <linux/minmax.h>

#include "logger.h"

#include "bio.h"
#include "block-map.h"
#include "comparisons.h"
#include "data-vio.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "vdo.h"
#include "vio-write.h"

/**
 * DOC: Bio flags.
 *
 * For certain flags set on user bios, if the user bio has not yet been
 * acknowledged, setting those flags on our own bio(s) for that request may
 * help underlying layers better fulfill the user bio's needs. This constant
 * contains the aggregate of those flags; VDO strips all the other flags, as
 * they convey incorrect information.
 *
 * These flags are always irrelevant if we have already finished the user bio
 * as they are only hints on IO importance. If VDO has finished the user bio,
 * any remaining IO done doesn't care how important finishing the finished bio
 * was.
 *
 * Note that bio.c contains the complete list of flags we believe may be set;
 * the following list explains the action taken with each of those flags VDO
 * could receive:
 *
 * * REQ_SYNC: Passed down if the user bio is not yet completed, since it
 *   indicates the user bio completion is required for further work to be
 *   done by the issuer.
 * * REQ_META: Passed down if the user bio is not yet completed, since it may
 *   mean the lower layer treats it as more urgent, similar to REQ_SYNC.
 * * REQ_PRIO: Passed down if the user bio is not yet completed, since it
 *   indicates the user bio is important.
 * * REQ_NOMERGE: Set only if the incoming bio was split; irrelevant to VDO IO.
 * * REQ_IDLE: Set if the incoming bio had more IO quickly following; VDO's IO
 *   pattern doesn't match incoming IO, so this flag is incorrect for it.
 * * REQ_FUA: Handled separately, and irrelevant to VDO IO otherwise.
 * * REQ_RAHEAD: Passed down, as, for reads, it indicates trivial importance.
 * * REQ_BACKGROUND: Not passed down, as VIOs are a limited resource and VDO
 *   needs them recycled ASAP to service heavy load, which is the only place
 *   where REQ_BACKGROUND might aid in load prioritization.
 */
static unsigned int PASSTHROUGH_FLAGS =
	(REQ_PRIO | REQ_META | REQ_SYNC | REQ_RAHEAD);

static void continue_partial_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);

	launch_write_data_vio(data_vio);
}

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
	struct bio *bio = data_vio->user_bio;

	assert_data_vio_on_cpu_thread(data_vio);

	if (bio_op(bio) == REQ_OP_DISCARD) {
		memset(data_vio->data_block + data_vio->offset,
		       '\0',
		       min_t(uint32_t,
			     data_vio->remaining_discard,
			     VDO_BLOCK_SIZE - data_vio->offset));
	} else {
		vdo_bio_copy_data_in(bio,
				     data_vio->data_block + data_vio->offset);
	}

	data_vio->is_zero_block = is_zero_block(data_vio->data_block);
	vio->operation = VIO_WRITE | (vio->operation & ~VIO_READ_WRITE_MASK);
	data_vio->is_partial_write = true;
	completion->error_handler = NULL;
	launch_data_vio_logical_callback(data_vio, continue_partial_write);
}

static void complete_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	bool compressed = vdo_is_state_compressed(data_vio->mapped.state);

	assert_data_vio_on_cpu_thread(data_vio);

	if (compressed) {
		int result = uncompress_data_vio(data_vio,
						 data_vio->mapped.state,
						 data_vio->data_block);

		if (result != VDO_SUCCESS) {
			finish_data_vio(data_vio, result);
			return;
		}
	}

	if (is_read_modify_write_vio(data_vio_as_vio(data_vio))) {
		modify_for_partial_write(completion);
		return;
	}

	if (compressed || data_vio->is_partial) {
		vdo_bio_copy_data_out(data_vio->user_bio,
				      data_vio->data_block + data_vio->offset);
	}

	acknowledge_data_vio(data_vio);
	complete_data_vio(completion);
}

static void read_endio(struct bio *bio)
{
	struct data_vio *data_vio = vio_as_data_vio(bio->bi_private);

	vdo_count_completed_bios(bio);
	launch_data_vio_cpu_callback(data_vio,
				     complete_read,
				     CPU_Q_COMPLETE_READ_PRIORITY);
}

static void complete_zero_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);

	if (data_vio->is_partial) {
		memset(data_vio->data_block, 0, VDO_BLOCK_SIZE);
		if (!is_read_data_vio(data_vio)) {
			modify_for_partial_write(completion);
			return;
		}
	} else {
		zero_fill_bio(data_vio->user_bio);
	}

	complete_read(completion);
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
	int result = VDO_SUCCESS;

	if (completion->result != VDO_SUCCESS) {
		complete_data_vio(completion);
		return;
	}

	completion->error_handler = complete_data_vio;

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		launch_data_vio_cpu_callback(data_vio,
					     complete_zero_read,
					     CPU_Q_COMPLETE_VIO_PRIORITY);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_READ_DATA_VIO;
	completion->error_handler = complete_data_vio;
	if (vdo_is_state_compressed(data_vio->mapped.state)) {
		result = prepare_data_vio_for_io(data_vio,
						 (char *) data_vio->compression.block,
						 read_endio,
						 REQ_OP_READ,
						 data_vio->mapped.pbn);
	} else {
		int opf = ((data_vio->user_bio->bi_opf & PASSTHROUGH_FLAGS) |
			   REQ_OP_READ);

		if (is_read_modify_write_vio(data_vio_as_vio(data_vio)) ||
		    (data_vio->is_partial)) {
			result = prepare_data_vio_for_io(data_vio,
							 data_vio->data_block,
							 read_endio,
							 opf,
							 data_vio->mapped.pbn);
		} else {
			/*
			 * A full 4k read. Use the incoming bio to avoid having
			 * to copy the data
			 */
			set_vio_physical(vio, data_vio->mapped.pbn);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
			bio_reset(vio->bio);
			__bio_clone_fast(vio->bio, data_vio->user_bio);
#else
			bio_reset(vio->bio, vio->bio->bi_bdev, opf);
			bio_init_clone(vio->bio->bi_bdev,
				       vio->bio,
				       data_vio->user_bio,
				       GFP_KERNEL);
#endif

			/* Copy over the original bio iovec and opflags. */
			vdo_set_bio_properties(vio->bio,
					       vio,
					       read_endio,
					       opf,
					       data_vio->mapped.pbn);
		}
	}

	if (result != VDO_SUCCESS) {
		continue_data_vio(data_vio, result);
		return;
	}

	submit_data_vio_io(data_vio);
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

	/* Go find the block map slot for the LBN mapping. */
	vdo_find_block_map_slot(data_vio,
				read_block_mapping,
				data_vio->logical.zone->thread_id);
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
	release_data_vio(data_vio);
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
