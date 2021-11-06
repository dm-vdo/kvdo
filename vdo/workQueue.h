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

#ifndef VDO_WORK_QUEUE_H
#define VDO_WORK_QUEUE_H

#include <linux/sched.h> /* for TASK_COMM_LEN */

#include "funnelQueue.h"

#include "kernel-types.h"
#include "types.h"

enum {
	MAX_VDO_WORK_QUEUE_NAME_LEN = TASK_COMM_LEN,
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
	enum vdo_work_item_priority max_priority;

	/** The default priority for this queue */
	enum vdo_work_item_priority default_priority;
};

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

void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_work_item *item);

void finish_work_queue(struct vdo_work_queue *queue);

void free_work_queue(struct vdo_work_queue *queue);

void dump_work_queue(struct vdo_work_queue *queue);

void dump_work_item_to_buffer(struct vdo_work_item *item,
			      char *buffer,
			      size_t length);

void *get_work_queue_private_data(void);

struct vdo_work_queue *get_current_work_queue(void);

struct vdo_thread *get_work_queue_owner(struct vdo_work_queue *queue);

bool __must_check
vdo_work_queue_type_is(struct vdo_work_queue *queue,
		       const struct vdo_work_queue_type *type);

#endif /* VDO_WORK_QUEUE_H */
