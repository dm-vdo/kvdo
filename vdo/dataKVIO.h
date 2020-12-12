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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.h#49 $
 */

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include "atomicDefs.h"
#include "uds.h"

#include "dataVIO.h"

#include "kernelVDO.h"
#include "kvio.h"

struct external_io_request {
	/*
	 * The bio which was received from the device mapper to initiate an I/O
	 * request. This field will be non-NULL only until the request is
	 * acknowledged.
	 */
	struct bio *bio;
	// Cached copies of fields from the bio which will need to be reset
	// after we're done.
	void *private;
	void *end_io;
	// This is a copy of the bi_rw field of the bio which sadly is not just
	// a boolean read-write flag, but also includes other flag bits.
	unsigned long rw;
};

/* Dedupe support */
struct dedupe_context {
	struct uds_request uds_request;
	struct list_head pending_list;
	uint64_t submission_jiffies;
	atomic_t request_state;
	int status;
	bool is_pending;
	/** Hash of the associated VIO (NULL if not calculated) */
	const struct uds_chunk_name *chunk_name;
};

struct read_block {
	/**
	 * A pointer to a block that holds the data from the last read
	 * operation.
	 **/
	char *data;
	/**
	 * Temporary storage for doing reads from the underlying device.
	 **/
	char *buffer;
	/**
	 * Callback to invoke after completing the read I/O operation.
	 **/
	vdo_action *callback;
	/**
	 * Mapping state passed to kvdo_read_block(), used to determine whether
	 * the data must be uncompressed.
	 **/
	BlockMappingState mapping_state;
	/**
	 * The result code of the read attempt.
	 **/
	int status;
};

struct data_kvio {
	/* The embedded base code's data_vio */
	struct data_vio data_vio;
	/* The bio from the request which is being serviced by this vio. */
	struct external_io_request external_io_request;
	/* Dedupe */
	struct dedupe_context dedupe_context;
	/* Read cache */
	struct read_block read_block;
	/* partial block support */
	block_size_t offset;
	bool is_partial;
	/* discard support */
	bool has_discard_permit;
	uint32_t remaining_discard;
	/**
	 * A copy of user data written, so we can do additional processing
	 * (dedupe, compression) after acknowledging the I/O operation and
	 * thus losing access to the original data.
	 *
	 * Also used as buffer space for read-modify-write cycles when
	 * emulating smaller-than-blockSize I/O operations.
	 **/
	char *data_block;
	/** A bio structure, used for all reads and writes from/to disk. */
	struct bio *bio;
	/** A block used as output during compression or uncompression. */
	char *scratch_block;
};

/**
 * Convert a vio to a data_kvio.
 *
 * @param vio  The vio to convert
 *
 * @return The vio as a data_kvio
 **/
static inline struct data_kvio *vio_as_data_kvio(struct vio *vio)
{
	ASSERT_LOG_ONLY(is_data_vio(vio), "vio is a data_vio");
	return container_of(vio_as_data_vio(vio), struct data_kvio, data_vio);
}

/**
 * Convert a data_kvio to a vio.
 *
 * @param data_kvio  The data_kvio to convert
 *
 * @return The data_kvio as a vio
 **/
static inline struct vio *data_kvio_as_vio(struct data_kvio *data_kvio)
{
	return data_vio_as_vio(&data_kvio->data_vio);
}

/**
 * Returns a pointer to the data_kvio wrapping a data_vio.
 *
 * @param data_vio  the data_vio
 *
 * @return the data_kvio
 **/
static inline struct data_kvio *data_vio_as_data_kvio(struct data_vio *data_vio)
{
	return container_of(data_vio, struct data_kvio, data_vio);
}

/**
 * Returns a pointer to the data_kvio wrapping a work item.
 *
 * @param item  the work item
 *
 * @return the data_kvio
 **/
static inline struct data_kvio *
work_item_as_data_kvio(struct vdo_work_item *item)
{
	return vio_as_data_kvio(work_item_as_vio(item));
}

/**
 * Get the work_item from a data_kvio.
 *
 * @param data_kvio  The data_kvio
 *
 * @return the data_kvio's work item
 **/
static inline struct vdo_work_item *
work_item_from_data_kvio(struct data_kvio *data_kvio)
{
	return work_item_from_vio(data_kvio_as_vio(data_kvio));
}

/**
 * Get the kernel_layer from a data_kvio.
 *
 * @param data_kvio  The data_kvio from which to get the kernel_layer
 *
 * @return The data_kvio's kernel_layer
 **/
static inline struct kernel_layer *
get_layer_from_data_kvio(struct data_kvio *data_kvio)
{
	PhysicalLayer *layer =
		vio_as_completion(data_kvio_as_vio(data_kvio))->layer;
	return as_kernel_layer(layer);
}

/**
 * Set up and enqueue a data_kvio's work item to be processed in the base code
 * context.
 *
 * @param data_kvio       The data_kvio with the work item to be run
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void enqueue_data_kvio(struct data_kvio *data_kvio,
				     KvdoWorkFunction work,
				     void *stats_function,
				     unsigned int action)
{
	enqueue_vio(data_kvio_as_vio(data_kvio),
		    work,
		    stats_function,
		    action);
}

/**
 * Enqueue a data_kvio on a work queue.
 *
 * @param queue      The queue
 * @param data_kvio  The data_kvio
 **/
static inline void enqueue_data_kvio_work(struct kvdo_work_queue *queue,
					  struct data_kvio *data_kvio)
{
	enqueue_vio_work(queue, data_kvio_as_vio(data_kvio));
}

/**
 * Add a trace record for the current source location.
 *
 * @param data_kvio  The data_kvio structure to be updated
 * @param location   The source-location descriptor to be recorded
 **/
static inline void
data_kvio_add_trace_record(struct data_kvio *data_kvio,
			   const struct trace_location *location)
{
	data_vio_add_trace_record(&data_kvio->data_vio, location);
}

/**
 * Set up and enqueue a data_kvio on the CPU queue.
 *
 * @param data_kvio       The data_kvio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void launch_data_kvio_on_cpu_queue(struct data_kvio *data_kvio,
						 KvdoWorkFunction work,
						 void *stats_function,
						 unsigned int action)
{
	struct vio *vio = data_kvio_as_vio(data_kvio);
	launch_vio(vio, work, stats_function, action,
		   as_kernel_layer(vio_as_completion(vio)->layer)->cpu_queue);
}

/**
 * Set up and enqueue a data_kvio on the bio Ack queue.
 *
 * @param data_kvio       The data_kvio to set up
 * @param work            The function pointer to execute
 * @param stats_function   A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void
launch_data_kvio_on_bio_ack_queue(struct data_kvio *data_kvio,
				  KvdoWorkFunction work,
				  void *stats_function,
				  unsigned int action)
{
	struct vio *vio = data_kvio_as_vio(data_kvio);
	struct kernel_layer *layer =
		as_kernel_layer(vio_as_completion(vio)->layer);
	launch_vio(vio, work, stats_function, action, layer->bio_ack_queue);
}

/**
 * Move a data_kvio back to the base threads.
 *
 * @param data_kvio The data_kvio to enqueue
 **/
static inline void kvdo_enqueue_data_vio_callback(struct data_kvio *data_kvio)
{
	kvdo_enqueue_vio_callback(data_kvio_as_vio(data_kvio));
}

/**
 * Check whether the external request bio had FUA set.
 *
 * @param data_kvio  The data_kvio to check
 *
 * @return <code>true</code> if the external request bio had FUA set
 **/
static inline bool requestor_set_fua(struct data_kvio *data_kvio)
{
	return ((data_kvio->external_io_request.rw & REQ_FUA) == REQ_FUA);
}

/**
 * Associate a kvio with a bio passed in from the block layer, and start
 * processing the kvio.
 *
 * If setting up a kvio fails, a message is logged, and the limiter permits
 * (request and maybe discard) released, but the caller is responsible for
 * disposing of the bio.
 *
 * @param layer                 The physical layer
 * @param bio                   The bio for which to create kvio
 * @param arrival_jiffies       The time (in jiffies) when the external request
 *                              entered the device mapbio function
 * @param has_discard_permit    Whether we got a permit from the discardLimiter
 *                              of the kernel layer
 *
 * @return VDO_SUCCESS or a system error code
 **/
int __must_check
kvdo_launch_data_kvio_from_bio(struct kernel_layer *layer,
			       struct bio *bio,
			       uint64_t arrival_jiffies,
			       bool has_discard_permit);

/**
 * Return a batch of data_kvio objects to the pool.
 *
 * <p>Implements batch_processor_callback.
 *
 * @param batch    The batch processor
 * @param closure  The kernal layer
 **/
void return_data_kvio_batch_to_pool(struct batch_processor *batch,
				    void *closure);

/**
 * Fetch the data for a block from storage. The fetched data will be
 * uncompressed when the callback is called, and the result of the read
 * operation will be stored in the read_block's status field. On success,
 * the data will be in the read_block's data pointer.
 *
 * @param data_vio       The data_vio to read a block in for
 * @param location       The physical block number to read from
 * @param mapping_state  The mapping state of the block to read
 * @param action         The bio queue action
 * @param callback       The function to call when the read is done
 **/
void kvdo_read_block(struct data_vio *data_vio,
		     physical_block_number_t location,
		     BlockMappingState mapping_state,
		     bio_q_action action,
		     vdo_action *callback);

/**
 * Allocate a buffer pool of data_kvio objects.
 *
 * @param [in]  pool_size        The number of data_kvio objects in the pool
 * @param [out] buffer_pool_ptr  A pointer to hold the new buffer pool
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_data_kvio_buffer_pool(uint32_t pool_size,
			   struct buffer_pool **buffer_pool_ptr);

/**
 * Get the state needed to generate UDS metadata from the data_kvio
 * associated with a dedupe_context.
 *
 * @param context  The dedupe_context
 *
 * @return the advice to store in the UDS index
 **/
struct data_location __must_check
get_dedupe_advice(const struct dedupe_context *context);

/**
 * Set the result of a dedupe query for the data_kvio associated with a
 * dedupe_context.
 *
 * @param context  The context receiving advice
 * @param advice   A data location at which the chunk named in the context
 *                 might be stored (will be NULL if no advice was found)
 **/
void set_dedupe_advice(struct dedupe_context *context,
		       const struct data_location *advice);

#endif /* DATA_KVIO_H */
