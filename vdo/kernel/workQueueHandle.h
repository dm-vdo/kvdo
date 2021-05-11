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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/workQueueHandle.h#1 $
 */

#ifndef WORK_QUEUE_HANDLE_H
#define WORK_QUEUE_HANDLE_H

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
#include <linux/sched/task_stack.h>
#else
#include <linux/sched.h>
#endif

#include "workQueueInternals.h"

/*
 * Layout of a special structure stored at a consistent place on the
 * stack in work queue threads.
 */
typedef struct workQueueStackHandle {
  unsigned long    nonce;
  SimpleWorkQueue *queue;
} WorkQueueStackHandle;

typedef struct workQueueStackHandleGlobals {
  /*
   * Location in the stack, relative to the task structure which is
   * contained in the same memory allocation.
   */
  long          offset;
  /*
   * A lock is used to guard against multiple updaters, but once an
   * update is done, the offset variable will be read-only.
   */
  spinlock_t    offsetLock;
  /*
   * A nonce chosen differently each time the module is loaded, used
   * as a marker so we can check that the current thread really is a
   * work queue thread. Set at module initialization time, before any
   * work queues are created.
   */
  unsigned long nonce;
} WorkQueueStackHandleGlobals;

extern WorkQueueStackHandleGlobals workQueueStackHandleGlobals;

/**
 * Initialize a stack handle associated with a work queue.
 *
 * @param [out] handle  The handle to be initialized
 * @param [in]  queue   The work queue pointer
 **/
void initializeWorkQueueStackHandle(WorkQueueStackHandle *handle,
                                    SimpleWorkQueue      *queue);

/**
 * Return the work queue pointer recorded at initialization time in
 * the work-queue stack handle initialized on the stack of the current
 * thread, if any.
 *
 * @return   the work queue pointer, or NULL
 **/
static inline SimpleWorkQueue *getCurrentThreadWorkQueue(void)
{
  WorkQueueStackHandle *handle
    = (WorkQueueStackHandle *)(task_stack_page(current)
                               + workQueueStackHandleGlobals.offset);
  if (likely(handle->nonce == workQueueStackHandleGlobals.nonce)) {
    return handle->queue;
  } else {
    return NULL;
  }
}

/**
 * Initialize the global state used by the work-queue stack-handle
 * code.
 **/
void initWorkQueueStackHandleOnce(void);

#endif // WORK_QUEUE_HANDLE_H
