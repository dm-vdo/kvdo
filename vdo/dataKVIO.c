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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.c#113 $
 */

#include "dataKVIO.h"

#include <asm/unaligned.h>
#include <linux/lz4.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "compressedBlock.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "physicalLayer.h"

#include "bio.h"
#include "dedupeIndex.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "ioSubmitter.h"
#include "vdoCommon.h"

static void dump_pooled_data_vio(void *data);

/**
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
 * Note that kernelLayer.c contains the complete list of flags we believe may
 * be set; the following list explains the action taken with each of those
 * flags VDO could receive:
 *
 * REQ_SYNC: Passed down if the user bio is not yet completed, since it
 * indicates the user bio completion is required for further work to be
 * done by the issuer.
 * REQ_META: Passed down if the user bio is not yet completed, since it may
 * mean the lower layer treats it as more urgent, similar to REQ_SYNC.
 * REQ_PRIO: Passed down if the user bio is not yet completed, since it
 * indicates the user bio is important.
 * REQ_NOMERGE: Set only if the incoming bio was split; irrelevant to VDO IO.
 * REQ_IDLE: Set if the incoming bio had more IO quickly following; VDO's IO
 * pattern doesn't match incoming IO, so this flag is incorrect for it.
 * REQ_FUA: Handled separately, and irrelevant to VDO IO otherwise.
 * REQ_RAHEAD: Passed down, as, for reads, it indicates trivial importance.
 * REQ_BACKGROUND: Not passed down, as VIOs are a limited resource and VDO
 * needs them recycled ASAP to service heavy load, which is the only place
 * where REQ_BACKGROUND might aid in load prioritization.
 **/
static unsigned int PASSTHROUGH_FLAGS =
	(REQ_PRIO | REQ_META | REQ_SYNC | REQ_RAHEAD);

enum {
	WRITE_PROTECT_FREE_POOL = 0,
	WP_DATA_VIO_SIZE =
		(sizeof(struct data_vio) + PAGE_SIZE - 1 -
		((sizeof(struct data_vio) + PAGE_SIZE - 1) % PAGE_SIZE)) };

/**
 * Alter the write-access permission to a page of memory, so that
 * objects in the free pool may no longer be modified.
 *
 * To do: Deny read access as well.
 *
 * @param address     The starting address to protect, which must be on a
 *                    page boundary
 * @param byte_count  The number of bytes to protect, which must be a multiple
 *                    of the page size
 * @param mode        The write protection mode (true means read-only)
 **/
static __always_inline void set_write_protect(void *address,
					      size_t byte_count,
					      bool mode __maybe_unused)
{
	BUG_ON((((long) address) % PAGE_SIZE) != 0);
	BUG_ON((byte_count % PAGE_SIZE) != 0);
	BUG(); // only works in internal code, sorry
}

/**
 * First tracing hook for data_vio completion.
 *
 * If the SystemTap script vdotrace.stp is in use, it does stage 1 of
 * its processing here. We must not call add_trace_record between the
 * two tap functions.
 *
 * @param data_vio  The data_vio we're finishing up
 **/
static void vio_completion_tap1(struct data_vio *data_vio)
{
	/*
	 * Ensure that data_vio doesn't get optimized out, even under inline
	 * expansion. Also, make sure the compiler has to emit debug info
	 * for base_trace_location, which some of our SystemTap scripts will
	 * use here.
	 *
	 * First, make it look as though all memory could be clobbered; then
	 * require that a value be read into a register. That'll force at
	 * least one instruction to exist (so SystemTap can hook in) where
	 * data_vio is live. We use a field that the caller would've
	 * accessed recently anyway, so it may be cached.
	 */
	barrier();
	__asm__ __volatile__(""
			     :
			     : "m"(data_vio), "m"(base_trace_location),
			       "r"(data_vio->allocating_vio.vio.completion.layer));
}

/**
 * Second tracing hook for data_vio completion.
 *
 * The SystemTap script vdotrace.stp splits its vio-completion work
 * into two stages, to reduce lock contention for script variables.
 * Hence, it needs two hooks in the code.
 *
 * @param data_vio  The data_vio we're finishing up
 **/
static void vio_completion_tap2(struct data_vio *data_vio)
{
	// Hack to ensure variable doesn't get optimized out.
	barrier();
	__asm__ __volatile__("" : : "m"(data_vio),
			     "r"(data_vio->allocating_vio.vio.completion.layer));
}

/**********************************************************************/
static void kvdo_acknowledge_data_vio(struct data_vio *data_vio)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);
	struct kernel_layer *layer = as_kernel_layer(completion->layer);
	struct external_io_request *external_io_request =
		&data_vio->external_io_request;
	struct bio *bio = external_io_request->bio;

	if (bio == NULL) {
		return;
	}

	external_io_request->bio = NULL;

	int error = map_to_system_error(completion->result);
	bio->bi_end_io = external_io_request->end_io;
	bio->bi_private = external_io_request->private;
	bio->bi_opf = external_io_request->rw;

	count_bios(&layer->bios_acknowledged, bio);
	if (data_vio->is_partial) {
		count_bios(&layer->bios_acknowledged_partial, bio);
	}


	vio_add_trace_record(as_vio(completion), THIS_LOCATION(NULL));
	complete_bio(bio, error);
}

/**********************************************************************/
static noinline void clean_data_vio(struct data_vio *data_vio,
				    struct free_buffer_pointers *fbp)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	vio_add_trace_record(vio, THIS_LOCATION(NULL));
	kvdo_acknowledge_data_vio(data_vio);
	vio->bio = NULL;

	if (unlikely(vio->trace != NULL)) {
		maybe_log_vio_trace(vio);
		vio_completion_tap1(data_vio);
		vio_completion_tap2(data_vio);
		free_trace_to_pool(as_kernel_layer(vio_as_completion(vio)->layer),
				   vio->trace);
	}

	add_free_buffer_pointer(fbp, data_vio);
}

/**********************************************************************/
void return_data_vio_batch_to_pool(struct batch_processor *batch,
				   void *closure)
{
	struct kernel_layer *layer = closure;
	uint32_t count = 0;

	ASSERT_LOG_ONLY(batch != NULL, "batch not null");
	ASSERT_LOG_ONLY(layer != NULL, "layer not null");

	struct free_buffer_pointers fbp;

	init_free_buffer_pointers(&fbp, layer->data_vio_pool);

	struct vdo_work_item *item;

	while ((item = next_batch_item(batch)) != NULL) {
		clean_data_vio(work_item_as_data_vio(item), &fbp);
		cond_resched_batch_processor(batch);
		count++;
	}

	if (fbp.index > 0) {
		free_buffer_pointers(&fbp);
	}

	complete_many_requests(layer, count);
}

/**********************************************************************/
static void
kvdo_acknowledge_then_complete_data_vio(struct vdo_work_item *item)
{
	struct data_vio *data_vio = work_item_as_data_vio(item);
	struct kernel_layer *layer
		= as_kernel_layer(container_of(item, struct vdo_completion,
					       work_item)->layer);
	kvdo_acknowledge_data_vio(data_vio);
	add_to_batch_processor(layer->data_vio_releaser, item);
}

/**********************************************************************/
void kvdo_complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	struct kernel_layer *layer = as_kernel_layer(completion->layer);

	if (use_bio_ack_queue(layer) && USE_BIO_ACK_QUEUE_FOR_READ &&
	    (data_vio->external_io_request.bio != NULL)) {
		launch_data_vio_on_bio_ack_queue(
			data_vio,
			kvdo_acknowledge_then_complete_data_vio,
			NULL,
			BIO_ACK_Q_ACTION_ACK);
	} else {
		add_to_batch_processor(layer->data_vio_releaser,
				       &completion->work_item);
	}
}

/**
 * For a read, dispatch the freshly uncompressed data to its destination:
 * - for a 4k read, copy it into the user bio for later acknowlegement;
 *
 * - for a partial read, invoke its callback; kvdo_complete_partial_read will
 *   copy the data into the user bio for acknowledgement;
 *
 * - for a partial write, copy it into the data block, so that we can later
 *   copy data from the user bio atop it in apply_partial_write and treat it as
 *   a full-block write.
 *
 * This is called from read_data_vio_read_block_callback, registered only in
 * read_data_vio() and therefore never called on a 4k write.
 *
 * @param work_item  The data_vio which requested the read
 **/
static void copy_read_block_data(struct vdo_work_item *work_item)
{
	struct data_vio *data_vio = work_item_as_data_vio(work_item);

	// For a read-modify-write, copy the data into the data_block buffer so
	// it will be set up for the write phase.
	if (is_read_modify_write_vio(data_vio_as_vio(data_vio))) {
		memcpy(data_vio->data_block, data_vio->read_block.data,
		       VDO_BLOCK_SIZE);
		enqueue_data_vio_callback(data_vio);
		return;
	}

	// For a partial read, the callback will copy the requested data from
	// the read block.
	if (data_vio->is_partial) {
		enqueue_data_vio_callback(data_vio);
		return;
	}

	// For a 4k read, copy the data to the user bio and acknowledge.
	bio_copy_data_out(data_vio->external_io_request.bio,
			  data_vio->read_block.data);
	acknowledge_data_vio(data_vio);
}

/**
 * Finish reading data for a compressed block. This callback is registered
 * in read_data_vio() when trying to read compressed data for a 4k read or
 * a partial read or write.
 *
 * @param completion  The data_vio which requested the read
 **/
static void
read_data_vio_read_block_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	if (data_vio->read_block.status != VDO_SUCCESS) {
		set_completion_result(completion,
				      data_vio->read_block.status);
		enqueue_data_vio_callback(data_vio);
		return;
	}

	launch_data_vio_on_cpu_queue(data_vio, copy_read_block_data, NULL,
				     CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Uncompress the data that's just been read and then call back the requesting
 * data_vio.
 *
 * @param work_item  The data_vio requesting the data
 **/
static void uncompress_read_block(struct vdo_work_item *work_item)
{
	struct vdo_completion *completion = container_of(work_item,
							 struct vdo_completion,
							 work_item);
	struct data_vio *data_vio = work_item_as_data_vio(work_item);
	struct read_block *read_block = &data_vio->read_block;

	// The data_vio's scratch block will be used to contain the
	// uncompressed data.
	uint16_t fragment_offset, fragment_size;
	char *compressed_data = read_block->data;
	int result = get_compressed_block_fragment(read_block->mapping_state,
						   compressed_data,
						   VDO_BLOCK_SIZE,
						   &fragment_offset,
						   &fragment_size);
	if (result != VDO_SUCCESS) {
		log_debug("%s: frag err %d", __func__, result);
		read_block->status = result;
		read_block->callback(completion);
		return;
	}

	char *fragment = compressed_data + fragment_offset;
	int size =
		LZ4_decompress_safe(fragment,
				    data_vio->scratch_block,
				    fragment_size,
				    VDO_BLOCK_SIZE);
	if (size == VDO_BLOCK_SIZE) {
		read_block->data = data_vio->scratch_block;
	} else {
		log_debug("%s: lz4 error", __func__);
		read_block->status = VDO_INVALID_FRAGMENT;
	}

	read_block->callback(completion);
}

/**
 * Now that we have gotten the data from storage, uncompress the data if
 * necessary and then call back the requesting data_vio.
 *
 * @param data_vio  The data_vio requesting the data
 **/
static void complete_read(struct data_vio *data_vio)
{
	struct read_block *read_block = &data_vio->read_block;

	read_block->status = blk_status_to_errno(data_vio->bio->bi_status);

	if ((read_block->status == VDO_SUCCESS) &&
	    is_compressed(read_block->mapping_state)) {
		launch_data_vio_on_cpu_queue(data_vio,
					     uncompress_read_block,
					     NULL,
					     CPU_Q_ACTION_COMPRESS_BLOCK);
		return;
	}

	read_block->callback(vio_as_completion(data_vio_as_vio(data_vio)));
}

/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 */
static void read_bio_callback(struct bio *bio)
{
	struct data_vio *data_vio = (struct data_vio *) bio->bi_private;
	data_vio->read_block.data = data_vio->read_block.buffer;
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	count_completed_bios(bio);
	complete_read(data_vio);
}

/**********************************************************************/
void kvdo_read_block(struct data_vio *data_vio,
		     physical_block_number_t location,
		     enum block_mapping_state mapping_state,
		     enum bio_q_action action,
		     vdo_action *callback)
{
	// This can be run on either a read of compressed data, or a write
	// trying to read-verify, so we can't assert about the operation.
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	struct read_block *read_block = &data_vio->read_block;

	read_block->callback = callback;
	read_block->status = VDO_SUCCESS;
	read_block->mapping_state = mapping_state;

	// Read the data using the read block buffer.
	struct vio *vio = data_vio_as_vio(data_vio);
	int result = reset_bio_with_buffer(data_vio->bio, read_block->buffer,
					   vio, read_bio_callback, REQ_OP_READ,
					   location);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(data_vio->bio, action);
}

/**********************************************************************/
static void acknowledge_user_bio(struct bio *bio)
{
	int error = get_bio_result(bio);
	struct vio *vio = (struct vio *) bio->bi_private;

	vio_add_trace_record(vio, THIS_LOCATION("$F($io);cb=io($io)"));
	count_completed_bios(bio);
	if (error == 0) {
		acknowledge_data_vio(vio_as_data_vio(vio));
		return;
	}

	continue_vio(vio, error);
}

/**********************************************************************/
void read_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!is_write_vio(data_vio_as_vio(data_vio)),
			"operation set correctly for data read");
	data_vio_add_trace_record(data_vio, THIS_LOCATION("$F;io=readData"));

	if (is_compressed(data_vio->mapped.state)) {
		kvdo_read_block(data_vio,
				data_vio->mapped.pbn,
				data_vio->mapped.state,
				BIO_Q_ACTION_COMPRESSED_DATA,
				read_data_vio_read_block_callback);
		return;
	}

	struct vio *vio = data_vio_as_vio(data_vio);
	struct bio *bio = data_vio->bio;

	// Read directly into the user buffer (for a 4k read) or the data
	// block (for a partial IO).
	int result = VDO_SUCCESS;
	int opf = (data_vio->external_io_request.bio->bi_opf &
		   PASSTHROUGH_FLAGS);
	if (is_read_modify_write_vio(data_vio_as_vio(data_vio))) {
		result = reset_bio_with_buffer(data_vio->bio,
					       data_vio->data_block, vio,
					       complete_async_bio,
					       REQ_OP_READ | opf,
					       data_vio->mapped.pbn);
	} else if (data_vio->is_partial) {
		// A partial read.
		result = reset_bio_with_buffer(data_vio->bio,
					       data_vio->data_block, vio,
					       complete_async_bio,
					       REQ_OP_READ | opf,
					       data_vio->mapped.pbn);
	} else {
		/*
		 * A full 4k read. We reset, use __bio_clone_fast() to copy
		 * over the original bio iovec information and opflags, then
		 * edit what is essentially a copy of the user bio to fit our
		 * needs.
		 */
		bio_reset(bio);
		__bio_clone_fast(bio, data_vio->external_io_request.bio);
		bio->bi_opf = REQ_OP_READ | opf;
		bio->bi_private = vio;
		bio->bi_end_io = acknowledge_user_bio;
		bio->bi_iter.bi_sector = block_to_sector(data_vio->mapped.pbn);
	}

	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
static void
kvdo_acknowledge_data_vio_then_continue(struct vdo_work_item *item)
{
	struct data_vio *data_vio = work_item_as_data_vio(item);

	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	kvdo_acknowledge_data_vio(data_vio);
	// Even if we're not using bio-ack threads, we may be in the wrong
	// base-code thread.
	enqueue_data_vio_callback(data_vio);
}

/**********************************************************************/
void acknowledge_data_vio(struct data_vio *data_vio)
{
	struct kernel_layer *layer =
		as_kernel_layer(vio_as_completion(data_vio_as_vio(data_vio))->layer);

	// If the remaining discard work is not completely processed by this
	// data_vio, don't acknowledge it yet.
	if ((data_vio->external_io_request.bio != NULL) &&
	    (bio_op(data_vio->external_io_request.bio) == REQ_OP_DISCARD) &&
	    (data_vio->remaining_discard >
	     (VDO_BLOCK_SIZE - data_vio->offset))) {
		invoke_callback(data_vio_as_completion(data_vio));
		return;
	}

	// We've finished with the vio; acknowledge completion of the bio to
	// the kernel.
	if (use_bio_ack_queue(layer)) {
		data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
		launch_data_vio_on_bio_ack_queue(
			data_vio,
			kvdo_acknowledge_data_vio_then_continue,
			NULL,
			BIO_ACK_Q_ACTION_ACK);
	} else {
		kvdo_acknowledge_data_vio_then_continue(
			work_item_from_data_vio(data_vio));
	}
}

/**********************************************************************/
void write_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(is_write_vio(data_vio_as_vio(data_vio)),
			"write_data_vio must be passed a write data_vio");
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("$F;io=writeData;j=normal"));

	struct vio *vio = data_vio_as_vio(data_vio);

	// In sync mode, we still have the original bio, and the hints are
	// appropriate for lower layers' fast completion of IO. In async,
	// we don't, and the hints don't matter anymore.
	unsigned int opf = 0;
	if (data_vio->external_io_request.bio != NULL) {
		opf = data_vio->external_io_request.rw & PASSTHROUGH_FLAGS;
	}

	// Write the data from the data block buffer.
	int result = reset_bio_with_buffer(data_vio->bio,
					   data_vio->data_block,
					   vio, complete_async_bio,
					   REQ_OP_WRITE | opf,
					   data_vio->new_mapped.pbn);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(data_vio->bio, BIO_Q_ACTION_DATA);
}

/**
 * Determines whether the data block buffer is all zeros.
 *
 * @param data_vio  The data_vio to check
 *
 * @return true is all zeroes, false otherwise
 **/
static inline bool is_zero_block(struct data_vio *data_vio)
{
	const char *buffer = data_vio->data_block;
	/*
	 * Handle expected common case of even the first word being nonzero,
	 * without getting into the more expensive (for one iteration) loop
	 * below.
	 */
	if (get_unaligned((u64 *) buffer) != 0) {
		return false;
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(uint64_t) == 0);
	unsigned int word_count = VDO_BLOCK_SIZE / sizeof(uint64_t);

	// Unroll to process 64 bytes at a time
	unsigned int chunk_count = word_count / 8;

	while (chunk_count-- > 0) {
		uint64_t word0 = get_unaligned((u64 *) buffer);
		uint64_t word1 =
			get_unaligned((u64 *) (buffer + 1 * sizeof(uint64_t)));
		uint64_t word2 =
			get_unaligned((u64 *) (buffer + 2 * sizeof(uint64_t)));
		uint64_t word3 =
			get_unaligned((u64 *) (buffer + 3 * sizeof(uint64_t)));
		uint64_t word4 =
			get_unaligned((u64 *) (buffer + 4 * sizeof(uint64_t)));
		uint64_t word5 =
			get_unaligned((u64 *) (buffer + 5 * sizeof(uint64_t)));
		uint64_t word6 =
			get_unaligned((u64 *) (buffer + 6 * sizeof(uint64_t)));
		uint64_t word7 =
			get_unaligned((u64 *) (buffer + 7 * sizeof(uint64_t)));
		uint64_t or = (word0 | word1 | word2 | word3 | word4 | word5 |
			       word6 | word7);
		// Prevent compiler from using 8*(cmp;jne).
		__asm__ __volatile__("" : : "g"(or));
		if (or != 0) {
			return false;
		}
		buffer += 8 * sizeof(uint64_t);
	}
	word_count %= 8;

	// Unroll to process 8 bytes at a time.
	// (Is this still worthwhile?)
	while (word_count-- > 0) {
		if (get_unaligned((u64 *) buffer) != 0) {
			return false;
		}
		buffer += sizeof(uint64_t);
	}
	return true;
}

/**********************************************************************/
void apply_partial_write(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	struct bio *bio = data_vio->external_io_request.bio;

	if (bio_op(bio) != REQ_OP_DISCARD) {
		bio_copy_data_in(bio, data_vio->data_block + data_vio->offset);
	} else {
		memset(data_vio->data_block + data_vio->offset, '\0',
		       min_t(uint32_t, data_vio->remaining_discard,
			     VDO_BLOCK_SIZE - data_vio->offset));

	}

	data_vio->is_zero_block = is_zero_block(data_vio);
}

/**********************************************************************/
void zero_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!is_write_vio(data_vio_as_vio(data_vio)),
			"only attempt to zero non-writes");
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("zeroDataVIO;io=readData"));
	if (data_vio->is_partial) {
		memset(data_vio->data_block, 0, VDO_BLOCK_SIZE);
	} else {
		zero_fill_bio(data_vio->external_io_request.bio);
	}
}

/**********************************************************************/
void copy_data(struct data_vio *source, struct data_vio *destination)
{
	ASSERT_LOG_ONLY(is_read_vio(data_vio_as_vio(destination)),
			"only copy to a pure read");
	ASSERT_LOG_ONLY(is_write_vio(data_vio_as_vio(source)),
			"only copy from a write");

	data_vio_add_trace_record(destination, THIS_LOCATION(NULL));
	if (destination->is_partial) {
		memcpy(destination->data_block, source->data_block,
		       VDO_BLOCK_SIZE);
	} else {
		bio_copy_data_out(destination->external_io_request.bio,
				  source->data_block);
	}
}

/**********************************************************************/
static void kvdo_compress_work(struct vdo_work_item *item)
{
	struct data_vio *data_vio = work_item_as_data_vio(item);

	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	char *context = get_work_queue_private_data();
	int size = LZ4_compress_default(data_vio->data_block,
					data_vio->scratch_block,
					VDO_BLOCK_SIZE,
					VDO_BLOCK_SIZE,
					context);
	if (size > 0) {
		// The scratch block will be used to contain the compressed
		// data.
		data_vio->compression.data = data_vio->scratch_block;
		data_vio->compression.size = size;
	} else {
		// Use block size plus one as an indicator for uncompressible
		// data.
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
	}

	enqueue_data_vio_callback(data_vio);
}

/**********************************************************************/
void compress_data_vio(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("compressDataVIO;io=compress;cb=compress"));

	/*
	 * If the orignal bio was a discard, but we got this far because the
	 * discard was a partial one (r/m/w), and it is part of a larger
	 * discard, we cannot compress this vio. We need to make sure the vio
	 * completes ASAP.
	 */
	if ((data_vio->external_io_request.bio != NULL) &&
	    (bio_op(data_vio->external_io_request.bio) == REQ_OP_DISCARD) &&
	    (data_vio->remaining_discard > 0)) {
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
		enqueue_data_vio_callback(data_vio);
		return;
	}

	launch_data_vio_on_cpu_queue(data_vio, kvdo_compress_work,
				      NULL,
				      CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Construct a data_vio.
 *
 * @param [in]  layer          The physical layer
 * @param [in]  bio            The bio to associate with this data_vio
 * @param [out] data_vio_ptr  A pointer to hold the new data_vio
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check make_data_vio(struct kernel_layer *layer,
				      struct bio *bio,
				      struct data_vio **data_vio_ptr)
{
	struct data_vio *data_vio;
	int result = alloc_buffer_from_pool(layer->data_vio_pool,
					    (void **) &data_vio);
	if (result != VDO_SUCCESS) {
		return log_error_strerror(result,
					  "data vio allocation failure");
	}

	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_vio, WP_DATA_VIO_SIZE, false);
	}

	// Zero out the fields which don't need to be preserved (i.e. which
	// are not pointers to separately allocated objects).
	memset(data_vio, 0, offsetof(struct data_vio, data_block));

	struct vio *vio = data_vio_as_vio(data_vio);
	vio->bio_to_submit = NULL;
	initialize_kvio(vio,
			layer,
			VIO_TYPE_DATA,
			VIO_PRIORITY_DATA,
			NULL,
			bio);
	*data_vio_ptr = data_vio;
	return VDO_SUCCESS;
}

/**
 * Creates a new data_vio structure. A data_vio represents a single logical
 * block of data. It is what most VDO operations work with. This function also
 * creates a wrapping data_vio structure that is used when we want to
 * physically read or write the data associated with the struct data_vio.
 *
 * @param [in]  layer            The physical layer
 * @param [in]  bio              The bio from the request the new data_vio
 *                               will service
 * @param [in]  arrival_jiffies  The arrival time of the bio
 * @param [out] data_vio_ptr  A pointer to hold the new data_vio
 *
 * @return VDO_SUCCESS or an error
 **/
static int kvdo_create_vio_from_bio(struct kernel_layer *layer,
				     struct bio *bio,
				     uint64_t arrival_jiffies,
				     struct data_vio **data_vio_ptr)
{
	struct external_io_request external_io_request = {
		.bio = bio,
		.private = bio->bi_private,
		.end_io = bio->bi_end_io,
		.rw = bio->bi_opf,
	};

	struct data_vio *data_vio = NULL;
	int result = make_data_vio(layer, bio, &data_vio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	data_vio->external_io_request = external_io_request;
	data_vio->offset = sector_to_block_offset(bio->bi_iter.bi_sector);
	data_vio->is_partial = ((bio->bi_iter.bi_size < VDO_BLOCK_SIZE) ||
				(data_vio->offset != 0));

	if (data_vio->is_partial) {
		count_bios(&layer->bios_in_partial, bio);
	} else {
		/*
		 * Note that we unconditionally fill in the data_block array
		 * for non-read operations. There are places like kvdo_copy_vio
		 * that may look at vio->data_block for a zero block (and maybe
		 * for discards?). We could skip filling in data_block for such
		 * cases, but only once we're sure all such places are fixed to
		 * check the is_zero_block flag first.
		 */
		if (bio_op(bio) == REQ_OP_DISCARD) {
			/*
			 * This is a discard/trim operation. This is treated
			 * much like the zero block, but we keep differen
			 * stats and distinguish it in the block map.
			 */
			memset(data_vio->data_block, 0, VDO_BLOCK_SIZE);
		} else if (bio_data_dir(bio) == WRITE) {
			// Copy the bio data to a char array so that we can
			// continue to use the data after we acknowledge the
			// bio.
			bio_copy_data_in(bio, data_vio->data_block);
			data_vio->is_zero_block = is_zero_block(data_vio);
		}
	}

	if (data_vio->is_partial || (bio_data_dir(bio) == WRITE)) {
		data_vio->read_block.data = data_vio->data_block;
	}

	data_vio_as_vio(data_vio)->bio = NULL;
	bio->bi_end_io = complete_async_bio;
	*data_vio_ptr = data_vio;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void launch_data_vio_work(struct vdo_work_item *item)
{
	run_callback(vio_as_completion(work_item_as_vio(item)));
}

/**
 * Continue discard processing for requests that span multiple physical blocks.
 * If all have been processed the vio is completed.  If we have already seen
 * an error, we skip the rest of the discard and fail immediately.
 *
 * <p>Invoked in a request-queue thread after the discard of a block has
 * completed.
 *
 * @param completion  A completion representing the discard vio
 **/
static void kvdo_continue_discard_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct kernel_layer *layer = as_kernel_layer(completion->layer);

	data_vio->remaining_discard -=
		min_t(uint32_t, data_vio->remaining_discard,
		      VDO_BLOCK_SIZE - data_vio->offset);
	if ((completion->result != VDO_SUCCESS) ||
	    (data_vio->remaining_discard == 0)) {
		if (data_vio->has_discard_permit) {
			limiter_release(&layer->discard_limiter);
			data_vio->has_discard_permit = false;
		}
		kvdo_complete_data_vio(completion);
		return;
	}

	data_vio->is_partial = (data_vio->remaining_discard < VDO_BLOCK_SIZE);
	data_vio->offset = 0;

	vio_operation operation;

	if (data_vio->is_partial) {
		operation = VIO_READ_MODIFY_WRITE;
	} else {
		operation = VIO_WRITE;
	}

	if (requestor_set_fua(data_vio)) {
		operation |= VIO_FLUSH_AFTER;
	}

	prepare_data_vio(data_vio, data_vio->logical.lbn + 1, operation,
		         !data_vio->is_partial, kvdo_continue_discard_vio);
	enqueue_vio(as_vio(completion), launch_data_vio_work,
		    completion->callback, REQ_Q_ACTION_MAP_BIO);
}

/**
 * Finish a partial read.
 *
 * @param completion  The partial read vio
 **/
static void kvdo_complete_partial_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	bio_copy_data_out(data_vio->external_io_request.bio,
			  data_vio->read_block.data + data_vio->offset);
	kvdo_complete_data_vio(completion);
	return;
}

/**********************************************************************/
int kvdo_launch_data_vio_from_bio(struct kernel_layer *layer,
				  struct bio *bio,
				  uint64_t arrival_jiffies,
				  bool has_discard_permit)
{

	struct data_vio *data_vio = NULL;
	int result = kvdo_create_vio_from_bio(layer, bio, arrival_jiffies,
					      &data_vio);
	if (unlikely(result != VDO_SUCCESS)) {
		log_info("%s: vio allocation failure", __func__);
		if (has_discard_permit) {
			limiter_release(&layer->discard_limiter);
		}
		limiter_release(&layer->request_limiter);
		return map_to_system_error(result);
	}

	/*
	 * Discards behave very differently than other requests when coming in
	 * from device-mapper. We have to be able to handle any size discards
	 * and with various sector offsets within a block.
	 */
	vdo_action *callback = kvdo_complete_data_vio;
	vio_operation operation = VIO_WRITE;
	bool is_trim = false;
	if (bio_op(bio) == REQ_OP_DISCARD) {
		data_vio->has_discard_permit = has_discard_permit;
		data_vio->remaining_discard = bio->bi_iter.bi_size;
		callback = kvdo_continue_discard_vio;
		if (data_vio->is_partial) {
			operation = VIO_READ_MODIFY_WRITE;
		} else {
			is_trim = true;
		}
	} else if (data_vio->is_partial) {
		if (bio_data_dir(bio) == READ) {
			callback = kvdo_complete_partial_read;
			operation = VIO_READ;
		} else {
			operation = VIO_READ_MODIFY_WRITE;
		}
	} else if (bio_data_dir(bio) == READ) {
		operation = VIO_READ;
	}

	if (requestor_set_fua(data_vio)) {
		operation |= VIO_FLUSH_AFTER;
	}

	logical_block_number_t lbn =
		sector_to_block(bio->bi_iter.bi_sector -
				layer->starting_sector_offset);
	prepare_data_vio(data_vio, lbn, operation, is_trim, callback);

	struct vio *vio = data_vio_as_vio(data_vio);
	enqueue_vio(vio, launch_data_vio_work,
		    vio_as_completion(vio)->callback, REQ_Q_ACTION_MAP_BIO);

	return VDO_SUCCESS;
}

/**
 * Hash a data_vio and set its chunk name.
 *
 * @param item  The data_vio to be hashed
 **/
static void kvdo_hash_data_work(struct vdo_work_item *item)
{
	struct data_vio *data_vio = work_item_as_data_vio(item);
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	MurmurHash3_x64_128(data_vio->data_block, VDO_BLOCK_SIZE, 0x62ea60be,
			    &data_vio->chunk_name);
	data_vio->dedupe_context.chunk_name = &data_vio->chunk_name;

	enqueue_data_vio_callback(data_vio);
}

/**********************************************************************/
void hash_data_vio(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	launch_data_vio_on_cpu_queue(data_vio,
				     kvdo_hash_data_work,
				     NULL,
				     CPU_Q_ACTION_HASH_BLOCK);
}

/**********************************************************************/
void check_for_duplication(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("checkForDuplication;dup=post"));
	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero block not checked for duplication");
	ASSERT_LOG_ONLY(data_vio->new_mapped.state != MAPPING_STATE_UNMAPPED,
			"discard not checked for duplication");

	if (has_allocation(data_vio)) {
		post_dedupe_advice(data_vio);
	} else {
		// This block has not actually been written (presumably because
		// we are full), so attempt to dedupe without posting bogus
		// advice.
		query_dedupe_advice(data_vio);
	}
}

/**********************************************************************/
void update_dedupe_index(struct data_vio *data_vio)
{
	update_dedupe_advice(data_vio);
}

/**
 * Implements buffer_free_function.
 **/
static void free_pooled_data_vio(void *data)
{
	if (data == NULL) {
		return;
	}

	struct data_vio *data_vio = (struct data_vio *) data;

	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_vio, WP_DATA_VIO_SIZE, false);
	}

	if (data_vio->bio != NULL) {
		free_bio(data_vio->bio);
	}

	FREE(data_vio->read_block.buffer);
	FREE(data_vio->data_block);
	FREE(data_vio->scratch_block);
	FREE(data_vio);
}

/**
 * Allocate a data_vio. This function is the internals of
 * make_pooled_data_vio().
 *
 * @param data_vio_ptr  A pointer to hold the newly allocated data_vio
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_pooled_data_vio(struct data_vio **data_vio_ptr)
{
	struct data_vio *data_vio;
	int result;

	if (WRITE_PROTECT_FREE_POOL) {
		STATIC_ASSERT(sizeof(struct data_vio) <= WP_DATA_VIO_SIZE);
		result = allocate_memory(WP_DATA_VIO_SIZE, 0, __func__,
					 &data_vio);
		if (result == VDO_SUCCESS) {
			BUG_ON((((size_t) data_vio) & (PAGE_SIZE - 1)) != 0);
		}
	} else {
		result = ALLOCATE(1, struct data_vio, __func__, &data_vio);
	}

	if (result != VDO_SUCCESS) {
		return log_error_strerror(result,
					  "data_vio allocation failure");
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = allocate_memory(VDO_BLOCK_SIZE, 0, "vio data",
				 &data_vio->data_block);
	if (result != VDO_SUCCESS) {
		free_pooled_data_vio(data_vio);
		return log_error_strerror(result,
					  "data_vio data allocation failure");
	}

	result = create_bio(&data_vio->bio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_vio(data_vio);
		return log_error_strerror(result,
					  "data_vio data bio allocation failure");
	}

	result = allocate_memory(VDO_BLOCK_SIZE, 0, "vio read buffer",
				 &data_vio->read_block.buffer);
	if (result != VDO_SUCCESS) {
		free_pooled_data_vio(data_vio);
		return log_error_strerror(result,
					  "data_vio read allocation failure");
	}

	result = allocate_memory(VDO_BLOCK_SIZE, 0, "vio scratch",
				 &data_vio->scratch_block);
	if (result != VDO_SUCCESS) {
		free_pooled_data_vio(data_vio);
		return log_error_strerror(result,
					  "data_vio scratch allocation failure");
	}

	*data_vio_ptr = data_vio;
	return VDO_SUCCESS;
}

/**
 * Implements buffer_allocate_function.
 **/
static int make_pooled_data_vio(void **data_ptr)
{
	struct data_vio *data_vio = NULL;
	int result = allocate_pooled_data_vio(&data_vio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_vio(data_vio);
		return result;
	}

	*data_ptr = data_vio;
	return VDO_SUCCESS;
}

/**
 * Dump out the waiters on each data_vio in the data_vio buffer pool.
 *
 * @param queue    The queue to check (logical or physical)
 * @param wait_on  The label to print for queue (logical or physical)
 **/
static void dump_vio_waiters(struct wait_queue *queue, char *wait_on)
{
	struct waiter *first = get_first_waiter(queue);

	if (first == NULL) {
		return;
	}

	struct data_vio *data_vio = waiter_as_data_vio(first);

	log_info("      %s is locked. Waited on by: vio %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
		 wait_on, data_vio, get_data_vio_allocation(data_vio),
		 data_vio->logical.lbn, data_vio->duplicate.pbn,
		 get_operation_name(data_vio));

	struct waiter *waiter;

	for (waiter = first->next_waiter; waiter != first;
	     waiter = waiter->next_waiter) {
		data_vio = waiter_as_data_vio(waiter);
		log_info("     ... and : vio %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
			 data_vio, get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn, data_vio->duplicate.pbn,
			 get_operation_name(data_vio));
	}
}

/**
 * Encode various attributes of a data_vio as a string of one-character flags
 * for dump logging. This encoding is for logging brevity:
 *
 * R => vio completion result not VDO_SUCCESS
 * W => vio is on a wait queue
 * D => vio is a duplicate
 *
 * <p>The common case of no flags set will result in an empty, null-terminated
 * buffer. If any flags are encoded, the first character in the string will be
 * a space character.
 *
 * @param data_vio  The vio to encode
 * @param buffer    The buffer to receive a null-terminated string of encoded
 *                  flag character
 **/
static void encode_vio_dump_flags(struct data_vio *data_vio, char buffer[8])
{
	char *p_flag = buffer;
	*p_flag++ = ' ';
	if (data_vio_as_completion(data_vio)->result != VDO_SUCCESS) {
		*p_flag++ = 'R';
	}
	if (data_vio_as_allocating_vio(data_vio)->waiter.next_waiter != NULL) {
		*p_flag++ = 'W';
	}
	if (data_vio->is_duplicate) {
		*p_flag++ = 'D';
	}
	if (p_flag == &buffer[1]) {
		// No flags, so remove the blank space.
		p_flag = buffer;
	}
	*p_flag = '\0';
}

/**
 * Dump out info on a data_vio from the data_vio pool.
 *
 * <p>Implements buffer_dump_function.
 *
 * @param data  The data_vio to dump
 **/
static void dump_pooled_data_vio(void *data)
{
	struct data_vio *data_vio = (struct data_vio *) data;

	/*
	 * This just needs to be big enough to hold a queue (thread) name
	 * and a function name (plus a separator character and NUL). The
	 * latter is limited only by taste.
	 *
	 * In making this static, we're assuming only one "dump" will run at
	 * a time. If more than one does run, the log output will be garbled
	 * anyway.
	 */
	static char vio_work_item_dump_buffer[100 + MAX_QUEUE_NAME_LEN];
	/*
	 * We're likely to be logging a couple thousand of these lines, and
	 * in some circumstances syslogd may have trouble keeping up, so
	 * keep it BRIEF rather than user-friendly.
	 */
	dump_work_item_to_buffer(work_item_from_data_vio(data_vio),
				 vio_work_item_dump_buffer,
				 sizeof(vio_work_item_dump_buffer));
	// Another static buffer...
	// log10(256) = 2.408+, round up:
	enum { DIGITS_PER_UINT64_T = (int) (1 + 2.41 * sizeof(uint64_t)) };
	static char vio_block_number_dump_buffer[sizeof("P L D")
						 + 3 * DIGITS_PER_UINT64_T];
	if (data_vio->is_duplicate) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu D%llu",
			 get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn,
			 data_vio->duplicate.pbn);
	} else if (has_allocation(data_vio)) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu",
			 get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn);
	} else {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer), "L%llu",
			 data_vio->logical.lbn);
	}

	static char vio_flush_generation_buffer[sizeof(" FG")
						+ DIGITS_PER_UINT64_T] = "";
	if (data_vio->flush_generation != 0) {
		snprintf(vio_flush_generation_buffer,
			 sizeof(vio_flush_generation_buffer), " FG%llu",
			 data_vio->flush_generation);
	}

	// Encode vio attributes as a string of one-character flags, usually
	// empty.
	static char flags_dump_buffer[8];
	encode_vio_dump_flags(data_vio, flags_dump_buffer);

	log_info("  vio %px %s%s %s %s%s", data_vio,
		 vio_block_number_dump_buffer, vio_flush_generation_buffer,
		 get_operation_name(data_vio), vio_work_item_dump_buffer,
		 flags_dump_buffer);
	// might want info on: wantAlbireoAnswer / operation / status
	// might want info on: bio / bio_to_submit / bios_merged

	dump_vio_waiters(&data_vio->logical.waiters, "lbn");

	// might want to dump more info from vio here
}

/**********************************************************************/
int make_data_vio_buffer_pool(uint32_t pool_size,
			      struct buffer_pool **buffer_pool_ptr)
{
	return make_buffer_pool("data_vio pool",
				pool_size,
				make_pooled_data_vio,
				free_pooled_data_vio,
				dump_pooled_data_vio,
				buffer_pool_ptr);
}

/**********************************************************************/
struct data_location get_dedupe_advice(const struct dedupe_context *context)
{
	struct data_vio *data_vio = container_of(context,
						 struct data_vio,
						 dedupe_context);
	return (struct data_location) {
		.state = data_vio->new_mapped.state,
		.pbn = data_vio->new_mapped.pbn,
	};
}

/**********************************************************************/
void set_dedupe_advice(struct dedupe_context *context,
		       const struct data_location *advice)
{
	receive_dedupe_advice(container_of(context,
					   struct data_vio,
					   dedupe_context),
			      advice);
}
