/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.c#36 $
 */

#include "dataKVIO.h"

#include <asm/unaligned.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "compressedBlock.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "lz4.h"
#include "physicalLayer.h"

#include "bio.h"
#include "dedupeIndex.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "ioSubmitter.h"
#include "vdoCommon.h"

static void dumpPooledDataKVIO(void *poolData, void *data);

enum { WRITE_PROTECT_FREE_POOL = 0,
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
					      bool mode __attribute__((unused)))
{
	BUG_ON((((long)address) % PAGE_SIZE) != 0);
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
	 * for baseTraceLocation, which some of our SystemTap scripts will
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
			     : "m"(data_kvio), "m"(baseTraceLocation),
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
static void kvdoAcknowledgeDataKVIO(struct data_kvio *data_kvio)
{
	struct kernel_layer *layer = data_kvio->kvio.layer;
	struct external_io_request *externalIORequest =
		&data_kvio->externalIORequest;
	struct bio *bio = externalIORequest->bio;
	if (bio == NULL) {
		return;
	}

	externalIORequest->bio = NULL;

	int error =
		map_to_system_error(dataVIOAsCompletion(&data_kvio->dataVIO)->result);
	bio->bi_end_io = externalIORequest->endIO;
	bio->bi_private = externalIORequest->private;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
	bio->bi_opf = externalIORequest->rw;
#else
	bio->bi_rw = externalIORequest->rw;
#endif

	count_bios(&layer->biosAcknowledged, bio);
	if (data_kvio->isPartial) {
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
	kvdoAcknowledgeDataKVIO(data_kvio);

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
	kvdoAcknowledgeDataKVIO(data_kvio);
	add_to_batch_processor(data_kvio->kvio.layer->data_kvio_releaser, item);
}

/**********************************************************************/
void kvdoCompleteDataKVIO(VDOCompletion *completion)
{
	struct data_kvio *data_kvio =
		data_vio_as_data_kvio(asDataVIO(completion));
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);
	if (use_bio_ack_queue(layer) && USE_BIO_ACK_QUEUE_FOR_READ &&
	    (data_kvio->externalIORequest.bio != NULL)) {
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
 * Copy the uncompressed data from a compressed block read into the user
 * bio which requested the read.
 *
 * @param work_item  The data_kvio which requested the read
 **/
static void copy_read_block_data(struct kvdo_work_item *work_item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(work_item);

	// For a read-modify-write, copy the data into the dataBlock buffer so
	// it will be set up for the write phase.
	if (isReadModifyWriteVIO(data_kvio->kvio.vio)) {
		bio_copy_data_out(get_bio_from_data_kvio(data_kvio),
				  data_kvio->readBlock.data);
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	// For a partial read, the callback will copy the requested data from
	// the read block.
	if (data_kvio->isPartial) {
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	// For a full block read, copy the data to the bio and acknowledge.
	bio_copy_data_out(get_bio_from_data_kvio(data_kvio),
			  data_kvio->readBlock.data);
	acknowledgeDataVIO(&data_kvio->dataVIO);
}

/**
 * Finish reading data for a compressed block.
 *
 * @param data_kvio  The data_kvio which requested the read
 **/
static void read_data_kvio_read_block_callback(struct data_kvio *data_kvio)
{
	if (data_kvio->readBlock.status != VDO_SUCCESS) {
		setCompletionResult(dataVIOAsCompletion(&data_kvio->dataVIO),
				    data_kvio->readBlock.status);
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	launch_data_kvio_on_cpu_queue(data_kvio, copy_read_block_data, NULL,
				      CPU_Q_ACTION_COMPRESS_BLOCK);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
/**
 * Complete and reset a bio that was supplied by the user and then used for a
 * read (so that we can complete it with the user's callback).
 *
 * @param bio   The bio to complete
 **/
static void reset_user_bio(struct bio *bio)
#else
/**
 * Complete and reset a bio that was supplied by the user and then used for a
 * read (so that we can complete it with the user's callback).
 *
 * @param bio   The bio to complete
 * @param error Possible error from underlying block device
 **/
static void reset_user_bio(struct bio *bio, int error)
#endif
{
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)) && \
     (LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)))
	// This is a user bio, and the device just called bio_endio() on it, so
	// we need to re-increment bi_remaining so we too can call bio_endio().
	atomic_inc(&bio->bi_remaining);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	complete_async_bio(bio);
#else
	complete_async_bio(bio, error);
#endif
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
	struct read_block *read_block = &data_kvio->readBlock;
	BlockSize block_size = VDO_BLOCK_SIZE;

	// The data_kvio's scratch block will be used to contain the
	// uncompressed data.
	uint16_t fragment_offset, fragment_size;
	char *compressedData = read_block->data;
	int result = getCompressedBlockFragment(read_block->mappingState,
						compressedData,
						block_size,
						&fragment_offset,
						&fragment_size);
	if (result != VDO_SUCCESS) {
		logDebug("%s: frag err %d", __func__, result);
		read_block->status = result;
		read_block->callback(data_kvio);
		return;
	}

	char *fragment = compressedData + fragment_offset;
	int size =
		LZ4_uncompress_unknownOutputSize(fragment,
						 data_kvio->scratchBlock,
						 fragment_size,
						 block_size);
	if (size == block_size) {
		read_block->data = data_kvio->scratchBlock;
	} else {
		logDebug("%s: lz4 error", __func__);
		read_block->status = VDO_INVALID_FRAGMENT;
	}

	read_block->callback(data_kvio);
}

/**
 * Now that we have gotten the data from storage, uncompress the data if
 * necessary and then call back the requesting data_kvio.
 *
 * @param data_kvio  The data_kvio requesting the data
 * @param result     The result of the read operation
 **/
static void complete_read(struct data_kvio *data_kvio, int result)
{
	struct read_block *read_block = &data_kvio->readBlock;
	read_block->status = result;

	if ((result == VDO_SUCCESS) && isCompressed(read_block->mappingState)) {
		launch_data_kvio_on_cpu_queue(data_kvio,
					      uncompress_read_block,
					      NULL,
					      CPU_Q_ACTION_COMPRESS_BLOCK);
		return;
	}

	read_block->callback(data_kvio);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 */
static void read_bio_callback(struct bio *bio)
#else
/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 * @param result  The result of the read operation
 */
static void read_bio_callback(struct bio *bio, int result)
#endif
{
	struct kvio *kvio = (struct kvio *)bio->bi_private;
	struct data_kvio *data_kvio = kvio_as_data_kvio(kvio);
	data_kvio->readBlock.data = data_kvio->readBlock.buffer;
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	count_completed_bios(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	complete_read(data_kvio, get_bio_result(bio));
#else
	complete_read(data_kvio, result);
#endif
}

/**********************************************************************/
void kvdo_read_block(DataVIO *dataVIO,
		     PhysicalBlockNumber location,
		     BlockMappingState mappingState,
		     bio_q_action action,
		     DataKVIOCallback callback)
{
	dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));

	struct data_kvio *data_kvio = data_vio_as_data_kvio(dataVIO);
	struct read_block *read_block = &data_kvio->readBlock;
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);

	read_block->callback = callback;
	read_block->status = VDO_SUCCESS;
	read_block->mappingState = mappingState;

	BUG_ON(get_bio_from_data_kvio(data_kvio)->bi_private !=
	       &data_kvio->kvio);
	// Read the data directly from the device using the read bio.
	struct bio *bio = read_block->bio;
	reset_bio(bio, layer);
	set_bio_sector(bio, block_to_sector(layer, location));
	set_bio_operation_read(bio);
	bio->bi_end_io = read_bio_callback;
	vdo_submit_bio(bio, action);
}

/**********************************************************************/
void readDataVIO(DataVIO *dataVIO)
{
	ASSERT_LOG_ONLY(!isWriteVIO(dataVIOAsVIO(dataVIO)),
			"operation set correctly for data read");
	dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F;io=readData"));

	if (isCompressed(dataVIO->mapped.state)) {
		kvdo_read_block(dataVIO,
				dataVIO->mapped.pbn,
				dataVIO->mapped.state,
				BIO_Q_ACTION_COMPRESSED_DATA,
				read_data_kvio_read_block_callback);
		return;
	}

	struct kvio *kvio = data_vio_as_kvio(dataVIO);
	struct bio *bio = kvio->bio;
	bio->bi_end_io = reset_user_bio;
	set_bio_sector(bio, block_to_sector(kvio->layer, dataVIO->mapped.pbn));
	vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
static void
kvdo_acknowledge_data_kvio_then_continue(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));
	kvdoAcknowledgeDataKVIO(data_kvio);
	// Even if we're not using bio-ack threads, we may be in the wrong
	// base-code thread.
	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void acknowledgeDataVIO(DataVIO *dataVIO)
{
	struct data_kvio *data_kvio = data_vio_as_data_kvio(dataVIO);
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);

	// If the remaining discard work is not completely processed by this
	// VIO, don't acknowledge it yet.
	if (is_discard_bio(data_kvio->externalIORequest.bio) &&
	    (data_kvio->remainingDiscard >
	     (VDO_BLOCK_SIZE - data_kvio->offset))) {
		invokeCallback(dataVIOAsCompletion(dataVIO));
		return;
	}

	// We've finished with the kvio; acknowledge completion of the bio to
	// the kernel.
	if (use_bio_ack_queue(layer)) {
		dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
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
void writeDataVIO(DataVIO *data_vio)
{
	ASSERT_LOG_ONLY(isWriteVIO(dataVIOAsVIO(data_vio)),
			"kvdoWriteDataVIO() called on write DataVIO");
	dataVIOAddTraceRecord(data_vio,
			      THIS_LOCATION("$F;io=writeData;j=normal"));

	struct kvio *kvio = data_vio_as_kvio(data_vio);
	struct bio *bio = kvio->bio;
	set_bio_operation_write(bio);
	set_bio_sector(bio,
		       block_to_sector(kvio->layer, data_vio->newMapped.pbn));
	vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**
 * Determines whether the data block buffer is all zeros.
 *
 * @param data_kvio  The data_kvio to check
 *
 * @return true is all zeroes, false otherwise
 **/
static inline bool isZeroBlock(struct data_kvio *data_kvio)
{
	const char *buffer = data_kvio->dataBlock;
	/*
	 * Handle expected common case of even the first word being nonzero,
	 * without getting into the more expensive (for one iteration) loop
	 * below.
	 */
	if (get_unaligned((u64 *)buffer) != 0) {
		return false;
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(uint64_t) == 0);
	unsigned int wordCount = VDO_BLOCK_SIZE / sizeof(uint64_t);

	// Unroll to process 64 bytes at a time
	unsigned int chunkCount = wordCount / 8;
	while (chunkCount-- > 0) {
		uint64_t word0 = get_unaligned((u64 *)buffer);
		uint64_t word1 =
			get_unaligned((u64 *)(buffer + 1 * sizeof(uint64_t)));
		uint64_t word2 =
			get_unaligned((u64 *)(buffer + 2 * sizeof(uint64_t)));
		uint64_t word3 =
			get_unaligned((u64 *)(buffer + 3 * sizeof(uint64_t)));
		uint64_t word4 =
			get_unaligned((u64 *)(buffer + 4 * sizeof(uint64_t)));
		uint64_t word5 =
			get_unaligned((u64 *)(buffer + 5 * sizeof(uint64_t)));
		uint64_t word6 =
			get_unaligned((u64 *)(buffer + 6 * sizeof(uint64_t)));
		uint64_t word7 =
			get_unaligned((u64 *)(buffer + 7 * sizeof(uint64_t)));
		uint64_t or = (word0 | word1 | word2 | word3 | word4 | word5 |
			       word6 | word7);
		// Prevent compiler from using 8*(cmp;jne).
		__asm__ __volatile__("" : : "g"(or));
		if (or != 0) {
			return false;
		}
		buffer += 8 * sizeof(uint64_t);
	}
	wordCount %= 8;

	// Unroll to process 8 bytes at a time.
	// (Is this still worthwhile?)
	while (wordCount-- > 0) {
		if (get_unaligned((u64 *)buffer) != 0) {
			return false;
		}
		buffer += sizeof(uint64_t);
	}
	return true;
}

/**********************************************************************/
void applyPartialWrite(DataVIO *data_vio)
{
	dataVIOAddTraceRecord(data_vio, THIS_LOCATION(NULL));
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct bio *bio = data_kvio->externalIORequest.bio;
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);
	reset_bio(data_kvio->dataBlockBio, layer);

	if (!is_discard_bio(bio)) {
		bio_copy_data_in(bio, data_kvio->dataBlock + data_kvio->offset);
	} else {
		memset(data_kvio->dataBlock + data_kvio->offset, '\0',
		       min(data_kvio->remainingDiscard,
			   (DiscardSize)(VDO_BLOCK_SIZE - data_kvio->offset)));
	}

	data_vio->isZeroBlock = isZeroBlock(data_kvio);
	data_kvio->dataBlockBio->bi_private = &data_kvio->kvio;
	copy_bio_operation_and_flags(data_kvio->dataBlockBio, bio);
	// Make the bio a write, not (potentially) a discard.
	set_bio_operation_write(data_kvio->dataBlockBio);
}

/**********************************************************************/
void zeroDataVIO(DataVIO *data_vio)
{
	dataVIOAddTraceRecord(data_vio,
			      THIS_LOCATION("zeroDataVIO;io=readData"));
	bio_zero_data(data_vio_as_kvio(data_vio)->bio);
}

/**********************************************************************/
void copyData(DataVIO *source, DataVIO *destination)
{
	dataVIOAddTraceRecord(destination, THIS_LOCATION(NULL));
	bio_copy_data_out(data_vio_as_kvio(destination)->bio,
			  data_vio_as_data_kvio(source)->dataBlock);
}

/**********************************************************************/
static void kvdo_compress_work(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	char *context = get_work_queue_private_data();
	int size =
		LZ4_compress_ctx_limitedOutput(context, data_kvio->dataBlock,
					       data_kvio->scratchBlock,
					       VDO_BLOCK_SIZE, VDO_BLOCK_SIZE);
	DataVIO *data_vio = &data_kvio->dataVIO;
	if (size > 0) {
		// The scratch block will be used to contain the compressed
		// data.
		data_vio->compression.data = data_kvio->scratchBlock;
		data_vio->compression.size = size;
	} else {
		// Use block size plus one as an indicator for uncompressible
		// data.
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
	}

	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void compressDataVIO(DataVIO *data_vio)
{
	dataVIOAddTraceRecord(data_vio,
			      THIS_LOCATION("compressDataVIO;"
					    "io=compress;cb=compress"));

	/*
	 * If the orignal bio was a discard, but we got this far because the
	 * discard was a partial one (r/m/w), and it is part of a larger
	 * discard, we cannot compress this VIO. We need to make sure the VIO
	 * completes ASAP.
	 */
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	if (is_discard_bio(data_kvio->externalIORequest.bio) &&
	    (data_kvio->remainingDiscard > 0)) {
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
		kvdo_enqueue_data_vio_callback(data_kvio);
		return;
	}

	launch_data_kvio_on_cpu_queue(data_kvio, kvdo_compress_work, NULL,
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
__attribute__((warn_unused_result)) static int
makeDataKVIO(struct kernel_layer *layer,
	     struct bio *bio,
	     struct data_kvio **data_kvio_ptr)
{
	struct data_kvio *data_kvio;
	int result = alloc_buffer_from_pool(layer->data_kvio_pool,
					    (void **)&data_kvio);
	if (result != VDO_SUCCESS) {
		return logErrorWithStringError(result,
					       "data kvio allocation failure");
	}

	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_kvio, WP_DATA_KVIO_SIZE, false);
	}

	struct kvio *kvio = &data_kvio->kvio;
	kvio->vio = dataVIOAsVIO(&data_kvio->dataVIO);
	memset(&kvio->enqueueable, 0, sizeof(struct kvdo_enqueueable));
	memset(&data_kvio->dedupeContext.pendingList, 0,
	       sizeof(struct list_head));
	memset(&data_kvio->dataVIO, 0, sizeof(DataVIO));
	kvio->bio_to_submit = NULL;
	bio_list_init(&kvio->bios_merged);

	// The dataBlock is only needed for writes and some partial reads.
	if (is_write_bio(bio) || (get_bio_size(bio) < VDO_BLOCK_SIZE)) {
		reset_bio(data_kvio->dataBlockBio, layer);
	}

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
 * Creates a new DataVIO structure. A DataVIO represents a single logical
 * block of data. It is what most VDO operations work with. This function also
 * creates a wrapping data_kvio structure that is used when we want to
 * physically read or write the data associated with the DataVIO.
 *
 * @param [in]  layer          The physical layer
 * @param [in]  bio            The bio from the request the new data_kvio will
 *                             service
 * @param [in]  arrival_time   The arrival time of the bio
 * @param [out] data_kvio_ptr  A pointer to hold the new data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int kvdoCreateKVIOFromBio(struct kernel_layer *layer,
				 struct bio *bio,
				 Jiffies arrival_time,
				 struct data_kvio **data_kvio_ptr)
{
	struct external_io_request externalIORequest = {
		.bio = bio,
		.private = bio->bi_private,
		.endIO = bio->bi_end_io,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
		.rw = bio->bi_opf,
#else
		.rw = bio->bi_rw,
#endif
	};

	// We will handle FUA at the end of the request (after we restore the
	// bi_rw field from externalIORequest.rw).
	clear_bio_operation_flag_fua(bio);

	struct data_kvio *data_kvio = NULL;
	int result = makeDataKVIO(layer, bio, &data_kvio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	data_kvio->externalIORequest = externalIORequest;
	data_kvio->offset = sector_to_block_offset(layer, get_bio_sector(bio));
	data_kvio->isPartial = ((get_bio_size(bio) < VDO_BLOCK_SIZE) ||
			       (data_kvio->offset != 0));

	if (data_kvio->isPartial) {
		count_bios(&layer->biosInPartial, bio);
	} else {
		/*
		 * Note that we unconditionally fill in the dataBlock array for
		 * non-read operations. There are places like kvdoCopyVIO that
		 * may look at kvio->dataBlock for a zero block (and maybe for
		 * discards?). We could skip filling in dataBlock for such
		 * cases, but only once we're sure all such places are fixed to
		 * check the isZeroBlock flag first.
		 */
		if (is_discard_bio(bio)) {
			/*
			 * This is a discard/trim operation. This is treated
			 * much like the zero block, but we keep differen
			 * stats and distinguish it in the block map.
			 */
			memset(data_kvio->dataBlock, 0, VDO_BLOCK_SIZE);
		} else if (bio_data_dir(bio) == WRITE) {
			// Copy the bio data to a char array so that we can
			// continue to use the data after we acknowledge the
			// bio.
			bio_copy_data_in(bio, data_kvio->dataBlock);
			data_kvio->dataVIO.isZeroBlock = isZeroBlock(data_kvio);
		}
	}

	if (data_kvio->isPartial || is_write_bio(bio)) {
		/*
		 * data_kvio->bio will point at kvio->dataBlockBio for all writes
		 * and partial block I/O so the rest of the kernel code doesn't
		 * need to make a decision as to what to use.
		 */
		data_kvio->dataBlockBio->bi_private = &data_kvio->kvio;
		if (data_kvio->isPartial && is_write_bio(bio)) {
			clear_bio_operation_and_flags(data_kvio->dataBlockBio);
			set_bio_operation_read(data_kvio->dataBlockBio);
		} else {
			copy_bio_operation_and_flags(data_kvio->dataBlockBio,
						     bio);
		}
		data_kvio_as_kvio(data_kvio)->bio = data_kvio->dataBlockBio;
		data_kvio->readBlock.data = data_kvio->dataBlock;
	}

	set_bio_block_device(bio, get_kernel_layer_bdev(layer));
	bio->bi_end_io = complete_async_bio;
	*data_kvio_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void launchDataKVIOWork(struct kvdo_work_item *item)
{
	runCallback(vioAsCompletion(work_item_as_kvio(item)->vio));
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
static void kvdo_continue_discard_kvio(VDOCompletion *completion)
{
	DataVIO *data_vio = asDataVIO(completion);
	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	struct kernel_layer *layer = get_layer_from_data_kvio(data_kvio);
	data_kvio->remainingDiscard -=
		min(data_kvio->remainingDiscard,
		    (DiscardSize)(VDO_BLOCK_SIZE - data_kvio->offset));
	if ((completion->result != VDO_SUCCESS) ||
	    (data_kvio->remainingDiscard == 0)) {
		if (data_kvio->hasDiscardPermit) {
			limiter_release(&layer->discard_limiter);
			data_kvio->hasDiscardPermit = false;
		}
		kvdoCompleteDataKVIO(completion);
		return;
	}

	struct bio *bio = get_bio_from_data_kvio(data_kvio);
	reset_bio(bio, layer);
	data_kvio->isPartial = (data_kvio->remainingDiscard < VDO_BLOCK_SIZE);
	data_kvio->offset = 0;

	VIOOperation operation;
	if (data_kvio->isPartial) {
		operation = VIO_READ_MODIFY_WRITE;
		set_bio_operation_read(bio);
	} else {
		operation = VIO_WRITE;
	}

	if (requestor_set_fua(data_kvio)) {
		operation |= VIO_FLUSH_AFTER;
	}

	prepareDataVIO(data_vio, data_vio->logical.lbn + 1, operation,
		       !data_kvio->isPartial, kvdo_continue_discard_kvio);
	enqueue_data_kvio(data_kvio, launchDataKVIOWork, completion->callback,
			  REQ_Q_ACTION_MAP_BIO);
}

/**
 * Finish a partial read.
 *
 * @param completion  The partial read kvio
 **/
static void kvdoCompletePartialRead(VDOCompletion *completion)
{
	struct data_kvio *data_kvio =
		data_vio_as_data_kvio(asDataVIO(completion));
	data_kvio_add_trace_record(data_kvio, THIS_LOCATION(NULL));

	bio_copy_data_out(data_kvio->externalIORequest.bio,
			  data_kvio->readBlock.data + data_kvio->offset);
	kvdoCompleteDataKVIO(completion);
	return;
}

/**********************************************************************/
int kvdo_launch_data_kvio_from_bio(struct kernel_layer *layer,
				   struct bio *bio,
				   uint64_t arrival_time,
				   bool hasDiscardPermit)
{

	struct data_kvio *data_kvio = NULL;
	int result = kvdoCreateKVIOFromBio(layer, bio, arrival_time, &data_kvio);
	if (unlikely(result != VDO_SUCCESS)) {
		logInfo("%s: kvio allocation failure", __func__);
		if (hasDiscardPermit) {
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
	VDOAction *callback = kvdoCompleteDataKVIO;
	VIOOperation operation = VIO_WRITE;
	bool isTrim = false;
	if (is_discard_bio(bio)) {
		data_kvio->hasDiscardPermit = hasDiscardPermit;
		data_kvio->remainingDiscard = get_bio_size(bio);
		callback = kvdo_continue_discard_kvio;
		if (data_kvio->isPartial) {
			operation = VIO_READ_MODIFY_WRITE;
		} else {
			isTrim = true;
		}
	} else if (data_kvio->isPartial) {
		if (bio_data_dir(bio) == READ) {
			callback = kvdoCompletePartialRead;
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

	LogicalBlockNumber lbn = sector_to_block(layer,
		get_bio_sector(bio) - layer->starting_sector_offset);
	prepareDataVIO(&data_kvio->dataVIO, lbn, operation, isTrim, callback);
	enqueue_kvio(kvio, launchDataKVIOWork,
		     vioAsCompletion(kvio->vio)->callback,
		     REQ_Q_ACTION_MAP_BIO);
	return VDO_SUCCESS;
}

/**
 * Hash a data_kvio and set its chunk name.
 *
 * @param item  The data_kvio to be hashed
 **/
static void kvdoHashDataWork(struct kvdo_work_item *item)
{
	struct data_kvio *data_kvio = work_item_as_data_kvio(item);
	DataVIO *data_vio = &data_kvio->dataVIO;
	dataVIOAddTraceRecord(data_vio, THIS_LOCATION(NULL));

	MurmurHash3_x64_128(data_kvio->dataBlock, VDO_BLOCK_SIZE, 0x62ea60be,
			    &data_vio->chunkName);
	data_kvio->dedupeContext.chunkName = &data_vio->chunkName;

	kvdo_enqueue_data_vio_callback(data_kvio);
}

/**********************************************************************/
void hashDataVIO(DataVIO *data_vio)
{
	dataVIOAddTraceRecord(data_vio, THIS_LOCATION(NULL));
	launch_data_kvio_on_cpu_queue(data_vio_as_data_kvio(data_vio),
				      kvdoHashDataWork, NULL,
				      CPU_Q_ACTION_HASH_BLOCK);
}

/**********************************************************************/
void checkForDuplication(DataVIO *data_vio)
{
	dataVIOAddTraceRecord(data_vio,
			      THIS_LOCATION("checkForDuplication;dup=post"));
	ASSERT_LOG_ONLY(!data_vio->isZeroBlock,
			"zero block not checked for duplication");
	ASSERT_LOG_ONLY(data_vio->newMapped.state != MAPPING_STATE_UNMAPPED,
			"discard not checked for duplication");

	struct data_kvio *data_kvio = data_vio_as_data_kvio(data_vio);
	if (hasAllocation(data_vio)) {
		post_dedupe_advice(data_kvio);
	} else {
		// This block has not actually been written (presumably because
		// we are full), so attempt to dedupe without posting bogus
		// advice.
		query_dedupe_advice(data_kvio);
	}
}

/**********************************************************************/
void updateDedupeIndex(DataVIO *data_vio)
{
	update_dedupe_advice(data_vio_as_data_kvio(data_vio));
}

/**
 * Implements buffer_free_function.
 **/
static void free_pooled_data_kvio(void *poolData, void *data)
{
	if (data == NULL) {
		return;
	}

	struct data_kvio *data_kvio = (struct data_kvio *)data;
	struct kernel_layer *layer = (struct kernel_layer *)poolData;
	if (WRITE_PROTECT_FREE_POOL) {
		set_write_protect(data_kvio, WP_DATA_KVIO_SIZE, false);
	}

	if (data_kvio->dataBlockBio != NULL) {
		free_bio(data_kvio->dataBlockBio, layer);
	}

	if (data_kvio->readBlock.bio != NULL) {
		free_bio(data_kvio->readBlock.bio, layer);
	}

	FREE(data_kvio->readBlock.buffer);
	FREE(data_kvio->dataBlock);
	FREE(data_kvio->scratchBlock);
	FREE(data_kvio);
}

/**
 * Allocate a data_kvio. This function is the internals of
 * makePooledDataKVIO().
 *
 * @param [in]  layer          The layer in which the data_kvio will operate
 * @param [out] data_kvio_ptr  A pointer to hold the newly allocated data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_pooled_data_kvio(struct kernel_layer *layer,
				     struct data_kvio **data_kvio_ptr)
{
	struct data_kvio *data_kvio;
	int result;
	if (WRITE_PROTECT_FREE_POOL) {
		STATIC_ASSERT(WP_DATA_KVIO_SIZE >= sizeof(struct data_kvio));
		result = allocateMemory(WP_DATA_KVIO_SIZE, 0, __func__,
					&data_kvio);
		if (result == VDO_SUCCESS) {
			BUG_ON((((size_t)data_kvio) & (PAGE_SIZE - 1)) != 0);
		}
	} else {
		result = ALLOCATE(1, struct data_kvio, __func__, &data_kvio);
	}

	if (result != VDO_SUCCESS) {
		return logErrorWithStringError(result,
					       "data_kvio allocation failure");
	}

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio data",
				&data_kvio->dataBlock);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(layer, data_kvio);
		return logErrorWithStringError(
			result, "data_kvio data allocation failure");
	}

	result = create_bio(layer,
			    data_kvio->dataBlock,
			    &data_kvio->dataBlockBio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(layer, data_kvio);
		return logErrorWithStringError(result,
					       "data_kvio data bio allocation failure");
	}

	result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio read buffer",
				&data_kvio->readBlock.buffer);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(layer, data_kvio);
		return logErrorWithStringError(
			result, "data_kvio read allocation failure");
	}

	result = create_bio(layer, data_kvio->readBlock.buffer,
			    &data_kvio->readBlock.bio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(layer, data_kvio);
		return logErrorWithStringError(result,
					       "data_kvio read bio allocation failure");
	}

	data_kvio->readBlock.bio->bi_private = &data_kvio->kvio;

	result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio scratch",
				&data_kvio->scratchBlock);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(layer, data_kvio);
		return logErrorWithStringError(result,
					       "data_kvio scratch allocation failure");
	}

	*data_kvio_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**
 * Implements buffer_allocate_function.
 **/
static int make_pooled_data_kvio(void *pool_data, void **data_ptr)
{
	struct data_kvio *data_kvio = NULL;
	int result =
		allocate_pooled_data_kvio((struct kernel_layer *) pool_data,
					  &data_kvio);
	if (result != VDO_SUCCESS) {
		free_pooled_data_kvio(pool_data, data_kvio);
		return result;
	}

	*data_ptr = data_kvio;
	return VDO_SUCCESS;
}

/**
 * Dump out the waiters on each DataVIO in the DataVIO buffer pool.
 *
 * @param queue    The queue to check (logical or physical)
 * @param wait_on  The label to print for queue (logical or physical)
 **/
static void dump_vio_waiters(WaitQueue *queue, char *wait_on)
{
	Waiter *first = getFirstWaiter(queue);
	if (first == NULL) {
		return;
	}

	DataVIO *data_vio = waiterAsDataVIO(first);
	logInfo("      %s is locked. Waited on by: VIO %" PRIptr " pbn %" PRIu64
		" lbn %llu d-pbn %llu lastOp %s",
		wait_on, data_vio, getDataVIOAllocation(data_vio),
		data_vio->logical.lbn, data_vio->duplicate.pbn,
		getOperationName(data_vio));

	for (Waiter *waiter = first->nextWaiter; waiter != first;
	     waiter = waiter->nextWaiter) {
		data_vio = waiterAsDataVIO(waiter);
		logInfo("     ... and : VIO %" PRIptr " pbn %" PRIu64
			" lbn %llu d-pbn %llu lastOp %s",
			data_vio, getDataVIOAllocation(data_vio),
			data_vio->logical.lbn, data_vio->duplicate.pbn,
			getOperationName(data_vio));
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
static void encode_vio_dump_flags(DataVIO *data_vio, char buffer[8])
{
	char *pFlag = buffer;
	*pFlag++ = ' ';
	if (dataVIOAsCompletion(data_vio)->result != VDO_SUCCESS) {
		*pFlag++ = 'R';
	}
	if (dataVIOAsAllocatingVIO(data_vio)->waiter.nextWaiter != NULL) {
		*pFlag++ = 'W';
	}
	if (data_vio->isDuplicate) {
		*pFlag++ = 'D';
	}
	if (pFlag == &buffer[1]) {
		// No flags, so remove the blank space.
		pFlag = buffer;
	}
	*pFlag = '\0';
}

/**
 * Dump out info on a data_kvio from the data_kvio pool.
 *
 * <p>Implements buffer_dump_function.
 *
 * @param pool_data  The pool data
 * @param data      The data_kvio to dump
 **/
static void dumpPooledDataKVIO(void *pool_data __attribute__((unused)),
			       void *data)
{
	struct data_kvio *data_kvio = (struct data_kvio *)data;
	DataVIO *data_vio = &data_kvio->dataVIO;

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
	dump_work_item_to_buffer(&data_kvio->kvio.enqueueable.work_item,
				 vio_work_item_dump_buffer,
				 sizeof(vio_work_item_dump_buffer));
	// Another static buffer...
	// log10(256) = 2.408+, round up:
	enum { DECIMAL_DIGITS_PER_UINT64_T = (int)(1 + 2.41 * sizeof(uint64_t))
	};
	static char vio_block_number_dump_buffer[sizeof("P L D") +
						 3 *
						 DECIMAL_DIGITS_PER_UINT64_T];
	if (data_vio->isDuplicate) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu D%llu",
			 getDataVIOAllocation(data_vio), data_vio->logical.lbn,
			 data_vio->duplicate.pbn);
	} else if (hasAllocation(data_vio)) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu",
			 getDataVIOAllocation(data_vio), data_vio->logical.lbn);
	} else {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer), "L%llu",
			 data_vio->logical.lbn);
	}

	static char vio_flush_generation_buffer[sizeof(" FG") +
						DECIMAL_DIGITS_PER_UINT64_T] =
		"";
	if (data_vio->flushGeneration != 0) {
		snprintf(vio_flush_generation_buffer,
			 sizeof(vio_flush_generation_buffer), " FG%llu",
			 data_vio->flushGeneration);
	}

	// Encode VIO attributes as a string of one-character flags, usually
	// empty.
	static char flags_dump_buffer[8];
	encode_vio_dump_flags(data_vio, flags_dump_buffer);

	logInfo("  kvio %" PRIptr " %s%s %s %s%s", data_kvio,
		vio_block_number_dump_buffer, vio_flush_generation_buffer,
		getOperationName(data_vio), vio_work_item_dump_buffer,
		flags_dump_buffer);
	// might want info on: wantAlbireoAnswer / operation / status
	// might want info on: bio / bio_to_submit / bios_merged

	dump_vio_waiters(&data_vio->logical.waiters, "lbn");

	// might want to dump more info from VIO here
}

/**********************************************************************/
int make_data_kvio_buffer_pool(struct kernel_layer *layer,
			       uint32_t pool_size,
			       struct buffer_pool **buffer_pool_ptr)
{
	return make_buffer_pool("data_kvio pool",
				pool_size,
				make_pooled_data_kvio,
				free_pooled_data_kvio,
				dumpPooledDataKVIO,
				layer,
				buffer_pool_ptr);
}

/**********************************************************************/
DataLocation get_dedupe_advice(const struct dedupe_context *context)
{
	struct data_kvio *data_kvio = container_of(context,
						   struct data_kvio,
						   dedupeContext);
	return (DataLocation){
		.state = data_kvio->dataVIO.newMapped.state,
		.pbn = data_kvio->dataVIO.newMapped.pbn,
	};
}

/**********************************************************************/
void set_dedupe_advice(struct dedupe_context *context,
		       const DataLocation *advice)
{
	struct data_kvio *data_kvio = container_of(context,
						   struct data_kvio,
						   dedupeContext);
	receiveDedupeAdvice(&data_kvio->dataVIO, advice);
}
