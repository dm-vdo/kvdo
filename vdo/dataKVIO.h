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
	struct vdo *vdo = vdo_get_from_data_vio(data_vio);

	launch_vio(data_vio_as_vio(data_vio),
		   work,
		   priority,
		   vdo->threads[vdo->thread_config->cpu_thread].queue);
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

void launch_data_vio(struct vdo *vdo,
		     struct data_vio *data_vio,
		     struct bio *bio);

void return_data_vio_batch_to_pool(struct batch_processor *batch,
				   void *closure);

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

struct data_location __must_check
vdo_get_dedupe_advice(const struct dedupe_context *context);

void vdo_set_dedupe_advice(struct dedupe_context *context,
			   const struct data_location *advice);

#endif /* DATA_KVIO_H */
