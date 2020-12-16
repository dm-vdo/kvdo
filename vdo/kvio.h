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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.h#31 $
 */

#ifndef KVIO_H
#define KVIO_H

#include "allocatingVIO.h"
#include "vio.h"

#include "kernelLayer.h"

/**
 * Enqueue a vio on a work queue.
 *
 * @param queue  The queue
 * @param vio    The vio
 **/
static inline void enqueue_vio_work(struct vdo_work_queue *queue,
				    struct vio *vio)
{
	enqueue_work_queue(queue, work_item_from_vio(vio));
}

/**
 * Set up the work item for a vio.
 *
 * @param vio             The vio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
static inline void setup_vio_work(struct vio *vio,
				  KvdoWorkFunction work,
				  void *stats_function,
				  unsigned int action)
{
	setup_work_item(work_item_from_vio(vio),
			work,
			stats_function,
			action);
}

/**
 * Set up and enqueue a vio.
 *
 * @param vio             The vio to set up
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 * @param queue           The queue on which to enqueue the kvio
 **/
static inline void launch_vio(struct vio *vio,
			      KvdoWorkFunction work,
			      void *stats_function,
			      unsigned int action,
			      struct vdo_work_queue *queue)
{
	setup_vio_work(vio, work, stats_function, action);
	enqueue_vio_work(queue, vio);
}

/**
 * Move a vio back to the base threads.
 *
 * @param vio  The vio to enqueue
 **/
void enqueue_vio_callback(struct vio *vio);

/**
 * Handles vio-related I/O post-processing.
 *
 * @param vio    The vio to finalize
 * @param error  Possible error
 **/
void continue_vio(struct vio *vio, int error);

/**
 * Log the trace for a vio if tracing is enabled.
 *
 * @param vio  The vio to log
 **/
void maybe_log_vio_trace(struct vio *vio);

/**
 * Initialize a vio structure.
 *
 * @param vio        The vio to initialize
 * @param layer      The physical layer
 * @param type       The type of vio to create
 * @param priority   The relative priority to assign to the kvio
 * @param parent     The parent of the kvio completion
 * @param bio        The bio to associate with this kvio
 **/
void initialize_kvio(struct vio *vio,
		     struct kernel_layer *layer,
		     vio_type type,
		     vio_priority priority,
		     void *parent,
		     struct bio *bio);

/**
 * Create a new vio for metadata operations.
 *
 * <p>Implements metadata_vio_creator.
 *
 * @param [in]  layer      The physical layer
 * @param [in]  type       The type of vio to create
 * @param [in]  priority   The relative priority to assign to the vio
 * @param [in]  parent     The parent to assign to the vio's completion
 * @param [in]  data       The buffer
 * @param [out] vio_ptr    A pointer to hold new vio
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
kvdo_create_metadata_vio(PhysicalLayer *layer,
			 vio_type type,
			 vio_priority priority,
			 void *parent,
			 char *data,
			 struct vio **vio_ptr);

/**
 * Create a new allocating_vio for compressed writes.
 *
 * <p>Implements compressed_write_vio_creator.
 *
 * @param [in]  layer              The physical layer
 * @param [in]  parent             The parent to assign to the allocating_vio's
 *                                 completion
 * @param [in]  data               The buffer
 * @param [out] allocating_vio_ptr  A pointer to hold new allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
kvdo_create_compressed_write_vio(PhysicalLayer *layer,
				 void *parent,
				 char *data,
				 struct allocating_vio **allocating_vio_ptr);

/**
 * Issue an empty flush to the lower layer using the bio in a metadata vio.
 *
 * <p>Implements metadata_writer.
 *
 * @param vio  The vio to flush
 **/
void kvdo_flush_vio(struct vio *vio);

#endif /* KVIO_H */
