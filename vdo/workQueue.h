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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueue.h#27 $
 */

#ifndef VDO_WORK_QUEUE_H
#define VDO_WORK_QUEUE_H

#include <linux/kobject.h>
#include <linux/sched.h> /* for TASK_COMM_LEN */

#include "kernelTypes.h"
#include "util/funnelQueue.h"

enum {
	MAX_VDO_WORK_QUEUE_NAME_LEN = TASK_COMM_LEN,
	/** Maximum number of action definitions per work queue type */
	VDO_WORK_QUEUE_ACTION_COUNT = 8,
	/** Number of priority values available */
	VDO_WORK_QUEUE_PRIORITY_COUNT = 4,
};

struct vdo_work_item {
	/** Entry link for lock-free work queue */
	struct funnel_queue_entry work_queue_entry_link;
	/** Function to be called */
	vdo_work_function work;
	/** Optional alternate function for display in queue stats */
	void *stats_function;
	/**
	 * An index into the statistics table; filled in by workQueueStats code
	 */
	unsigned int stat_table_index;
	/**
	 * The action code given to setup_work_item, from which a priority will
	 * be determined.
	 **/
	unsigned int action;
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
 * Table entries defining an action.
 *
 * Actions are intended to distinguish general classes of activity for
 * prioritization purposes, but not necessarily to indicate specific work
 * functions. They are indicated to setup_work_item numerically, using an
 * enumerator defined per kind of work queue -- bio submission work queue
 * actions use bio_q_action, cpu actions use cpu_q_action, etc. For example,
 * for the CPU work queues, data compression can be prioritized separately
 * from final cleanup processing of a vio or from dedupe verification; base
 * code threads prioritize all VIO callback invocation the same, but separate
 * from sync or heartbeat operations. The bio acknowledgement work queue, on
 * the other hand, only does one thing, so it only defines one action code.
 *
 * Action codes values must be small integers, 0 through
 * VDO_WORK_QUEUE_ACTION_COUNT-1, and should not be duplicated for a queue
 * type.
 *
 * A table of vdo_work_queue_action entries embedded in struct
 * vdo_work_queue_type specifies the name, code, and priority for each type
 * of action in the work queue. The table can have at most
 * VDO_WORK_QUEUE_ACTION_COUNT entries, but a NULL name indicates an earlier
 * end to the table.
 *
 * Priorities may be specified as values from 0 through
 * VDO_WORK_QUEUE_PRIORITY_COUNT-1, higher values indicating higher priority.
 * Priorities are just strong suggestions; it's possible for a lower-priority
 * work item scheduled right after a high-priority one to be run first, if the
 * worker thread happens to be scanning its queues at just the wrong moment,
 * but the high-priority item will be picked up next.
 *
 * Internally, the priorities in this table are used to initialize another
 * table in the constructed work queue object, and in internal builds,
 * device-mapper messages can be sent to change the priority for an action,
 * identified by name, in a running VDO device. Doing so does not affect the
 * priorities for other devices, or for future VDO device creation.
 **/
struct vdo_work_queue_action {
	/** Name of the action */
	char *name;

	/** The action code (per-type enum) */
	unsigned int code;

	/** The initial priority for this action */
	unsigned int priority;
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

	/** Table of actions for this work queue */
	struct vdo_work_queue_action action_table[VDO_WORK_QUEUE_ACTION_COUNT];
};

/**
 * Create a work queue.
 *
 * If multiple threads are requested, work items will be distributed to them in
 * round-robin fashion.
 *
 * @param [in]  thread_name_prefix  The per-device prefix to use in thread
 *                                  names
 * @param [in]  name                The queue name
 * @param [in]  parent_kobject      The parent sysfs node
 * @param [in]  owner               The VDO owning the work queue
 * @param [in]  private             Private data of the queue for use by work
 *                                  items or other queue-specific functions
 * @param [in]  thread_privates     If non-NULL, an array of separate private
 *                                  data pointers, one for each service thread,
 *                                  to use instead of sharing 'private'
 * @param [in]  type                The work queue type defining the lifecycle
 *                                  functions, queue actions, priorities, and
 *                                  timeout behavior
 * @param [in]  thread_count        Number of service threads to set up
 * @param [out] queue_ptr           Where to store the queue handle
 *
 * @return VDO_SUCCESS or an error code
 **/
int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct kobject *parent_kobject,
		    struct vdo *owner,
		    void *private,
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
 * The action code is typically defined in a work-queue-type-specific
 * enumeration; see the description of struct vdo_work_queue_action.
 *
 * @param item            The work item to initialize
 * @param work            The function pointer to execute
 * @param stats_function  A function pointer to record for stats, or NULL
 * @param action          Action code, for determination of priority
 **/
void setup_work_item(struct vdo_work_item *item,
		     vdo_work_function work,
		     void *stats_function,
		     unsigned int action);

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
 * Checks whether two work items have the same action codes
 *
 * @param item1  The first item
 * @param item2  The second item
 *
 * @return TRUE if the actions are the same, FALSE otherwise
 */
static inline bool are_work_item_actions_equal(struct vdo_work_item *item1,
					       struct vdo_work_item *item2)
{
	return item1->action == item2->action;
}

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
 * Returns the VDO that owns the work queue.
 *
 * @param queue  The work queue
 *
 * @return The owner pointer supplied at work queue creation
 **/
struct vdo *get_work_queue_owner(struct vdo_work_queue *queue);

#endif /* VDO_WORK_QUEUE_H */
