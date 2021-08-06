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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/kernel/kvio.h#1 $
 */

#ifndef KVIO_H
#define KVIO_H

#include "vio.h"

#include "kernelLayer.h"
#include "workQueue.h"

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
				  vdo_work_function work,
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
			      vdo_work_function work,
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

#endif /* KVIO_H */
