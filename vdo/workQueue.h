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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueue.h#31 $
 */

#ifndef VDO_WORK_QUEUE_H
#define VDO_WORK_QUEUE_H

#include <linux/sched.h> /* for TASK_COMM_LEN */

#include "util/funnelQueue.h"

#include "kernelTypes.h"
#include "types.h"

enum {
	MAX_VDO_WORK_QUEUE_NAME_LEN = TASK_COMM_LEN,
	/** Number of priority values available */
	VDO_WORK_QUEUE_PRIORITY_COUNT = 4,
};

struct vdo_work_item {
	/** Entry link for lock-free work queue */
	struct funnel_queue_entry work_queue_entry_link;
	/** Function to be called */
	vdo_work_function work;
	/**
	 * An index into the statistics table; filled in by workQueueStats code
	 */
	unsigned int stat_table_index;
	/** The priority of the work to be done */
	enum vdo_work_item_priority priority;
	/**
	 * The work queue in which the item is enqueued, or NULL if not
	 * enqueued.
	 */
	struct vdo_work_queue *my_queue;
	/**
	 * Time of enqueueing, in ns, for recording queue (waiting) time stats
	 */
	uint64_t enqueue_time;
};

/**
 * Static attributes of a work queue that are fixed at compile time
 * for a given call site. (Attributes that may be computed at run time
 * are passed as separate arguments.)
 **/
struct vdo_work_queue_type {
	/** A function to call in the new thread before servicing requests */
	void (*start)(void *);

	/** A function to call in the new thread when shutting down */
	void (*finish)(void *);

	/** The largest priority value used by this queue */
	uint8_t max_priority;
};

/**
 * Create a work queue.
 *
 * <p>If multiple threads are requested, work items will be distributed to them
 * in round-robin fashion.
 *
 * Each queue is associated with a struct vdo_thread which has a single vdo
 * thread id. Regardless of the actual number of queues and threads allocated
 * here, code outside of the queue implementation will treat this as a single
 * zone.
 *
 * @param [in]  thread_name_prefix  The per-device prefix to use in thread names
 * @param [in]  name                The queue name
 * @param [in]  owner               The vdo "thread" correpsonding to this queue
 * @param [in]  type                The work queue type defining the lifecycle
 *                                  functions, priorities, and timeout behavior
 * @param [in]  thread_count        Number of service threads to set up
 * @param [in]  thread_privates     If non-NULL, an array of separate private
 *                                  data pointers, one for each service thread
 * @param [out] queue_ptr           Where to store the queue handle
 *
 * @return VDO_SUCCESS or an error code
 **/
int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct vdo_thread *owner,
		    const struct vdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct vdo_work_queue **queue_ptr);

/**
 * Set up the fields of a work queue item.
 *
 * Before the first setup call (setup_work_item), the work item must
 * have been initialized to all-zero. Resetting a previously-used work
 * item does not require another memset.
 *
 * @param item            The work item to initialize
 * @param work            The function pointer to execute
 * @param priority        The priority of the work to be done
 **/
void setup_work_item(struct vdo_work_item *item,
		     vdo_work_function work,
		     enum vdo_work_item_priority priority);

/**
 * Add a work item to a work queue.
 *
 * If the work item has a timeout that has already passed, the timeout
 * handler function may be invoked at this time.
 *
 * @param queue  The queue handle
 * @param item   The work item to be processed
 **/
void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_work_item *item);

/**
 * Shut down a work queue's worker thread.
 *
 * Alerts the worker thread that it should shut down, and then waits
 * for it to do so.
 *
 * There should not be any new enqueueing of work items done once this
 * function is called.
 *
 * @param queue  The work queue to shut down (may be NULL)
 **/
void finish_work_queue(struct vdo_work_queue *queue);

/**
 * Free a work queue.
 *
 * @param queue  The work queue to free
 **/
void free_work_queue(struct vdo_work_queue *queue);

/**
 * Print work queue state and statistics to the kernel log.
 *
 * @param queue  The work queue to examine
 **/
void dump_work_queue(struct vdo_work_queue *queue);

/**
 * Write to the buffer some info about the work item, for logging.
 * Since the common use case is dumping info about a lot of work items
 * to syslog all at once, the format favors brevity over readability.
 *
 * @param item    The work item
 * @param buffer  The message buffer to fill in
 * @param length  The length of the message buffer
 **/
void dump_work_item_to_buffer(struct vdo_work_item *item,
			      char *buffer,
			      size_t length);

/**
 * Returns the private data for the current thread's work queue.
 *
 * @return The private data pointer, or NULL if none or if the current
 *         thread is not a work queue thread.
 **/
void *get_work_queue_private_data(void);

/**
 * Returns the work queue pointer for the current thread, if any.
 *
 * @return The work queue pointer or NULL
 **/
struct vdo_work_queue *get_current_work_queue(void);

/**
 * Returns the vdo thread that owns the work queue.
 *
 * @param queue  The work queue
 *
 * @return The owner pointer supplied at work queue creation
 **/
struct vdo_thread *get_work_queue_owner(struct vdo_work_queue *queue);

/**
 * Check whether a work queue is of a specified type.
 *
 * @param queue  The queue to check
 * @param type   The desired type
 **/
bool __must_check
vdo_work_queue_type_is(struct vdo_work_queue *queue,
		       const struct vdo_work_queue_type *type);

#endif /* VDO_WORK_QUEUE_H */
