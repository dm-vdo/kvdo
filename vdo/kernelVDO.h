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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.h#32 $
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"
#include "types.h"
#include "vdo.h"
#include "vdoInternal.h"

#include "kernelTypes.h"
#include "threadRegistry.h"
#include "workQueue.h"

enum {
	REQ_Q_ACTION_COMPLETION,
	REQ_Q_ACTION_FLUSH,
	REQ_Q_ACTION_MAP_BIO,
	REQ_Q_ACTION_SYNC,
	REQ_Q_ACTION_VIO_CALLBACK
};

/**
 * Make base threads.
 *
 * @param [in]  vdo            The vdo to be initialized
 * @param [in]  thread_config  The base-code thread configuration
 * @param [out] reason         The reason for failure
 *
 * @return  VDO_SUCCESS or an error code
 **/
int make_vdo_threads(struct vdo *vdo,
		     const struct thread_config *thread_config,
		     char **reason);

/**
 * Load the VDO state from disk but don't alter the on-disk state. This method
 * is ultimately called from the constructor for devices which have not been
 * resumed.
 *
 * @param [in]  vdo                  The vdo to be started
 * @param [in]  common               The physical layer pointer
 * @param [in]  load_config          Load-time parameters for the VDO
 * @param [in]  vio_trace_recording  Debug flag to store
 * @param [out] reason               The reason for failure
 **/
int preload_kvdo(struct vdo *vdo,
		 PhysicalLayer *common,
		 const struct vdo_load_config *load_config,
		 bool vio_trace_recording,
		 char **reason);

/**
 * Starts the base VDO instance associated with the kernel layer. This method
 * is ultimately called from preresume the first time an instance is resumed.
 *
 * @param [in]  vdo     The vdo to be started
 * @param [in]  common  The physical layer pointer
 * @param [out] reason  The reason for failure
 *
 * @return VDO_SUCCESS if started, otherwise error
 **/
int start_kvdo(struct vdo *vdo, PhysicalLayer *common, char **reason);

/**
 * Suspend the base VDO instance associated with the kernel layer.
 *
 * @param vdo  The vdo to be suspended
 *
 * @return VDO_SUCCESS if stopped, otherwise error
 **/
int suspend_kvdo(struct vdo *vdo);

/**
 * Resume the base VDO instance associated with the kernel layer.
 *
 * @param vdo  The vdo to be resumed
 *
 * @return VDO_SUCCESS or an error
 **/
int resume_kvdo(struct vdo *vdo);

/**
 * Shut down the base code interface. The vdo object must first be stopped.
 *
 * @param vdo  The vdo to be shut down
 **/
void finish_kvdo(struct vdo *vdo);

/**
 * Free up storage of the base code interface. The vdo object must first have
 * been "finished".
 *
 * @param vdo  The vdo object to be destroyed
 **/
void destroy_kvdo(struct vdo *vdo);


/**
 * Dump to the kernel log any work-queue info associated with the base code.
 *
 * @param vdo  The vdo object to be examined
 **/
void dump_vdo_work_queue(struct vdo *vdo);

/**
 * Set whether compression is enabled.
 *
 * @param vdo                 The vdo object
 * @param enable_compression  The new compression mode
 *
 * @return state of compression before new value is set
 **/
bool set_kvdo_compressing(struct vdo *vdo, bool enable_compression);

/**
 * Gets the latest statistics gathered by the base code.
 *
 * @param vdo    the vdo object
 * @param stats  the statistics struct to fill in
 **/
void get_kvdo_statistics(struct vdo *vdo,
			 struct vdo_statistics *stats);

/**
 * Dump base code status information to the kernel log for debugging.
 *
 * @param vdo  The vdo to be examined
 **/
void dump_kvdo_status(struct vdo *vdo);

/**
 * Notify the base code of resized physical storage.
 *
 * @param vdo             The vdo to be updated
 * @param physical_count  The new size
 *
 * @return VDO_SUCCESS or error
 **/
int kvdo_resize_physical(struct vdo *vdo, block_count_t physical_count);

/**
 * Request the base code grow the logical space.
 *
 * @param vdo            The vdo to be updated
 * @param logical_count  The new size
 *
 * @return VDO_SUCCESS or error
 **/
int kvdo_resize_logical(struct vdo *vdo, block_count_t logical_count);

/**
 * Request the base code go read-only.
 *
 * @param vdo     The vdo to be updated
 * @param result  The error code causing the read only
 **/
void set_kvdo_read_only(struct vdo *vdo, int result);


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
 * @param action          Action code, mapping to a relative priority
 **/
void enqueue_vio(struct vio *vio,
		 vdo_work_function work,
		 void *stats_function,
		 unsigned int action);

/**
 * Enqueue an arbitrary completion for execution on its indicated thread.
 *
 * @param completion  The completion to enqueue
 **/
void kvdo_enqueue(struct vdo_completion *completion);

#endif // KERNEL_VDO_H
