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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.h#77 $
 */

#ifndef DATA_KVIO_H
#define DATA_KVIO_H

#include <linux/atomic.h>

#include "data-vio.h"

#include "batchProcessor.h"
#include "bufferPool.h"
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
 * Set up and enqueue a data_vio on the CPU queue.
 *
 * @param data_vio        The data_vio to set up
 * @param work            The function pointer to execute
 * @param priority        The priority for this work
 **/
static inline void
launch_data_vio_on_cpu_queue(struct data_vio *data_vio,
			     vdo_work_function work,
			     enum vdo_work_item_priority priority)
{
	struct vdo *vdo = get_vdo_from_data_vio(data_vio);

	launch_vio(data_vio_as_vio(data_vio),
		   work,
		   priority,
		   vdo->threads[vdo->thread_config->cpu_thread].queue);
}

/**
 * Set up and enqueue a data_vio on the bio Ack queue.
 *
 * @param data_vio        The data_vio to set up
 * @param work            The function pointer to execute
 * @param priority        The priority for this work
 **/
static inline void
launch_data_vio_on_bio_ack_queue(struct data_vio *data_vio,
				 vdo_work_function work,
				 enum vdo_work_item_priority priority)
{
	struct vdo *vdo = get_vdo_from_data_vio(data_vio);

	launch_vio(data_vio_as_vio(data_vio),
		   work,
		   priority,
		   vdo->threads[vdo->thread_config->bio_ack_thread].queue);
}

/**
 * Move a data_vio back to the base threads.
 *
 * @param data_vio The data_vio to enqueue
 **/
static inline void enqueue_data_vio_callback(struct data_vio *data_vio)
{
	continue_vio(data_vio_as_vio(data_vio), VDO_SUCCESS);
}

/**
 * Initialize a newly acquiered data_vio based on an incoming bio and initiate
 * processing of the request.
 *
 * @param vdo       The vdo
 * @param data_vio  The data_vio which will process the bio
 * @param bio       The incoming bio to be processed
 **/
void launch_data_vio(struct vdo *vdo,
		     struct data_vio *data_vio,
		     struct bio *bio);

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
 * @param priority       The priority of this read
 * @param callback       The function to call when the read is done
 **/
void vdo_read_block(struct data_vio *data_vio,
		    physical_block_number_t location,
		    enum block_mapping_state mapping_state,
		    enum vdo_work_item_priority priority,
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
vdo_get_dedupe_advice(const struct dedupe_context *context);

/**
 * Set the result of a dedupe query for the data_vio associated with a
 * dedupe_context.
 *
 * @param context  The context receiving advice
 * @param advice   A data location at which the chunk named in the context
 *                 might be stored (will be NULL if no advice was found)
 **/
void vdo_set_dedupe_advice(struct dedupe_context *context,
			   const struct data_location *advice);

#endif /* DATA_KVIO_H */
