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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#59 $
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

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio.
 *
 * @param item    The work item of the vio to complete
 **/
static void kvdo_handle_vio_callback(struct vdo_work_item *item)
{
	run_callback(container_of(item, struct vdo_completion, work_item));
}

/**********************************************************************/
void enqueue_vio_callback(struct vio *vio)
{
	enqueue_vio(vio,
		    kvdo_handle_vio_callback,
		    (KvdoWorkFunction) vio_as_completion(vio)->callback,
		    REQ_Q_ACTION_VIO_CALLBACK);
}

/**********************************************************************/
void continue_vio(struct vio *vio, int error)
{
	if (unlikely(error != VDO_SUCCESS)) {
		set_completion_result(vio_as_completion(vio), error);
	}

	enqueue_vio_callback(vio);
}

/**********************************************************************/
void maybe_log_vio_trace(struct vio *vio)
{
	if (as_kernel_layer(vio_as_completion(vio)->layer)->trace_logging) {
		log_vio_trace(vio);
	}
}

/**********************************************************************/
void destroy_vio(struct vio **vio_ptr)
{
	struct vio *vio = *vio_ptr;

	if (vio == NULL) {
		return;
	}

	BUG_ON(is_data_vio(vio));

	if (unlikely(vio->trace != NULL)) {
		maybe_log_vio_trace(vio);
		FREE(vio->trace);
	}

	free_bio(vio->bio);
	FREE(vio);
	*vio_ptr = NULL;
}


/**********************************************************************/
void write_compressed_block(struct allocating_vio *allocating_vio)
{
	// This method assumes that compressed writes never set the flush or
	// FUA bits.
	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	struct bio *bio = vio->bio;

	// Write the compressed block, using the compressed kvio's own bio.
	int result = reset_bio_with_buffer(bio, vio->data, vio,
					   complete_async_bio, REQ_OP_WRITE,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	vdo_submit_bio(bio, BIO_Q_ACTION_COMPRESSED_DATA);
}

/**
 * Get the bio queue action for a metadata vio based on that vio's priority.
 *
 * @param vio  The vio
 *
 * @return The action with which to submit the vio's bio.
 **/
static inline enum bio_q_action get_metadata_action(struct vio *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH) ? BIO_Q_ACTION_HIGH :
						       BIO_Q_ACTION_METADATA);
}

/**********************************************************************/
void submit_metadata_vio(struct vio *vio)
{
	struct bio *bio = vio->bio;
	unsigned int bi_opf;
	if (is_read_vio(vio)) {
		ASSERT_LOG_ONLY(!vio_requires_flush_before(vio),
				"read vio does not require flush before");
		vio_add_trace_record(vio, THIS_LOCATION("$F;io=readMeta"));
		bi_opf = REQ_OP_READ;
	} else {
		struct kernel_layer *layer =
			as_kernel_layer(vio_as_completion(vio)->layer);
		enum kernel_layer_state state = get_kernel_layer_state(layer);
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

	int result = reset_bio_with_buffer(bio, vio->data, vio,
					   complete_async_bio, bi_opf,
					   vio->physical);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
		return;
	}

	// Perform the metadata IO, using the metadata kvio's own bio.
	vdo_submit_bio(bio, get_metadata_action(vio));
}

/**********************************************************************/
void kvdo_flush_vio(struct vio *vio)
{
	struct bio *bio = vio->bio;

	/*
	 * One would think we could use REQ_OP_FLUSH on new kernels, but some
	 * layers of the stack don't recognize that as a flush. So do it
	 * like blkdev_issue_flush() and make it a write+flush.
	 */
	int result = reset_bio_with_buffer(bio, NULL, vio, complete_async_bio,
					   REQ_OP_WRITE | REQ_PREFLUSH, 0);
	if (result != VDO_SUCCESS) {
		continue_vio(vio, result);
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
 * @param vio    The vio being initialized
 * @param layer  The kernel layer
 * @param bio    The incoming I/O request
 *
 * @return whether it's useful to track latency for vios looking like
 *         this one
 */
static noinline bool sample_this_vio(struct vio *vio,
				     struct kernel_layer *layer,
				     struct bio *bio)
{
	bool result = true;
	// Ensure the arguments and result exist at the same time, for
	// SystemTap.
	__asm__ __volatile__(""
			     : "=g"(result)
			     : "0"(result), "g"(vio), "g"(layer), "g"(bio)
			     : "memory");
	return result;
}

/**********************************************************************/
void initialize_kvio(struct vio *vio,
		     struct kernel_layer *layer,
		     vio_type type,
		     vio_priority priority,
		     void *parent,
		     struct bio *bio)
{
	if (layer->vio_trace_recording && sample_this_vio(vio, layer, bio) &&
	    sample_this_one(&layer->trace_sample_counter)) {
		int result =
			(is_data_vio_type(type) ?
			 alloc_trace_from_pool(layer, &vio->trace) :
			 ALLOCATE(1, struct trace, "trace", &vio->trace));
		if (result != VDO_SUCCESS) {
			uds_log_error("trace record allocation failure %d",
				      result);
		}
	}

	vio->bio = bio;
	initialize_vio(vio,
		       type,
		       priority,
		       parent,
		       get_vdo(&layer->kvdo),
		       &layer->common);

	// XXX: The "init" label should be replaced depending on the
	// write/read/flush path followed.
	vio_add_trace_record(vio, THIS_LOCATION("$F;io=?init;j=normal"));
}

/**********************************************************************/
int kvdo_create_metadata_vio(PhysicalLayer *layer,
			     vio_type type,
			     vio_priority priority,
			     void *parent,
			     char *data,
			     struct vio **vio_ptr)
{
	int result = ASSERT(is_metadata_vio_type(type),
			    "%d is a metadata type",
			    type);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct bio *bio;
	result = create_bio(&bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// If struct vio grows past 256 bytes, we'll lose benefits of
	// VDOSTORY-176.
	STATIC_ASSERT(sizeof(struct vio) <= 256);

	// Metadata vios should use direct allocation and not use the buffer
	// pool, which is reserved for submissions from the linux block layer.
	struct vio *vio;
	result = ALLOCATE(1, struct vio, __func__, &vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata kvio allocation failure %d", result);
		free_bio(bio);
		return result;
	}

	initialize_kvio(vio, as_kernel_layer(layer), type, priority,
			parent, bio);
	vio->data = data;
	*vio_ptr  = vio;
	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_create_compressed_write_vio(PhysicalLayer *layer,
				     void *parent,
				     char *data,
				     struct allocating_vio **allocating_vio_ptr)
{
	struct bio *bio;
	int result = create_bio(&bio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	// Compressed write vios should use direct allocation and not use the
	// buffer pool, which is reserved for submissions from the linux block
	// layer.
	struct allocating_vio *allocating_vio;
	result = ALLOCATE(1, struct allocating_vio, __func__,
			  &allocating_vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("compressed write vio allocation failure %d",
			      result);
		free_bio(bio);
		return result;
	}

	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	initialize_kvio(vio,
			as_kernel_layer(layer),
			VIO_TYPE_COMPRESSED_BLOCK,
			VIO_PRIORITY_COMPRESSED_DATA,
			parent,
			bio);
	vio->data = data;
	*allocating_vio_ptr = allocating_vio;
	return VDO_SUCCESS;
}
