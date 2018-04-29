/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/workQueue.h#1 $
 */

#ifndef ALBIREO_WORK_QUEUE_H
#define ALBIREO_WORK_QUEUE_H

#include <linux/kobject.h>
#include <linux/sched.h>        /* for TASK_COMM_LEN */

#include "funnelQueue.h"
#include "kernelTypes.h"

enum {
  MAX_QUEUE_NAME_LEN        = TASK_COMM_LEN,
  /** Maximum number of action definitions per work queue type */
  WORK_QUEUE_ACTION_COUNT   = 8,
  /** Number of priority values available */
  WORK_QUEUE_PRIORITY_COUNT = 4,
};

struct kvdoWorkItem {
  /** Entry link for lock-free work queue */
  FunnelQueueEntry  workQueueEntryLink;
  /** Function to be called */
  KvdoWorkFunction  work;
  /** Optional alternate function for display in queue stats */
  void             *statsFunction;
  /** Optional alternate function to be invoked on timeout */
  KvdoWorkFunction  timeoutWork;
  /** The point at which to time out (absolute time, in jiffies) */
  Jiffies           timeout;
  /** An index into the statistics table; filled in by workQueueStats code */
  unsigned int      statTableIndex;
  /**
   * The action code given to setupWorkItem, from which a priority will be
   * determined.
   **/
  unsigned int      action;
  /** The work queue in which the item is enqueued, or NULL if not enqueued. */
  KvdoWorkQueue    *myQueue;
  /**
   * Time at which to execute in jiffies for a delayed work item, or zero to
   * queue for execution ASAP.
   **/
  Jiffies           executionTime;
  /** List management for delayed or expired work items */
  KvdoWorkItem     *next;
  /** Time of enqueueing, in ns, for recording queue (waiting) time stats */
  uint64_t          enqueueTime;
};

/**
 * Table entries defining an action.
 *
 * Actions are intended to distinguish general classes of activity for
 * prioritization purposes, but not necessarily to indicate specific work
 * functions. They are indicated to setupWorkItem numerically, using an
 * enumerator defined per kind of work queue -- bio submission work queue
 * actions use BioQAction, cpu actions use CPUQAction, etc. For example, for
 * the CPU work queues, data compression can be prioritized separately from
 * final cleanup processing of a KVIO or from dedupe verification; base code
 * threads prioritize all VIO callback invocation the same, but separate from
 * sync or heartbeat operations. The bio acknowledgement work queue, on the
 * other hand, only does one thing, so it only defines one action code.
 *
 * Action codes values must be small integers, 0 through
 * WORK_QUEUE_ACTION_COUNT-1, and should not be duplicated for a queue type.
 *
 * A table of KvdoWorkQueueAction entries embedded in KvdoWorkQueueType
 * specifies the name, code, and priority for each type of action in the work
 * queue. The table can have at most WORK_QUEUE_ACTION_COUNT entries, but a
 * NULL name indicates an earlier end to the table.
 *
 * Priorities may be specified as values from 0 through
 * WORK_QUEUE_PRIORITY_COUNT-1, higher values indicating higher priority.
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
typedef struct kvdoWorkQueueAction {
  /** Name of the action */
  char         *name;

  /** The action code (per-type enum) */
  unsigned int  code;

  /** The initial priority for this action */
  unsigned int  priority;
} KvdoWorkQueueAction;

typedef void (*KvdoWorkQueueFunction)(void *);

/**
 * Static attributes of a work queue that are fixed at compile time
 * for a given call site. (Attributes that may be computed at run time
 * are passed as separate arguments.)
 **/
typedef struct kvdoWorkQueueType {
  /** A function to call in the new thread before servicing requests */
  KvdoWorkQueueFunction start;

  /** A function to call in the new thread when shutting down */
  KvdoWorkQueueFunction finish;

  /** A function to call in the new thread after running out of work */
  KvdoWorkQueueFunction suspend;

  /** Whether this work queue may get work items with timeouts associated */
  bool                  needTimeouts;

  /** Table of actions for this work queue */
  KvdoWorkQueueAction   actionTable[WORK_QUEUE_ACTION_COUNT];
} KvdoWorkQueueType;

/**
 * Create a work queue.
 *
 * If multiple threads are requested, work items will be distributed to them in
 * round-robin fashion.
 *
 * @param [in]  threadNamePrefix The per-device prefix to use in thread names
 * @param [in]  name             The queue name
 * @param [in]  parentKobject    The parent sysfs node
 * @param [in]  owner            The kernel layer owning the work queue
 * @param [in]  private          Private data of the queue for use by work
 *                               items or other queue-specific functions
 * @param [in]  type             The work queue type defining the lifecycle
 *                               functions, queue actions, priorities, and
 *                               timeout behavior
 * @param [in]  threadCount      Number of service threads to set up
 * @param [out] queuePtr         Where to store the queue handle
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeWorkQueue(const char               *threadNamePrefix,
                  const char               *name,
                  struct kobject           *parentKobject,
                  KernelLayer              *owner,
                  void                     *private,
                  const KvdoWorkQueueType  *type,
                  unsigned int              threadCount,
                  KvdoWorkQueue           **queuePtr);

/**
 * Set up the fields of a work queue item.
 *
 * Before the first setup call (setupWorkItem or setupWorkItemWithTimeout), the
 * work item must have been initialized to all-zero. Resetting a
 * previously-used work item does not require another memset.
 *
 * The action code is typically defined in a work-queue-type-specific
 * enumeration; see the description of KvdoWorkQueueAction.
 *
 * @param item           The work item to initialize
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, for determination of priority
 **/
void setupWorkItem(KvdoWorkItem     *item,
                   KvdoWorkFunction  work,
                   void             *statsFunction,
                   unsigned int      action);

/**
 * Set up the fields of a work queue item, as for setupWorkItem, but with a
 * timeout handler.
 *
 * The timeout handler runs in a kernel timer context, and should do as little
 * work as possible. Currently it sometimes runs with the work queue locked, so
 * it must not attempt to re-queue the work item on the same queue.
 *
 * @param item           The work item to initialize
 * @param work           The function pointer to execute
 * @param statsFunction  A function pointer to record for stats, or NULL
 * @param action         Action code, for determination of priority
 * @param timeoutWork    The function to invoke on timeout
 * @param timeout        The time at which to time out (absolute, in jiffies)
 **/
void setupWorkItemWithTimeout(KvdoWorkItem     *item,
                              KvdoWorkFunction  work,
                              void             *statsFunction,
                              unsigned int      action,
                              KvdoWorkFunction  timeoutWork,
                              Jiffies           timeout);

/**
 * Add a work item to a work queue.
 *
 * If the work item has a timeout that has already passed, the timeout
 * handler function may be invoked at this time.
 *
 * @param queue      The queue handle
 * @param item       The work item to be processed
 **/
void enqueueWorkQueue(KvdoWorkQueue *queue, KvdoWorkItem *item);

/**
 * Add a work item to a work queue, to be run at a later point in time.
 *
 * Currently delayed work items are used only in a very limited fashion -- at
 * most one at a time for any of the work queue types that use them -- and some
 * shortcuts have been taken that assume that that's the case. Multiple delayed
 * work items should work, but they will execute in the order they were
 * enqueued.
 *
 * @param queue           The queue handle
 * @param item            The work item to be processed
 * @param executionTime   When to run the work item (jiffies)
 **/
void enqueueWorkQueueDelayed(KvdoWorkQueue *queue,
                             KvdoWorkItem  *item,
                             Jiffies        executionTime);

/**
 * Shut down a work queue's worker thread.
 *
 * Alerts the worker thread that it should shut down, and then waits
 * for it to do so.
 *
 * There should not be any new enqueueing of work items done once this
 * function is called. Any pending delayed work items will be
 * processed, as scheduled, before the worker thread shuts down, but
 * they must not re-queue themselves to run again.
 *
 * @param queue  The work queue to shut down
 **/
void finishWorkQueue(KvdoWorkQueue *queue);

/**
 * Free a work queue and null out the reference to it.
 *
 * @param queuePtr  Where the queue handle is found
 **/
void freeWorkQueue(KvdoWorkQueue **queuePtr);

/**
 * Print work queue state and statistics to the kernel log.
 *
 * @param queue  The work queue to examine
 **/
void dumpWorkQueue(KvdoWorkQueue *queue);

/**
 * Write to the buffer some info about the work item, for logging.
 * Since the common use case is dumping info about a lot of work items
 * to syslog all at once, the format favors brevity over readability.
 *
 * @param item    The work item
 * @param buffer  The message buffer to fill in
 * @param length  The length of the message buffer
 **/
void dumpWorkItemToBuffer(KvdoWorkItem *item, char *buffer, size_t length);


/**
 * Initialize work queue internals at module load time.
 **/
void initWorkQueueOnce(void);

/**
 * Checks whether two work items have the same action codes
 *
 * @param item1 The first item
 * @param item2 The second item
 *
 * @return TRUE if the actions are the same, FALSE otherwise
 */
static inline bool areWorkItemActionsEqual(KvdoWorkItem *item1,
                                           KvdoWorkItem *item2)
{
  return item1->action == item2->action;
}

/**
 * Returns the private data for the current thread's work queue.
 *
 * @return  The private data pointer, or NULL if none or if the current
 *          thread is not a work queue thread.
 **/
void *getWorkQueuePrivateData(void);

/**
 * Updates the private data pointer for the current thread's work queue.
 *
 * @param newData  The new private data pointer
 **/
void setWorkQueuePrivateData(void *newData);

/**
 * Returns the work queue pointer for the current thread, if any.
 *
 * @return   The work queue pointer or NULL
 **/
KvdoWorkQueue *getCurrentWorkQueue(void);

/**
 * Returns the kernel layer that owns the work queue.
 *
 * @param queue  The work queue
 *
 * @return   The owner pointer supplied at work queue creation
 **/
KernelLayer *getWorkQueueOwner(KvdoWorkQueue *queue);

#endif /* ALBIREO_WORK_QUEUE_H */
