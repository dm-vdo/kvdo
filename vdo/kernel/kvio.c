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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#49 $
 */

#include "kvio.h"


#include "logger.h"
#include "memoryAlloc.h"

#include "numUtils.h"
#include "vdo.h"
#include "waitQueue.h"

#include "bio.h"
#include "dataKVIO.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"

/**********************************************************************/
struct kvio *work_item_as_kvio(struct kvdo_work_item *item)
{
	struct vio *vio = container_of(item, struct vio, completion.work_item);
	if (is_metadata_vio(vio)) {
                return &(vio_as_metadata_kvio(vio)->kvio);
	}

	if (is_compressed_write_vio(vio)) {
		struct allocating_vio *avio = vio_as_allocating_vio(vio);
		return &(allocating_vio_as_compressed_write_kvio(avio)->kvio);
	}

	return &(data_vio_as_data_kvio(vio_as_data_vio(vio))->kvio);
}

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio
 *
 * @param item    The work item of the vio to complete
 **/
static void kvdo_handle_vio_callback(struct kvdo_work_item *item)
{
	struct kvio *kvio = work_item_as_kvio(item);

	run_callback(vio_as_completion(kvio->vio));
}

/**********************************************************************/
void kvdo_enqueue_vio_callback(struct kvio *kvio)
{
	enqueue_kvio(kvio,
		     kvdo_handle_vio_callback,
		     (KvdoWorkFunction) vio_as_completion(kvio->vio)->callback,
		     REQ_Q_ACTION_VIO_CALLBACK);
}

/**********************************************************************/
void kvdo_continue_kvio(struct kvio *kvio, int error)
{
	if (unlikely(error != VDO_SUCCESS)) {
		set_completion_result(vio_as_completion(kvio->vio), error);
	}
	kvdo_enqueue_vio_callback(kvio);
}

/**********************************************************************/
// noinline ensures systemtap can hook in here
static noinline void maybe_log_kvio_trace(struct kvio *kvio)
{
	if (kvio->layer->trace_logging) {
		log_kvio_trace(kvio);
	}
}

/**********************************************************************/
static void free_kvio(struct kvio **kvio_ptr)
{
	struct kvio *kvio = *kvio_ptr;

	if (kvio == NULL) {
		return;
	}

	if (unlikely(kvio->vio->trace != NULL)) {
		maybe_log_kvio_trace(kvio);
		FREE(kvio->vio->trace);
	}

	free_bio(kvio->bio);
	FREE(kvio);
	*kvio_ptr = NULL;
}

/**********************************************************************/
void free_metadata_kvio(struct metadata_kvio **metadata_kvio_ptr)
{
	free_kvio((struct kvio **) metadata_kvio_ptr);
}

/**********************************************************************/
void free_compressed_write_kvio(
	struct compressed_write_kvio **compressed_write_kvio_ptr)
{
	free_kvio((struct kvio **) compressed_write_kvio_ptr);
}

/**********************************************************************/
void write_compressed_block(struct allocating_vio *allocating_vio)
{
	// This method assumes that compressed writes never set the flush or
	// FUA bits.
	struct compressed_write_kvio *compressed_write_kvio =
		allocating_vio_as_compressed_write_kvio(allocating_vio);
	struct kvio *kvio =
		compressed_write_kvio_as_kvio(compressed_write_kvio);
	struct bio *bio = kvio->bio;

	// Write the compressed block, using the compressed kvio's own bio.
	int result = reset_bio_with_buffer(bio, compressed_write_kvio->data, 
					   kvio, complete_async_bio,
					   REQ_OP_WRITE, kvio->vio->physical);
	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_ACTION_COMPRESSED_DATA);
}

/**
 * Get the BioQueue action for a metadata vio based on that vio's priority.
 *
 * @param vio  The vio
 *
 * @return The action with which to submit the vio's bio.
 **/
static inline bio_q_action get_metadata_action(struct vio *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH) ? BIO_Q_ACTION_HIGH :
						       BIO_Q_ACTION_METADATA);
}

/**********************************************************************/
void submit_metadata_vio(struct vio *vio)
{
	struct metadata_kvio *metadata_kvio = vio_as_metadata_kvio(vio);
	struct kvio *kvio = metadata_kvio_as_kvio(metadata_kvio);
	struct bio *bio = kvio->bio;

	unsigned int bi_opf;
	if (is_read_vio(vio)) {
		ASSERT_LOG_ONLY(!vio_requires_flush_before(vio),
				"read vio does not require flush before");
		vio_add_trace_record(vio, THIS_LOCATION("$F;io=readMeta"));
		bi_opf = REQ_OP_READ;
	} else {
		kernel_layer_state state = get_kernel_layer_state(kvio->layer);

		ASSERT_LOG_ONLY(((state == LAYER_RUNNING)
				 || (state == LAYER_RESUMING)
				 || (state = LAYER_STARTING)),
				"write metadata in allowed state %d", state);
		if (vio_requires_flush_before(vio)) {
			bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
			vio_add_trace_record(vio,
					     THIS_LOCATION("$F;io=flushWriteMeta"));
		} else {
			bi_opf = REQ_OP_WRITE;
			vio_add_trace_record(vio,
					     THIS_LOCATION("$F;io=writeMeta"));
		}
	}

	if (vio_requires_flush_after(vio)) {
		bi_opf |= REQ_FUA;
	}

	int result = reset_bio_with_buffer(bio, metadata_kvio->data, kvio,
					   complete_async_bio, bi_opf,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}

	// Perform the metadata IO, using the metadata kvio's own bio.
	vdo_submit_bio(bio, get_metadata_action(vio));
}

/**
 * Handle the completion of a base-code initiated flush by continuing the flush
 * vio.
 *
 * @param bio    The bio to complete
 **/
static void complete_flush_bio(struct bio *bio)
{
	int error = get_bio_result(bio);
	struct kvio *kvio = (struct kvio *) bio->bi_private;
	kvdo_continue_kvio(kvio, error);
}

/**********************************************************************/
void kvdo_flush_vio(struct vio *vio)
{
	struct kvio *kvio = metadata_kvio_as_kvio(vio_as_metadata_kvio(vio));
	struct bio *bio = kvio->bio;

	/*
	 * One would think we could use REQ_OP_FLUSH on new kernels, but some
	 * layers of the stack don't recognize that as a flush. So do it
	 * like blkdev_issue_flush() and make it a write+flush.
	 */
	int result = reset_bio_with_buffer(bio, NULL, kvio, complete_flush_bio,
					   REQ_OP_WRITE | REQ_PREFLUSH, 0);
	if (result != VDO_SUCCESS) {
		kvdo_continue_kvio(kvio, result);
		return;
	}
	vdo_submit_bio(bio, get_metadata_action(vio));
}

/*
 * Hook for a SystemTap probe to potentially restrict the choices
 * of which vios should have their latencies tracked.
 *
 * Normally returns true. Even if true is returned, sample_this_one may
 * cut down the monitored vios by some fraction so as to reduce the
 * impact on system performance.
 *
 * Must be "noinline" so that SystemTap can find the return
 * instruction and modify the return value.
 *
 * @param kvio   The kvio being initialized
 * @param layer  The kernel layer
 * @param bio    The incoming I/O request
 *
 * @return whether it's useful to track latency for vios looking like
 *         this one
 */
static noinline bool sample_this_vio(struct kvio *kvio,
				     struct kernel_layer *layer,
				     struct bio *bio)
{
	bool result = true;
	// Ensure the arguments and result exist at the same time, for
	// SystemTap.
	__asm__ __volatile__(""
			     : "=g"(result)
			     : "0"(result), "g"(kvio), "g"(layer), "g"(bio)
			     : "memory");
	return result;
}

/**********************************************************************/
void initialize_kvio(struct kvio *kvio,
		     struct kernel_layer *layer,
		     vio_type vio_type,
		     vio_priority priority,
		     void *parent,
		     struct bio *bio)
{
	if (layer->vioTraceRecording && sample_this_vio(kvio, layer, bio) &&
	    sample_this_one(&layer->trace_sample_counter)) {
		int result =
			(is_data_vio_type(vio_type) ?
			 alloc_trace_from_pool(layer, &kvio->vio->trace) :
			 ALLOCATE(1, struct trace, "trace", &kvio->vio->trace));
		if (result != VDO_SUCCESS) {
			uds_log_error("trace record allocation failure %d",
				      result);
		}
	}

	kvio->bio = bio;
	kvio->layer = layer;
	if (bio != NULL) {
		bio->bi_private = kvio;
	}

	initialize_vio(kvio->vio,
		       vio_type,
		       priority,
		       parent,
		       get_vdo(&layer->kvdo),
		       &layer->common);

	// XXX: The "init" label should be replaced depending on the
	// write/read/flush path followed.
	kvio_add_trace_record(kvio, THIS_LOCATION("$F;io=?init;j=normal"));
}

/**********************************************************************/
int kvdo_create_metadata_vio(PhysicalLayer *layer,
			     vio_type vio_type,
			     vio_priority priority,
			     void *parent,
			     char *data,
			     struct vio **vio_ptr)
{
	int result = ASSERT(is_metadata_vio_type(vio_type),
			    "%d is a metadata type",
			    vio_type);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct bio *bio;

	result = create_bio(data, &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// If struct metadata_kvio grows past 256 bytes, we'll lose benefits of
	// VDOSTORY-176.
	STATIC_ASSERT(sizeof(struct metadata_kvio) <= 256);

	// Metadata vios should use direct allocation and not use the buffer
	// pool, which is reserved for submissions from the linux block layer.
	struct metadata_kvio *metadata_kvio;
	result = ALLOCATE(1, struct metadata_kvio, __func__, &metadata_kvio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata kvio allocation failure %d", result);
		free_bio(bio);
		return result;
	}

	struct kvio *kvio = &metadata_kvio->kvio;

	kvio->vio = &metadata_kvio->vio;
	initialize_kvio(kvio, as_kernel_layer(layer), vio_type, priority,
			parent, bio);
	metadata_kvio->data = data;

	*vio_ptr = &metadata_kvio->vio;
	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_create_compressed_write_vio(PhysicalLayer *layer,
				     void *parent,
				     char *data,
				     struct allocating_vio **allocating_vio_ptr)
{
	struct bio *bio;
	int result = create_bio(data, &bio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	// Compressed write vios should use direct allocation and not use the
	// buffer pool, which is reserved for submissions from the linux block
	// layer.
	struct compressed_write_kvio *compressed_write_kvio;
	result = ALLOCATE(1, struct compressed_write_kvio, __func__,
			  &compressed_write_kvio);
	if (result != VDO_SUCCESS) {
		uds_log_error("compressed write kvio allocation failure %d",
			      result);
		free_bio(bio);
		return result;
	}

	struct kvio *kvio = &compressed_write_kvio->kvio;

	kvio->vio =
		allocating_vio_as_vio(&compressed_write_kvio->allocating_vio);
	initialize_kvio(kvio,
			as_kernel_layer(layer),
			VIO_TYPE_COMPRESSED_BLOCK,
			VIO_PRIORITY_COMPRESSED_DATA,
			parent,
			bio);
	compressed_write_kvio->data = data;

	*allocating_vio_ptr = &compressed_write_kvio->allocating_vio;
	return VDO_SUCCESS;
}
