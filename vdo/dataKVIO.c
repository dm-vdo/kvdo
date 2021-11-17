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

#include "dataKVIO.h"

#include <asm/unaligned.h>
#include <linux/lz4.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "atomic-stats.h"
#include "comparisons.h"
#include "compressed-block.h"
#include "constants.h"
#include "data-vio.h"
#include "hash-lock.h"
#include "io-submitter.h"
#include "vdo.h"

#include "batchProcessor.h"
#include "bio.h"
#include "bufferPool.h"
#include "dedupeIndex.h"
#include "dump.h"
#include "kvio.h"

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

static const unsigned int VDO_SECTORS_PER_BLOCK_MASK =
	VDO_SECTORS_PER_BLOCK - 1;

/*
 * Prepare to return a data vio to the pool, by acknowledging the user bio
 * if it's not already, and then adding it to the batch to free.
 */
static noinline void clean_data_vio(struct data_vio *data_vio,
				    struct free_buffer_pointers *fbp)
{
	acknowledge_data_vio(data_vio);
	add_free_buffer_pointer(fbp, data_vio);
}

/*
 * Implements batch_processor_callback.
 * closure should be a pointer to this VDO.
 */
void return_data_vio_batch_to_pool(struct batch_processor *batch,
				   void *closure)
{
	struct free_buffer_pointers fbp;
	struct vdo_work_item *item;
	struct vdo *vdo = closure;
	uint32_t count = 0;

	ASSERT_LOG_ONLY(batch != NULL, "batch not null");
	ASSERT_LOG_ONLY(vdo != NULL, "vdo not null");


	init_free_buffer_pointers(&fbp, vdo->data_vio_pool);

	while ((item = next_batch_item(batch)) != NULL) {
		clean_data_vio(work_item_as_data_vio(item), &fbp);
		cond_resched_batch_processor(batch);
		count++;
	}

	if (fbp.index > 0) {
		free_buffer_pointers(&fbp);
	}

	/* Notify the limiter, so it can wake any blocked processes. */
	if (count > 0) {
		limiter_release_many(&vdo->request_limiter, count);
	}
}

static void vdo_complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vdo *vdo = vdo_get_from_data_vio(data_vio);

	add_to_batch_processor(vdo->data_vio_releaser, &completion->work_item);
}

/*
 * Initiate copying read data to its final buffer.
 * 
 * For a read, dispatch the freshly uncompressed data to its destination:
 * * for a 4k read, copy it into the user bio for later acknowlegement;
 * * for a partial read, invoke its callback; vdo_complete_partial_read will
 *   copy the data into the user bio for acknowledgement;
 * * for a partial write, copy it into the data block, so that we can later
 *   copy data from the user bio atop it in vdo_apply_partial_write and treat
 *   it as a full-block write.
 *
 * At present, this is called from read_data_vio_read_block_callback,
 * registered only in read_data_vio(). Therefore it is never called on a 4k
 * write, which it cannot handle.
 */
static void copy_read_block_data(struct vdo_work_item *work_item)
{
	struct data_vio *data_vio = work_item_as_data_vio(work_item);

	/*
	 * For a read-modify-write, copy the data into the data_block buffer so 
	 * it will be set up for the write phase. 
	 */
	if (is_read_modify_write_vio(data_vio_as_vio(data_vio))) {
		memcpy(data_vio->data_block, data_vio->read_block.data,
		       VDO_BLOCK_SIZE);
		enqueue_data_vio_callback(data_vio);
		return;
	}

	/*
	 * For a partial read, the callback will copy the requested data from 
	 * the read block. 
	 */
	if (data_vio->is_partial) {
		enqueue_data_vio_callback(data_vio);
		return;
	}

	/* For a 4k read, copy the data to the user bio and acknowledge. */
	vdo_bio_copy_data_out(data_vio->user_bio, data_vio->read_block.data);
	acknowledge_data_vio(data_vio);
	enqueue_data_vio_callback(data_vio);
}

/*
 * Finish reading data for a compressed block. This callback is registered
 * in read_data_vio() when trying to read compressed data for a 4k read or
 * a partial read or write.
 */
static void
read_data_vio_read_block_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if (data_vio->read_block.status != VDO_SUCCESS) {
		vdo_set_completion_result(completion,
					  data_vio->read_block.status);
		enqueue_data_vio_callback(data_vio);
		return;
	}

	launch_data_vio_on_cpu_queue(data_vio, copy_read_block_data,
				     CPU_Q_COMPRESS_BLOCK_PRIORITY);
}

/*
 * Uncompress the data that's just been read and then call back the requesting
 * data_vio.
 */
static void uncompress_read_block(struct vdo_work_item *work_item)
{
	struct vdo_completion *completion = container_of(work_item,
							 struct vdo_completion,
							 work_item);
	struct data_vio *data_vio = work_item_as_data_vio(work_item);
	struct read_block *read_block = &data_vio->read_block;
	int size;

	/*
	 * The data_vio's scratch block will be used to contain the 
	 * uncompressed data. 
	 */
	uint16_t fragment_offset, fragment_size;
	char *compressed_data = read_block->data;
	int result = vdo_get_compressed_block_fragment(read_block->mapping_state,
						       compressed_data,
						       VDO_BLOCK_SIZE,
						       &fragment_offset,
						       &fragment_size);
	if (result != VDO_SUCCESS) {
		uds_log_debug("%s: frag err %d", __func__, result);
		read_block->status = result;
		read_block->callback(completion);
		return;
	}

	size = LZ4_decompress_safe((compressed_data + fragment_offset),
				   data_vio->scratch_block, fragment_size,
				   VDO_BLOCK_SIZE);
	if (size == VDO_BLOCK_SIZE) {
		read_block->data = data_vio->scratch_block;
	} else {
		uds_log_debug("%s: lz4 error", __func__);
		read_block->status = VDO_INVALID_FRAGMENT;
	}

	read_block->callback(completion);
}

/*
 * Now that we have gotten the data from storage, uncompress the data if
 * necessary and then call back the requesting data_vio.
 */
static void complete_read(struct data_vio *data_vio)
{
	struct read_block *read_block = &data_vio->read_block;
	struct vio *vio = data_vio_as_vio(data_vio);

	read_block->status = blk_status_to_errno(vio->bio->bi_status);

	if ((read_block->status == VDO_SUCCESS) &&
	    vdo_is_state_compressed(read_block->mapping_state)) {
		launch_data_vio_on_cpu_queue(data_vio,
					     uncompress_read_block,
					     CPU_Q_COMPRESS_BLOCK_PRIORITY);
		return;
	}

	read_block->callback(vio_as_completion(vio));
}

static void read_bio_callback(struct bio *bio)
{
	struct data_vio *data_vio = (struct data_vio *) bio->bi_private;

	data_vio->read_block.data = data_vio->read_block.buffer;
	vdo_count_completed_bios(bio);
	complete_read(data_vio);
}

/**
 * Fetch the data for a block from storage.
 * The fetched data will be uncompressed when the callback is called, and the
 * result of the read operation will be stored in data_vio->read_block.status.
 * On success, the data will be in data_vio->read_block.data.
 */
void vdo_read_block(struct data_vio *data_vio,
		    physical_block_number_t location,
		    enum block_mapping_state mapping_state,
		    enum vdo_work_item_priority priority,
		    vdo_action *callback)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct read_block *read_block = &data_vio->read_block;
	int result;

	read_block->callback = callback;
	read_block->status = VDO_SUCCESS;
	read_block->mapping_state = mapping_state;

	result = prepare_data_vio_for_io(data_vio,
					 read_block->buffer,
					 read_bio_callback,
					 REQ_OP_READ,
					 location);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(vio->bio, priority);
}

static void acknowledge_user_bio(struct bio *bio)
{
	int error = vdo_get_bio_result(bio);
	struct vio *vio = (struct vio *) bio->bi_private;

	vdo_count_completed_bios(bio);
	if (error == 0) {
		acknowledge_data_vio(vio_as_data_vio(vio));
	}

	continue_vio(vio, error);
}

void read_data_vio(struct data_vio *data_vio)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct bio *bio = vio->bio;
	int result = VDO_SUCCESS;
	int opf;

	ASSERT_LOG_ONLY(!is_write_vio(vio),
			"operation set correctly for data read");

	if (vdo_is_state_compressed(data_vio->mapped.state)) {
		vdo_read_block(data_vio,
			       data_vio->mapped.pbn,
			       data_vio->mapped.state,
			       BIO_Q_COMPRESSED_DATA_PRIORITY,
			       read_data_vio_read_block_callback);
		return;
	}

	/*
	 * Read into the data block (for a RMW or partial IO) or directly into 
	 * the user buffer (for a 4k read). 
	 */
	opf = (data_vio->user_bio->bi_opf & PASSTHROUGH_FLAGS) | REQ_OP_READ;
	if (is_read_modify_write_vio(data_vio_as_vio(data_vio)) ||
	    (data_vio->is_partial)) {
		result = prepare_data_vio_for_io(data_vio,
						 data_vio->data_block,
						 vdo_complete_async_bio,
						 opf,
						 data_vio->mapped.pbn);
		if (result != VDO_SUCCESS) {
			continue_vio(vio, result);
			return;
		}
	} else {
		/*
		 * A full 4k read. Use the incoming bio to avoid having to 
		 * copy the data 
		 */
		set_vio_physical(vio, data_vio->mapped.pbn);
		bio_reset(bio);
		/* Copy over the original bio iovec and opflags. */
		__bio_clone_fast(bio, data_vio->user_bio);
		vdo_set_bio_properties(bio,
				       vio,
				       acknowledge_user_bio,
				       opf,
				       data_vio->mapped.pbn);
	}

	vdo_submit_bio(bio, BIO_Q_DATA_PRIORITY);
}

void vdo_apply_partial_write(struct data_vio *data_vio)
{
	struct bio *bio = data_vio->user_bio;

	if (bio_op(bio) != REQ_OP_DISCARD) {
		vdo_bio_copy_data_in(bio, data_vio->data_block + data_vio->offset);
	} else {
		memset(data_vio->data_block + data_vio->offset, '\0',
		       min_t(uint32_t, data_vio->remaining_discard,
			     VDO_BLOCK_SIZE - data_vio->offset));

	}

	data_vio->is_zero_block = is_zero_block(data_vio->data_block);
}

/*
 * Reset a data_vio which has just been acquired from the pool.
 */
static void reset_data_vio(struct data_vio *data_vio, struct vdo *vdo)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	/*
	 * FIXME We save the bio out of the vio so that we don't forget it. 
	 * Maybe we should just not zero that field somehow. 
	 */
	struct bio *bio = vio->bio;

	/*
	 * Zero out the fields which don't need to be preserved (i.e. which 
	 * are not pointers to separately allocated objects). 
	 */
	memset(data_vio, 0, offsetof(struct data_vio, dedupe_context));
	memset(&data_vio->dedupe_context.pending_list, 0,
	       sizeof(struct list_head));

	initialize_vio(vio,
		       bio,
		       VIO_TYPE_DATA,
		       VIO_PRIORITY_DATA,
		       NULL,
		       vdo,
		       NULL);
}

/*
 * Finish a sub-4k read IO.
 */
static void vdo_complete_partial_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	vdo_bio_copy_data_out(data_vio->user_bio,
			      data_vio->read_block.data + data_vio->offset);
	vdo_complete_data_vio(completion);
}

void launch_data_vio(struct vdo *vdo,
		     struct data_vio *data_vio,
		     struct bio *bio)
{
	vdo_action *callback = vdo_complete_data_vio;
	enum vio_operation operation = VIO_WRITE;
	logical_block_number_t lbn;

	reset_data_vio(data_vio, vdo);
	data_vio->user_bio = bio;
	data_vio->offset = to_bytes(bio->bi_iter.bi_sector
				    & VDO_SECTORS_PER_BLOCK_MASK);
	data_vio->is_partial = ((bio->bi_iter.bi_size < VDO_BLOCK_SIZE) ||
				(data_vio->offset != 0));

	/*
	 * Discards behave very differently than other requests when coming in
	 * from device-mapper. We have to be able to handle any size discards
	 * and with various sector offsets within a block.
	 */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		data_vio->remaining_discard = bio->bi_iter.bi_size;
		callback = vdo_complete_data_vio;
		if (data_vio->is_partial) {
			vdo_count_bios(&vdo->stats.bios_in_partial, bio);
			data_vio->read_block.data = data_vio->data_block;
			operation = VIO_READ_MODIFY_WRITE;
		}
	} else if (data_vio->is_partial) {
		vdo_count_bios(&vdo->stats.bios_in_partial, bio);
		data_vio->read_block.data = data_vio->data_block;
		if (bio_data_dir(bio) == READ) {
			callback = vdo_complete_partial_read;
			operation = VIO_READ;
		} else {
			operation = VIO_READ_MODIFY_WRITE;
		}
	} else if (bio_data_dir(bio) == READ) {
		operation = VIO_READ;
	} else {
		/*
		 * Copy the bio data to a char array so that we can 
		 * continue to use the data after we acknowledge the 
		 * bio.
		 */
		vdo_bio_copy_data_in(bio, data_vio->data_block);
		data_vio->is_zero_block = is_zero_block(data_vio->data_block);
		data_vio->read_block.data = data_vio->data_block;
	}

	if (data_vio->user_bio->bi_opf & REQ_FUA) {
		operation |= VIO_FLUSH_AFTER;
	}

	lbn = ((bio->bi_iter.bi_sector - vdo->starting_sector_offset)
	       / VDO_SECTORS_PER_BLOCK);
	prepare_data_vio(data_vio, lbn, operation, callback);
	vdo_invoke_completion_callback_with_priority(data_vio_as_completion(data_vio),
						     VDO_REQ_Q_MAP_BIO_PRIORITY);
}

/**********************************************************************/
void check_data_vio_for_duplication(struct data_vio *data_vio)
{

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero block not checked for duplication");
	ASSERT_LOG_ONLY(data_vio->new_mapped.state != VDO_MAPPING_STATE_UNMAPPED,
			"discard not checked for duplication");

	if (data_vio_has_allocation(data_vio)) {
		vdo_post_dedupe_advice(data_vio);
	} else {
		/*
		 * This block has not actually been written (presumably because 
		 * we are full), so attempt to dedupe without posting bogus 
		 * advice.
		 */
		vdo_query_dedupe_advice(data_vio);
	}
}

/**********************************************************************/
void vdo_update_dedupe_index(struct data_vio *data_vio)
{
	vdo_update_dedupe_advice(data_vio);
}

/*
 * Get the state needed to generate UDS metadata from the data_vio wrapping a
 * dedupe_context.
 *
 * FIXME: the name says nothing about data vios or dedupe_contexts or data
 * locations, just about VDOs, maybe too generic.
 */
struct data_location
vdo_get_dedupe_advice(const struct dedupe_context *context)
{
	struct data_vio *data_vio = container_of(context,
						 struct data_vio,
						 dedupe_context);
	return (struct data_location) {
		.state = data_vio->new_mapped.state,
		.pbn = data_vio->new_mapped.pbn,
	};
}

void vdo_set_dedupe_advice(struct dedupe_context *context,
			   const struct data_location *advice)
{
	receive_data_vio_dedupe_advice(container_of(context,
						    struct data_vio,
						    dedupe_context),
				       advice);
}
