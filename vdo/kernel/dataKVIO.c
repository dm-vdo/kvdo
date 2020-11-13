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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.c#96 $
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

static void dump_pooled_data_kvio(void *data);

enum {
	WRITE_PROTECT_FREE_POOL = 0,
	WP_DATA_KVIO_SIZE =
		(sizeof(struct data_kvio) + PAGE_SIZE - 1 -
		((sizeof(struct data_kvio) + PAGE_SIZE - 1) % PAGE_SIZE)) };

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

/**********************************************************************/
static void maybe_log_data_kvio_trace(struct data_kvio *data_kvio)
{
	if (data_kvio->kvio.layer->trace_logging) {
		log_kvio_trace(&data_kvio->kvio);
	}
}

/**
 * First tracing hook for VIO completion.
 *
 * If the SystemTap script vdotrace.stp is in use, it does stage 1 of
 * its processing here. We must not call addTraceRecord between the
 * two tap functions.
 *
 * @param data_kvio  The VIO we're finishing up
 **/
static void kvio_completion_tap1(struct data_kvio *data_kvio)
{
	/*
	 * Ensure that data_kvio doesn't get optimized out, even under inline
	 * expansion. Also, make sure the compiler has to emit debug info
	 * for base_trace_location, which some of our SystemTap scripts will
	 * use here.
	 *
	 * First, make it look as though all memory could be clobbered; then
	 * require that a value be read into a register. That'll force at
	 * least one instruction to exist (so SystemTap can hook in) where
	 * data_kvio is live. We use a field that the caller would've
	 * accessed recently anyway, so it may be cached.
	 */
	barrier();
	__asm__ __volatile__(""
			     :
			     : "m"(data_kvio), "m"(base_trace_location),
			       "r"(data_kvio->kvio.layer));
}

/**
 * Second tracing hook for VIO completion.
 *
 * The SystemTap script vdotrace.stp splits its VIO-completion work
 * into two stages, to reduce lock contention for script variables.
 * Hence, it needs two hooks in the code.
 *
 * @param data_kvio  The VIO we're finishing up
 **/
static void kvio_completion_tap2(struct data_kvio *data_kvio)
{
	// Hack to ensure variable doesn't get optimized out.
	barrier();
	__asm__ __volatile__("" : : "m"(data_kvio), "r"(data_kvio->kvio.layer));
}

/**********************************************************************/
static void kvdo_acknowledge_data_kvio(struct data_kvio *data_kvio)
{
	struct kernel_layer *layer = data_kvio->kvio.layer;
	struct external_io_request *external_io_request =
		&data_kvio->external_io_request;
	struct bio *bio = external_io_request->bio;

	if (bio == NULL) {
		return;
	}

	external_io_request->bio = NULL;

	int error =
		map_to_system_error(data_vio_as_completion(&data_kvio->data_vio)->result);
	bio->bi_end_io = external_io_request->end_io;
	bio->bi_private = external_io_request->private;
	bio->bi_opf = external_io_request->rw;

	count_bios(&layer->biosAcknowledged, bio);
	if (data_kvio->is_partial) {
		count_bios(&layer->biosAcknowledgedPartial, bio);
	}


	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	complete_bio(bio, error);
}

/**********************************************************************/
static noinline void clean_data_kvio(struct data_kvio *data_kvio,
				     struct free_buffer_pointers *fbp)
{
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	kvdo_acknowledge_data_kvio(data_kvio);

	struct kvio *kvio = data_kvio_as_kvio(data_kvio);

	kvio->bio = NULL;

	if (unlikely(kvio->vio->trace != NULL)) {
		maybe_log_data_kvio_trace(data_kvio);
		kvio_completion_tap1(data_kvio);
		kvio_completion_tap2(data_kvio);
		free_trace_to_pool(kvio->layer, kvio->vio->trace);
	}

	add_free_buffer_pointer(fbp, data_kvio);
}

/**********************************************************************/
void return_data_kvio_batch_to_pool(struct batch_processor *batch,
				    void *closure)
{
	struct kernel_layer *layer = closure;
	uint32_t count = 0;

	ASSERT_LOG_ONLY(batch != NULL, "batch not null");
	ASSERT_LOG_ONLY(layer != NULL, "layer not null");

	struct free_buffer_pointers fbp;

	init_free_buffer_pointers(&fbp, layer->data_kvio_pool);

	struct kvdo_work_item *item;

	while ((item = next_batch_item(batch)) != NULL) {
		clean_data_kvio(work_item_as_data_kvio(item), &fbp);
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
kvdo_acknowledge_then_complete_data_kvio(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);

	kvdo_acknowledge_data_kvio(data_kvio);
	add_to_batch_processor(data_kvio->kvio.layer->data_kvio_releaser, item);
}

/**********************************************************************/
void kvdo_complete_data_kvio(struct vdo_completion *completion)
{
	struct data_kvio *data_kvio =
		data_vio_as_data_kvio(as_data_vio(completion));
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);

	if (use_bio_ack_queue(layer) && USE_BIO_ACK_QUEUE_FOR_READ &&
	    (data_kvio->external_io_request.bio != NULL)) {
		launch_data_kvio_on_bio_ack_queue(
			data_kvio,
			kvdo_acknowledge_then_complete_data_kvio,
			NULL,
			BIO_ACK_Q_ACTION_ACK);
	} else {
		add_to_batch_processor(layer->data_kvio_releaser,
				       work_item_from_data_kvio(data_kvio));
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
 * This is called from read_data_kvio_read_block_callback, registered only in
 * read_data_vio() and therefore never called on a 4k write.
 *
 * @param work_item  The data_kvio which requested the read
 **/
static void copy_read_block_data(struct kvdo_work_item *work_item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(work_item);

	// For a read-modify-write, copy the data into the data_block buffer so
	// it will be set up for the write phase.
	if (is_read_modify_write_vio(data_kvio->kvio.vio)) {
		memcpy(data_kvio->data_block, data_kvio->read_block.data,
		       VDO_BLOCK_SIZE);
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	// For a partial read, the callback will copy the requested data from
	// the read block.
	if (data_kvio->is_partial) {
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	// For a 4k read, copy the data to the user bio and acknowledge.
	bio_copy_data_out(data_kvio->external_io_request.bio,
			  data_kvio->read_block.data);
	acknowledge_data_vio(&data_kvio->data_vio);
}

/**
 * Finish reading data for a compressed block. This callback is registered
 * in read_data_vio() when trying to read compressed data for a 4k read or
 * a partial read or write.
 *
 * @param data_kvio  The data_kvio which requested the read
 **/
static void read_data_kvio_read_block_callback(struct data_kvio *data_kvio)
{
	if (data_kvio->read_block.status != VDO_SUCCESS) {
		set_completion_result(data_vio_as_completion(&data_kvio->data_vio),
				      data_kvio->read_block.status);
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	launch_data_kvio_on_cpu_queue(data_kvio, copy_read_block_data, NULL,
				      CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Uncompress the data that's just been read and then call back the requesting
 * data_kvio.
 *
 * @param work_item  The data_kvio requesting the data
 **/
static void uncompress_read_block(struct kvdo_work_item *work_item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(work_item);
	struct read_block *read_block = &data_kvio->read_block;

	// The data_kvio's scratch block will be used to contain the
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
		read_block->callback(data_kvio);
		return;
	}

	char *fragment = compressed_data + fragment_offset;
	int size =
		LZ4_decompress_safe(fragment,
				    data_kvio->scratch_block,
				    fragment_size,
				    VDO_BLOCK_SIZE);
	if (size == VDO_BLOCK_SIZE) {
		read_block->data = data_kvio->scratch_block;
	} else {
		log_debug("%s: lz4 error", __func__);
		read_block->status = VDO_INVALID_FRAGMENT;
	}

	read_block->callback(data_kvio);
}

/**
 * Now that we have gotten the data from storage, uncompress the data if
 * necessary and then call back the requesting data_kvio.
 *
 * @param data_kvio  The data_kvio requesting the data
 **/
static void complete_read(struct data_kvio *data_kvio)
{
	struct read_block *read_block = &data_kvio->read_block;

	read_block->status = blk_status_to_errno(data_kvio->bio->bi_status);

	if ((read_block->status == VDO_SUCCESS) &&
	    is_compressed(read_block->mapping_state)) {
		launch_data_kvio_on_cpu_queue(data_kvio,
					      uncompress_read_block,
					      NULL,
					      CPU_Q_ACTION_COMPRESS_BLOCK);
		return;
	}

	read_block->callback(data_kvio);
}

/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 */
static void read_bio_callback(struct bio *bio)
{
	struct kvio *kvio = (struct kvio *) bio->bi_private;
	struct data_kvio *data_kvio = kvio_as_data_kvio(kvio);

	data_kvio->read_block.data = data_kvio->read_block.buffer;
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	count_completed_bios(bio);
	complete_read(data_kvio);
}

/**********************************************************************/
void kvdo_read_block(struct data_vio *data_vio,
		     physical_block_number_t location,
		     BlockMappingState mapping_state,
		     bio_q_action action,
		     DataKVIOCallback callback)
{
	// This can be run on either a read of compressed data, or a write
	// trying to read-verify, so we can't assert about the operation.
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct kvio *kvio = data_kvio_as_kvio(data_kvio);
	struct read_block *read_block = &data_kvio->read_block;

	read_block->callback = callback;
	read_block->status = VDO_SUCCESS;
	read_block->mapping_state = mapping_state;

	// Read the data using the read block buffer.
	int result = reset_bio_with_buffer(data_kvio->bio, read_block->buffer,
					   kvio, read_bio_callback,
					   REQ_OP_READ, location);
	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}

	vdo_submit_bio(data_kvio->bio, action);
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
				read_data_kvio_read_block_callback);
		return;
	}

	struct kvio *kvio = data_vio_as_kvio(data_vio);
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct bio *bio = data_kvio->bio;

	// Read directly into the user buffer (for a 4k read) or the data
	// block (for a partial IO).
	int result = VDO_SUCCESS;
	if (is_read_modify_write_vio(data_vio_as_vio(data_vio))) {
		result = reset_bio_with_buffer(data_kvio->bio,
					       data_kvio->data_block, kvio,
					       complete_async_bio, REQ_OP_READ,
					       data_vio->mapped.pbn);
	} else if (data_kvio->is_partial) {
		// A partial read.
		int opf = data_kvio->external_io_request.bio->bi_opf & ~REQ_FUA;
		result = reset_bio_with_buffer(data_kvio->bio,
					       data_kvio->data_block, kvio,
					       complete_async_bio, opf,
					       data_vio->mapped.pbn);
	} else {
		/*
		 * A full 4k read. We reset, use __bio_clone_fast() to copy
		 * over the original bio iovec information and opflags, then
		 * edit what is essentially a copy of the user bio to fit our
		 * needs.
		 */
		bio_reset(bio);
		__bio_clone_fast(bio, data_kvio->external_io_request.bio);
		bio->bi_opf &= ~REQ_FUA;
		bio->bi_private = kvio;
		bio->bi_end_io = complete_async_bio;
		bio->bi_iter.bi_sector = block_to_sector(data_vio->mapped.pbn);
	}

	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
static void
kvdo_acknowledge_data_kvio_then_continue(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);

	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	kvdo_acknowledge_data_kvio(data_kvio);
	// Even if we're not using bio-ack threads, we may be in the wrong
	// base-code thread.
	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void acknowledge_data_vio(struct data_vio *data_vio)
{
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);

	// If the remaining discard work is not completely processed by this
	// VIO, don't acknowledge it yet.
	if ((data_kvio->external_io_request.bio != NULL) &&
	    (bio_op(data_kvio->external_io_request.bio) == REQ_OP_DISCARD) &&
	    (data_kvio->remaining_discard >
	     (VDO_BLOCK_SIZE - data_kvio->offset))) {
		invoke_callback(data_vio_as_completion(data_vio));
		return;
	}

	// We've finished with the kvio; acknowledge completion of the bio to
	// the kernel.
	if (use_bio_ack_queue(layer)) {
		data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
		launch_data_kvio_on_bio_ack_queue(
			data_kvio,
			kvdo_acknowledge_data_kvio_then_continue,
			NULL,
			BIO_ACK_Q_ACTION_ACK);
	} else {
		kvdo_acknowledge_data_kvio_then_continue(
			work_item_from_data_kvio(data_kvio));
	}
}

/**********************************************************************/
void write_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(is_write_vio(data_vio_as_vio(data_vio)),
			"kvdoWriteDataVIO() called on write data_vio");
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("$F;io=writeData;j=normal"));

	struct kvio *kvio = data_vio_as_kvio(data_vio);

	// Force the bio to be a write, while preserving the original bio's
	// flags (except FUA).
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);

	int bi_opf = (REQ_OP_WRITE |
		      ((data_kvio->external_io_request.rw & ~REQ_OP_MASK) &
		       ~REQ_FUA));

	// Write the data from the data block buffer.
	int result = reset_bio_with_buffer(data_kvio->bio, data_kvio->data_block,
					   kvio, complete_async_bio, bi_opf,
					   data_vio->new_mapped.pbn);
	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}

	vdo_submit_bio(data_kvio->bio, BIO_Q_ACTION_DATA);
}

/**
 * Determines whether the data block buffer is all zeros.
 *
 * @param data_kvio  The data_kvio to check
 *
 * @return true is all zeroes, false otherwise
 **/
static inline bool is_zero_block(struct data_kvio *data_kvio)
{
	const char *buffer = data_kvio->data_block;
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
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct bio *bio = data_kvio->external_io_request.bio;

	if (bio_op(bio) != REQ_OP_DISCARD) {
		bio_copy_data_in(bio, data_kvio->data_block + data_kvio->offset);
	} else {
		memset(data_kvio->data_block + data_kvio->offset, '\0',
		       min(data_kvio->remaining_discard,
			   (DiscardSize) (VDO_BLOCK_SIZE - data_kvio->offset)));
	}

	data_vio->is_zero_block = is_zero_block(data_kvio);
}

/**********************************************************************/
void zero_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!is_write_vio(data_vio_as_vio(data_vio)),
			"only attempt to zero non-writes");
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("zeroDataVIO;io=readData"));
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	if (data_kvio->is_partial) {
		memset(data_kvio->data_block, 0, VDO_BLOCK_SIZE);
	} else {
		zero_fill_bio(data_kvio->external_io_request.bio);
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
	struct data_kvio *dest = data_vio_as_data_kvio(destination);
	struct data_kvio *src = data_vio_as_data_kvio(source);
	if (dest->is_partial) {
		memcpy(dest->data_block, src->data_block, VDO_BLOCK_SIZE);
	} else {
		bio_copy_data_out(dest->external_io_request.bio,
				  src->data_block);
	}
}

/**********************************************************************/
static void kvdo_compress_work(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);

	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	char *context = get_work_queue_private_data();
	int size =
		LZ4_compress_default(data_kvio->data_block,
				     data_kvio->scratch_block,
				     VDO_BLOCK_SIZE,
				     VDO_BLOCK_SIZE,
				     context);
	struct data_vio *data_vio = &data_kvio->data_vio;

	if (size > 0) {
		// The scratch block will be used to contain the compressed
		// data.
		data_vio->compression.data = data_kvio->scratch_block;
		data_vio->compression.size = size;
	} else {
		// Use block size plus one as an indicator for uncompressible
		// data.
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
	}

	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void compress_data_vio(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio,
			          THIS_LOCATION("compressDataVIO;io=compress;cb=compress"));

	/*
	 * If the orignal bio was a discard, but we got this far because the
	 * discard was a partial one (r/m/w), and it is part of a larger
	 * discard, we cannot compress this VIO. We need to make sure the VIO
	 * completes ASAP.
	 */
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);

	if ((data_kvio->external_io_request.bio != NULL) &&
	    (bio_op(data_kvio->external_io_request.bio) == REQ_OP_DISCARD) &&
	    (data_kvio->remaining_discard > 0)) {
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	launch_data_kvio_on_cpu_queue(data_kvio, kvdo_compress_work,
				      NULL,
				      CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Construct a data_kvio.
 *
 * @param [in]  layer          The physical layer
 * @param [in]  bio            The bio to associate with this data_kvio
 * @param [out] data_kvio_ptr  A pointer to hold the new data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check make_data_kvio(struct kernel_layer *layer,
				       struct bio *bio,
				       struct data_kvio **data_kvio_ptr)
{
	struct data_kvio *data_kvio;
	int result = alloc_buffer_from_pool(layer->data_kvio_pool,
					    (void **) &data_kvio);
	if (result != VDO_SUCCESS) {
		return log_error_strerror(result,
					  "data kvio allocation failure");
	}

	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_kvio, WP_DATA_KVIO_SIZE, false);
	}

	struct kvio *kvio = &data_kvio->kvio;

	kvio->vio = data_vio_as_vio(&data_kvio->data_vio);
	memset(&data_kvio->dedupe_context.pending_list, 0,
	       sizeof(struct list_head));
	memset(&data_kvio->data_vio, 0, sizeof(struct data_vio));
	kvio->bio_to_submit = NULL;

	initialize_kvio(kvio,
			layer,
			VIO_TYPE_DATA,
			VIO_PRIORITY_DATA,
			NULL,
			bio);
	*data_kvio_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**
 * Creates a new data_vio structure. A data_vio represents a single logical
 * block of data. It is what most VDO operations work with. This function also
 * creates a wrapping data_kvio structure that is used when we want to
 * physically read or write the data associated with the struct data_vio.
 *
 * @param [in]  layer          The physical layer
 * @param [in]  bio            The bio from the request the new data_kvio will
 *                             service
 * @param [in]  arrival_time   The arrival time of the bio
 * @param [out] data_kvio_ptr  A pointer to hold the new data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int kvdo_create_kvio_from_bio(struct kernel_layer *layer,
				     struct bio *bio,
				     Jiffies arrival_time,
				     struct data_kvio **data_kvio_ptr)
{
	struct external_io_request external_io_request = {
		.bio = bio,
		.private = bio->bi_private,
		.end_io = bio->bi_end_io,
		.rw = bio->bi_opf,
	};

	struct data_kvio *data_kvio = NULL;
	int result = make_data_kvio(layer, bio, &data_kvio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	data_kvio->external_io_request = external_io_request;
	data_kvio->offset = sector_to_block_offset(bio->bi_iter.bi_sector);
	data_kvio->is_partial = ((bio->bi_iter.bi_size < VDO_BLOCK_SIZE) ||
			       (data_kvio->offset != 0));

	if (data_kvio->is_partial) {
		count_bios(&layer->biosInPartial, bio);
	} else {
		/*
		 * Note that we unconditionally fill in the data_block array for
		 * non-read operations. There are places like kvdoCopyVIO that
		 * may look at kvio->data_block for a zero block (and maybe for
		 * discards?). We could skip filling in data_block for such
		 * cases, but only once we're sure all such places are fixed to
		 * check the is_zero_block flag first.
		 */
		if (bio_op(bio) == REQ_OP_DISCARD) {
			/*
			 * This is a discard/trim operation. This is treated
			 * much like the zero block, but we keep differen
			 * stats and distinguish it in the block map.
			 */
			memset(data_kvio->data_block, 0, VDO_BLOCK_SIZE);
		} else if (bio_data_dir(bio) == WRITE) {
			// Copy the bio data to a char array so that we can
			// continue to use the data after we acknowledge the
			// bio.
			bio_copy_data_in(bio, data_kvio->data_block);
			data_kvio->data_vio.is_zero_block =
				is_zero_block(data_kvio);
		}
	}

	if (data_kvio->is_partial || (bio_data_dir(bio) == WRITE)) {
		data_kvio->read_block.data = data_kvio->data_block;
	}

	data_kvio_as_kvio(data_kvio)->bio = NULL;
	bio->bi_end_io = complete_async_bio;
	*data_kvio_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void launchDataKVIOWork(struct kvdo_work_item *item)
{
	run_callback(vio_as_completion(work_item_as_kvio(item)->vio));
}

/**
 * Continue discard processing for requests that span multiple physical blocks.
 * If all have been processed the kvio is completed.  If we have already seen
 * an error, we skip the rest of the discard and fail immediately.
 *
 * <p>Invoked in a request-queue thread after the discard of a block has
 * completed.
 *
 * @param completion  A completion representing the discard kvio
 **/
static void kvdo_continue_discard_kvio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);

	data_kvio->remaining_discard -=
		min(data_kvio->remaining_discard,
		    (DiscardSize) (VDO_BLOCK_SIZE - data_kvio->offset));
	if ((completion->result != VDO_SUCCESS) ||
	    (data_kvio->remaining_discard == 0)) {
		if (data_kvio->has_discard_permit) {
			limiter_release(&layer->discard_limiter);
			data_kvio->has_discard_permit = false;
		}
		kvdo_complete_data_kvio(completion);
		return;
	}

	data_kvio->is_partial = (data_kvio->remaining_discard < VDO_BLOCK_SIZE);
	data_kvio->offset = 0;

	vio_operation operation;

	if (data_kvio->is_partial) {
		operation = VIO_READ_MODIFY_WRITE;
	} else {
		operation = VIO_WRITE;
	}

	if (requestor_set_fua(data_kvio)) {
		operation |= VIO_FLUSH_AFTER;
	}

	prepare_data_vio(data_vio, data_vio->logical.lbn + 1, operation,
		         !data_kvio->is_partial, kvdo_continue_discard_kvio);
	enqueue_data_kvio(data_kvio, launchDataKVIOWork, completion->callback,
			  REQ_Q_ACTION_MAP_BIO);
}

/**
 * Finish a partial read.
 *
 * @param completion  The partial read kvio
 **/
static void kvdo_complete_partial_read(struct vdo_completion *completion)
{
	struct data_kvio *data_kvio =
		data_vio_as_data_kvio(as_data_vio(completion));
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	bio_copy_data_out(data_kvio->external_io_request.bio,
			  data_kvio->read_block.data + data_kvio->offset);
	kvdo_complete_data_kvio(completion);
	return;
}

/**********************************************************************/
int kvdo_launch_data_kvio_from_bio(struct kernel_layer *layer,
				   struct bio *bio,
				   uint64_t arrival_time,
				   bool has_discard_permit)
{

	struct data_kvio *data_kvio = NULL;
	int result = kvdo_create_kvio_from_bio(layer, bio, arrival_time, &data_kvio);

	if (unlikely(result != VDO_SUCCESS)) {
		log_info("%s: kvio allocation failure", __func__);
		if (has_discard_permit) {
			limiter_release(&layer->discard_limiter);
		}
		limiter_release(&layer->request_limiter);
		return map_to_system_error(result);
	}

	/*
	 * Discards behave very differently than other requests when coming
	 * in from device-mapper. We have to be able to handle any size discards
	 * and with various sector offsets within a block.
	 */
	struct kvio *kvio = &data_kvio->kvio;
	vdo_action *callback = kvdo_complete_data_kvio;
	vio_operation operation = VIO_WRITE;
	bool is_trim = false;

	if (bio_op(bio) == REQ_OP_DISCARD) {
		data_kvio->has_discard_permit = has_discard_permit;
		data_kvio->remaining_discard = bio->bi_iter.bi_size;
		callback = kvdo_continue_discard_kvio;
		if (data_kvio->is_partial) {
			operation = VIO_READ_MODIFY_WRITE;
		} else {
			is_trim = true;
		}
	} else if (data_kvio->is_partial) {
		if (bio_data_dir(bio) == READ) {
			callback = kvdo_complete_partial_read;
			operation = VIO_READ;
		} else {
			operation = VIO_READ_MODIFY_WRITE;
		}
	} else if (bio_data_dir(bio) == READ) {
		operation = VIO_READ;
	}

	if (requestor_set_fua(data_kvio)) {
		operation |= VIO_FLUSH_AFTER;
	}

	logical_block_number_t lbn =
		sector_to_block(bio->bi_iter.bi_sector -
				layer->starting_sector_offset);
	prepare_data_vio(&data_kvio->data_vio, lbn, operation, is_trim, callback);
	enqueue_kvio(kvio, launchDataKVIOWork,
		     vio_as_completion(kvio->vio)->callback,
		     REQ_Q_ACTION_MAP_BIO);
	return VDO_SUCCESS;
}

/**
 * Hash a data_kvio and set its chunk name.
 *
 * @param item  The data_kvio to be hashed
 **/
static void kvdo_hash_data_work(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);
	struct data_vio *data_vio = &data_kvio->data_vio;

	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));

	MurmurHash3_x64_128(data_kvio->data_block, VDO_BLOCK_SIZE, 0x62ea60be,
			    &data_vio->chunk_name);
	data_kvio->dedupe_context.chunk_name = &data_vio->chunk_name;

	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void hash_data_vio(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	launch_data_kvio_on_cpu_queue(data_vio_as_data_kvio(data_vio),
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

	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);

	if (has_allocation(data_vio)) {
		post_dedupe_advice(data_kvio);
	} else {
		// This block has not actually been written (presumably because
		// we are full), so attempt to dedupe without posting bogus
		// advice.
		query_dedupe_advice(data_kvio);
	}
}

/**********************************************************************/
void update_dedupe_index(struct data_vio *data_vio)
{
	update_dedupe_advice(data_vio_as_data_kvio(data_vio));
}

/**
 * Implements buffer_free_function.
 **/
static void free_pooled_data_kvio(void *data)
{
	if (data == NULL) {
		return;
	}

	struct data_kvio *data_kvio = (struct data_kvio *) data;

	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_kvio, WP_DATA_KVIO_SIZE, false);
	}

	if (data_kvio->bio != NULL) {
		free_bio(data_kvio->bio);
	}

	FREE(data_kvio->read_block.buffer);
	FREE(data_kvio->data_block);
	FREE(data_kvio->scratch_block);
	FREE(data_kvio);
}

/**
 * Allocate a data_kvio. This function is the internals of
 * makePooledDataKVIO().
 *
 * @param data_kvio_ptr  A pointer to hold the newly allocated data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_pooled_data_kvio(struct data_kvio **data_kvio_ptr)
{
	struct data_kvio *data_kvio;
	int result;

	if (WRITE_PROTECT_FREE_POOL) {
		STATIC_ASSERT(sizeof(struct data_kvio) <= WP_DATA_KVIO_SIZE);
		result = allocate_memory(WP_DATA_KVIO_SIZE, 0, __func__,
					 &data_kvio);
		if (result == VDO_SUCCESS) {
			BUG_ON((((size_t) data_kvio) & (PAGE_SIZE - 1)) != 0);
		}
	} else {
		result = ALLOCATE(1, struct data_kvio, __func__, &data_kvio);
	}

	if (result != VDO_SUCCESS) {
		return log_error_strerror(result,
					  "data_kvio allocation failure");
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = allocate_memory(VDO_BLOCK_SIZE, 0, "kvio data",
				 &data_kvio->data_block);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(data_kvio);
		return log_error_strerror(result,
					  "data_kvio data allocation failure");
	}

	result = create_bio(&data_kvio->bio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(data_kvio);
		return log_error_strerror(result,
					  "data_kvio data bio allocation failure");
	}

	result = allocate_memory(VDO_BLOCK_SIZE, 0, "kvio read buffer",
				 &data_kvio->read_block.buffer);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(data_kvio);
		return log_error_strerror(result,
					  "data_kvio read allocation failure");
	}

	result = allocate_memory(VDO_BLOCK_SIZE, 0, "kvio scratch",
				 &data_kvio->scratch_block);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(data_kvio);
		return log_error_strerror(result,
					  "data_kvio scratch allocation failure");
	}

	*data_kvio_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**
 * Implements buffer_allocate_function.
 **/
static int make_pooled_data_kvio(void **data_ptr)
{
	struct data_kvio *data_kvio = NULL;
	int result = allocate_pooled_data_kvio(&data_kvio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(data_kvio);
		return result;
	}

	*data_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**
 * Dump out the waiters on each struct data_vio in the struct data_vio buffer pool.
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

	log_info("      %s is locked. Waited on by: VIO %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
		 wait_on, data_vio, get_data_vio_allocation(data_vio),
		 data_vio->logical.lbn, data_vio->duplicate.pbn,
		 get_operation_name(data_vio));

	struct waiter *waiter;

	for (waiter = first->next_waiter; waiter != first;
	     waiter = waiter->next_waiter) {
		data_vio = waiter_as_data_vio(waiter);
		log_info("     ... and : VIO %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
			 data_vio, get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn, data_vio->duplicate.pbn,
			 get_operation_name(data_vio));
	}
}

/**
 * Encode various attributes of a VIO as a string of one-character flags for
 * dump logging. This encoding is for logging brevity:
 *
 * R => VIO completion result not VDO_SUCCESS
 * W => VIO is on a wait queue
 * D => VIO is a duplicate
 *
 * <p>The common case of no flags set will result in an empty, null-terminated
 * buffer. If any flags are encoded, the first character in the string will be
 * a space character.
 *
 * @param data_vio  The VIO to encode
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
 * Dump out info on a data_kvio from the data_kvio pool.
 *
 * <p>Implements buffer_dump_function.
 *
 * @param data  The data_kvio to dump
 **/
static void dump_pooled_data_kvio(void *data)
{
	struct data_kvio *data_kvio = (struct data_kvio *) data;
	struct data_vio *data_vio = &data_kvio->data_vio;

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
	dump_work_item_to_buffer(work_item_from_data_kvio(data_kvio),
				 vio_work_item_dump_buffer,
				 sizeof(vio_work_item_dump_buffer));
	// Another static buffer...
	// log10(256) = 2.408+, round up:
	enum { DECIMAL_DIGITS_PER_UINT64_T = (int) (1 + 2.41 * sizeof(uint64_t))
	};
	static char vio_block_number_dump_buffer[sizeof("P L D") +
						 3 *
						 DECIMAL_DIGITS_PER_UINT64_T];
	if (data_vio->is_duplicate) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu D%llu",
			 get_data_vio_allocation(data_vio), data_vio->logical.lbn,
			 data_vio->duplicate.pbn);
	} else if (has_allocation(data_vio)) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu",
			 get_data_vio_allocation(data_vio), data_vio->logical.lbn);
	} else {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer), "L%llu",
			 data_vio->logical.lbn);
	}

	static char vio_flush_generation_buffer[sizeof(" FG") +
						DECIMAL_DIGITS_PER_UINT64_T] =
		"";
	if (data_vio->flush_generation != 0) {
		snprintf(vio_flush_generation_buffer,
			 sizeof(vio_flush_generation_buffer), " FG%llu",
			 data_vio->flush_generation);
	}

	// Encode VIO attributes as a string of one-character flags, usually
	// empty.
	static char flags_dump_buffer[8];

	encode_vio_dump_flags(data_vio, flags_dump_buffer);

	log_info("  kvio %px %s%s %s %s%s", data_kvio,
		 vio_block_number_dump_buffer, vio_flush_generation_buffer,
		 get_operation_name(data_vio), vio_work_item_dump_buffer,
		 flags_dump_buffer);
	// might want info on: wantAlbireoAnswer / operation / status
	// might want info on: bio / bio_to_submit / bios_merged

	dump_vio_waiters(&data_vio->logical.waiters, "lbn");

	// might want to dump more info from VIO here
}

/**********************************************************************/
int make_data_kvio_buffer_pool(uint32_t pool_size,
			       struct buffer_pool **buffer_pool_ptr)
{
	return make_buffer_pool("data_kvio pool",
				pool_size,
				make_pooled_data_kvio,
				free_pooled_data_kvio,
				dump_pooled_data_kvio,
				buffer_pool_ptr);
}

/**********************************************************************/
struct data_location get_dedupe_advice(const struct dedupe_context *context)
{
	struct data_kvio *data_kvio = container_of(context,
						   struct data_kvio,
						   dedupe_context);
	return (struct data_location) {
		.state = data_kvio->data_vio.new_mapped.state,
		.pbn = data_kvio->data_vio.new_mapped.pbn,
	};
}

/**********************************************************************/
void set_dedupe_advice(struct dedupe_context *context,
		       const struct data_location *advice)
{
	struct data_kvio *data_kvio = container_of(context,
						   struct data_kvio,
						   dedupe_context);
	receive_dedupe_advice(&data_kvio->data_vio, advice);
}
