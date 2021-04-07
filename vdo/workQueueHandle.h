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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueHandle.h#11 $
 */

#ifndef WORK_QUEUE_HANDLE_H
#define WORK_QUEUE_HANDLE_H

#include <linux/sched/task_stack.h>

#include "workQueueInternals.h"

/*
 * Layout of a special structure stored at a consistent place on the
 * stack in work queue threads.
 */
struct work_queue_stack_handle {
	unsigned long nonce;
	struct simple_work_queue *queue;
};

struct work_queue_stack_handle_globals {
	/*
	 * Location in the stack, relative to the task structure which is
	 * contained in the same memory allocation.
	 */
	long offset;
	/*
	 * A lock is used to guard against multiple updaters, but once an
	 * update is done, the offset variable will be read-only.
	 */
	spinlock_t offset_lock;
	/*
	 * A nonce chosen differently each time the module is loaded, used
	 * as a marker so we can check that the current thread really is a
	 * work queue thread. Set at module initialization time, before any
	 * work queues are created.
	 */
	unsigned long nonce;
};

extern struct work_queue_stack_handle_globals work_queue_stack_handle_globals;

/**
 * Initialize a stack handle associated with a work queue.
 *
 * @param [out] handle  The handle to be initialized
 * @param [in]  queue   The work queue pointer
 **/
void initialize_work_queue_stack_handle(struct work_queue_stack_handle *handle,
					struct simple_work_queue *queue);

/**
 * Return the work queue pointer recorded at initialization time in
 * the work-queue stack handle initialized on the stack of the current
 * thread, if any.
 *
 * @return the work queue pointer, or NULL
 **/
static inline __no_sanitize_address
struct simple_work_queue *get_current_thread_work_queue(void)
{
	struct work_queue_stack_handle *handle =
		(struct work_queue_stack_handle *)
			(task_stack_page(current) +
			 work_queue_stack_handle_globals.offset);
	if (likely(handle->nonce == work_queue_stack_handle_globals.nonce)) {
		return handle->queue;
	} else {
		return NULL;
	}
}

/**
 * Initialize the global state used by the work-queue stack-handle
 * code.
 **/
void init_work_queue_stack_handle_once(void);

#endif // WORK_QUEUE_HANDLE_H
