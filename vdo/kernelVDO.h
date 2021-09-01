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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.h#50 $
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"
#include "types.h"
#include "vdo.h"

#include "kernelTypes.h"
#include "workQueue.h"

/**
 * Make base threads.
 *
 * @param [in]  vdo                 The vdo to be initialized
 * @param [in]  thread_name_prefix  The per-device prefix to use in thread
 *                                  names
 * @param [out] reason              The reason for failure
 *
 * @return VDO_SUCCESS or an error code
 **/
int make_vdo_threads(struct vdo *vdo,
		     const char *thread_name_prefix,
		     char **reason);

/**
 * Dump to the kernel log any work-queue info associated with the base code.
 *
 * @param vdo  The vdo object to be examined
 **/
void dump_vdo_work_queue(struct vdo *vdo);

/**
 * Gets the latest statistics gathered by the base code.
 *
 * @param vdo    the vdo object
 * @param stats  the statistics struct to fill in
 **/
void get_kvdo_statistics(struct vdo *vdo,
			 struct vdo_statistics *stats);


/**
 * Enqueue a work item to be processed in the base code context.
 *
 * @param vdo        The vdo object in which to run the work item
 * @param item       The work item to be run
 * @param thread_id  The thread on which to run the work item
 **/
void enqueue_vdo_work(struct vdo *vdo,
		      struct vdo_work_item *item,
		      thread_id_t thread_id);

/**
 * Set up and enqueue a vio's work item to be processed in the base code
 * context.
 *
 * @param vio             The vio with the work item to be run
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param priority        The priority of the work
 **/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 void *stats_function,
		 enum vdo_work_item_priority priority);

#endif // KERNEL_VDO_H
