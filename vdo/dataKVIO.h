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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/dataKVIO.h#1 $
 */

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include "atomicDefs.h"
#include "uds.h"

#include "dataVIO.h"

#include "kernelVDO.h"
#include "kvio.h"

/**
 * Returns a pointer to the data_vio wrapping a work item.
 *
 * @param item  the work item
 *
 * @return the data_vio
 **/
static inline struct data_vio *
work_item_as_data_vio(struct vdo_work_item *item)
{
	return vio_as_data_vio(work_item_as_vio(item));
}

/**
 * Get the work_item from a data_vio.
 *
 * @param data_vio  The data_vio
 *
 * @return the data_vio's work item
 **/
static inline struct vdo_work_item *
work_item_from_data_vio(struct data_vio *data_vio)
{
	return work_item_from_vio(data_vio_as_vio(data_vio));
}

/**
 * Set up and enqueue a data_vio on the CPU queue.
 *
 * @param data_vio        The data_vio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void launch_data_vio_on_cpu_queue(struct data_vio *data_vio,
						vdo_work_function work,
						void *stats_function,
						unsigned int action)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	launch_vio(vio, work, stats_function, action,
		   vdo_as_kernel_layer(vio->vdo)->cpu_queue);
}

/**
 * Set up and enqueue a data_vio on the bio Ack queue.
 *
 * @param data_vio        The data_vio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void
launch_data_vio_on_bio_ack_queue(struct data_vio *data_vio,
				 vdo_work_function work,
				 void *stats_function,
				 unsigned int action)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct kernel_layer *layer = vdo_as_kernel_layer(vio->vdo);
	launch_vio(vio, work, stats_function, action, layer->bio_ack_queue);
}

/**
 * Move a data_vio back to the base threads.
 *
 * @param data_vio The data_vio to enqueue
 **/
static inline void enqueue_data_vio_callback(struct data_vio *data_vio)
{
	enqueue_vio_callback(data_vio_as_vio(data_vio));
}

/**
 * Associate a vio with a bio passed in from the block layer, and start
 * processing the vio.
 *
 * If setting up a vio fails, a message is logged, and the limiter permits
 * (request and maybe discard) released, but the caller is responsible for
 * disposing of the bio.
 *
 * @param vdo                 The vdo
 * @param bio                 The bio for which to create vio
 * @param arrival_jiffies     The time (in jiffies) when the external request
 *                            entered the device mapbio function
 * @param has_discard_permit  Whether we got a permit from the discard
 *                            limiter of the kernel layer
 *
 * @return VDO_SUCCESS or a system error code
 **/
int __must_check vdo_launch_data_vio_from_bio(struct vdo *vdo,
					      struct bio *bio,
					      uint64_t arrival_jiffies,
					      bool has_discard_permit);

/**
 * Return a batch of data_vio objects to the pool.
 *
 * <p>Implements batch_processor_callback.
 *
 * @param batch    The batch processor
 * @param closure  The kernal layer
 **/
void return_data_vio_batch_to_pool(struct batch_processor *batch,
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
void vdo_read_block(struct data_vio *data_vio,
		    physical_block_number_t location,
		    enum block_mapping_state mapping_state,
		    enum bio_q_action action,
		    vdo_action *callback);

/**
 * Allocate a buffer pool of data_vio objects.
 *
 * @param [in]  pool_size        The number of data_vio objects in the pool
 * @param [out] buffer_pool_ptr  A pointer to hold the new buffer pool
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_data_vio_buffer_pool(uint32_t pool_size,
			  struct buffer_pool **buffer_pool_ptr);

/**
 * Get the state needed to generate UDS metadata from the data_vio
 * associated with a dedupe_context.
 *
 * @param context  The dedupe_context
 *
 * @return the advice to store in the UDS index
 **/
struct data_location __must_check
get_dedupe_advice(const struct dedupe_context *context);

/**
 * Set the result of a dedupe query for the data_vio associated with a
 * dedupe_context.
 *
 * @param context  The context receiving advice
 * @param advice   A data location at which the chunk named in the context
 *                 might be stored (will be NULL if no advice was found)
 **/
void set_dedupe_advice(struct dedupe_context *context,
		       const struct data_location *advice);

#endif /* DATA_KVIO_H */
