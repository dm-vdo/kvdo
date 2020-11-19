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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelVDO.h#23 $
 */

#ifndef KERNEL_VDO_H
#define KERNEL_VDO_H

#include "completion.h"

#include "kernelTypes.h"
#include "threadRegistry.h"
#include "workQueue.h"

struct vdo_thread {
	struct kvdo *kvdo;
	thread_id_t thread_id;
	struct kvdo_work_queue *request_queue;
	struct registered_thread allocating_thread;
};

struct kvdo {
	struct vdo_thread *threads;
	thread_id_t initialized_thread_count;
	struct kvdo_work_item work_item;
	vdo_action *action;
	struct vdo_completion *completion;
	// Base-code device info
	struct vdo *vdo;
};

typedef enum reqQAction {
	REQ_Q_ACTION_COMPLETION,
	REQ_Q_ACTION_FLUSH,
	REQ_Q_ACTION_MAP_BIO,
	REQ_Q_ACTION_SYNC,
	REQ_Q_ACTION_VIO_CALLBACK
} ReqQAction;

/**
 * Initialize the base code interface.
 *
 * @param [in]  kvdo           The kvdo to be initialized
 * @param [in]  thread_config  The base-code thread configuration
 * @param [out] reason         The reason for failure
 *
 * @return  VDO_SUCCESS or an error code
 **/
int initialize_kvdo(struct kvdo *kvdo,
		    const struct thread_config *thread_config,
		    char **reason);

/**
 * Load the VDO state from disk but don't alter the on-disk state. This method
 * is ultimately called from the constructor for devices which have not been
 * resumed.
 *
 * @param [in]  kvdo                 The kvdo to be started
 * @param [in]  common               The physical layer pointer
 * @param [in]  load_config          Load-time parameters for the VDO
 * @param [in]  vio_trace_recording  Debug flag to store
 * @param [out] reason               The reason for failure
 **/
int preload_kvdo(struct kvdo *kvdo,
		 PhysicalLayer *common,
		 const struct vdo_load_config *load_config,
		 bool vio_trace_recording,
		 char **reason);

/**
 * Starts the base VDO instance associated with the kernel layer. This method
 * is ultimately called from preresume the first time an instance is resumed.
 *
 * @param [in]  kvdo                  The kvdo to be started
 * @param [in]  common                The physical layer pointer
 * @param [out] reason                The reason for failure
 *
 * @return VDO_SUCCESS if started, otherwise error
 */
int start_kvdo(struct kvdo *kvdo, PhysicalLayer *common, char **reason);

/**
 * Suspend the base VDO instance associated with the kernel layer.
 *
 * @param kvdo  The kvdo to be suspended
 *
 * @return VDO_SUCCESS if stopped, otherwise error
 **/
int suspend_kvdo(struct kvdo *kvdo);

/**
 * Resume the base VDO instance associated with the kernel layer.
 *
 * @param kvdo  The kvdo to be resumed
 *
 * @return VDO_SUCCESS or an error
 **/
int resume_kvdo(struct kvdo *kvdo);

/**
 * Shut down the base code interface. The kvdo object must first be
 * stopped.
 *
 * @param kvdo         The kvdo to be shut down
 **/
void finish_kvdo(struct kvdo *kvdo);

/**
 * Free up storage of the base code interface. The kvdo object must
 * first have been "finished".
 *
 * @param kvdo         The kvdo object to be destroyed
 **/
void destroy_kvdo(struct kvdo *kvdo);


/**
 * Dump to the kernel log any work-queue info associated with the base
 * code.
 *
 * @param kvdo     The kvdo object to be examined
 **/
void dump_kvdo_work_queue(struct kvdo *kvdo);

/**
 * Get the VDO pointer for a kvdo object
 *
 * @param kvdo          The kvdo object
 *
 * @return the VDO pointer
 */
static inline struct vdo *get_vdo(struct kvdo *kvdo)
{
	return kvdo->vdo;
}

/**
 * Set whether compression is enabled.
 *
 * @param kvdo                The kvdo object
 * @param enable_compression  The new compression mode
 *
 * @return state of compression before new value is set
 **/
bool set_kvdo_compressing(struct kvdo *kvdo, bool enable_compression);

/**
 * Get the current compression mode
 *
 * @param kvdo          The kvdo object to be queried
 *
 * @return whether compression is currently enabled
 */
bool get_kvdo_compressing(struct kvdo *kvdo);

/**
 * Gets the latest statistics gathered by the base code.
 *
 * @param kvdo  the kvdo object
 * @param stats the statistics struct to fill in
 */
void get_kvdo_statistics(struct kvdo *kvdo,
			 struct vdo_statistics *stats);

/**
 * Get the current write policy
 *
 * @param kvdo          The kvdo to be queried
 *
 * @return  the write policy in effect
 */
write_policy get_kvdo_write_policy(struct kvdo *kvdo);

/**
 * Dump base code status information to the kernel log for debugging.
 *
 * @param kvdo          The kvdo to be examined
 */
void dump_kvdo_status(struct kvdo *kvdo);

/**
 * Request the base code prepare to grow the physical space.
 *
 * @param kvdo            The kvdo to be updated
 * @param physical_count  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdo_prepare_to_grow_physical(struct kvdo *kvdo,
				  block_count_t physical_count);

/**
 * Notify the base code of resized physical storage.
 *
 * @param kvdo            The kvdo to be updated
 * @param physical_count  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdo_resize_physical(struct kvdo *kvdo, block_count_t physical_count);

/**
 * Request the base code prepare to grow the logical space.
 *
 * @param kvdo           The kvdo to be updated
 * @param logical_count  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdo_prepare_to_grow_logical(struct kvdo *kvdo, block_count_t logical_count);

/**
 * Request the base code grow the logical space.
 *
 * @param kvdo           The kvdo to be updated
 * @param logical_count  The new size
 *
 * @return VDO_SUCCESS or error
 */
int kvdo_resize_logical(struct kvdo *kvdo, block_count_t logical_count);

/**
 * Request the base code go read-only.
 *
 * @param kvdo          The kvdo to be updated
 * @param result        The error code causing the read only
 */
void set_kvdo_read_only(struct kvdo *kvdo, int result);

/**
 * Perform an extended base-code command
 *
 * @param kvdo          The kvdo upon which to perform the operation.
 * @param argc          The number of arguments to the command.
 * @param argv          The command arguments. Note that all extended
 *                        command argv[0] strings start with "x-".
 *
 * @return VDO_SUCCESS or an error code
 **/
int perform_kvdo_extended_command(struct kvdo *kvdo, int argc, char **argv);

/**
 * Enqueue a work item to be processed in the base code context.
 *
 * @param kvdo          The kvdo object in which to run the work item
 * @param item          The work item to be run
 * @param thread_id     The thread on which to run the work item
 **/
void enqueue_kvdo_work(struct kvdo *kvdo,
		       struct kvdo_work_item *item,
		       thread_id_t thread_id);

/**
 * Set up and enqueue a VIO's work item to be processed in the base code
 * context.
 *
 * @param kvio            The VIO with the work item to be run
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, mapping to a relative priority
 **/
void enqueue_kvio(struct kvio *kvio, KvdoWorkFunction work,
		  void *stats_function, unsigned int action);

/**
 * Enqueue an arbitrary completion for execution on its indicated thread.
 *
 * @param completion  The completion to enqueue
 **/
void kvdo_enqueue(struct vdo_completion *completion);

#endif // KERNEL_VDO_H
