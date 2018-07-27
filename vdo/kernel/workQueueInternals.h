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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/workQueueInternals.h#3 $
 */

#ifndef WORK_QUEUE_INTERNALS_H
#define WORK_QUEUE_INTERNALS_H

#include <linux/completion.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "workItemStats.h"
#include "workQueueStats.h"

typedef struct kvdoWorkItemList {
  KvdoWorkItem *tail;
} KvdoWorkItemList;

/**
 * Work queue definition.
 *
 * There are two types of work queues: simple, with one worker thread, and
 * round-robin, which uses a group of the former to do the work, and assigns
 * work to them in -- you guessed it -- round-robin fashion. Externally, both
 * are represented via the same common sub-structure, though there's actually
 * not a great deal of overlap between the two types internally.
 **/
struct kvdoWorkQueue {
  /** Name of just the work queue (e.g., "cpuQ12") */
  char           *name;
  /**
   * Whether this is a round-robin work queue or a simple (one-thread)
   * work queue.
   **/
  bool            roundRobinMode;
  /** A handle to a sysfs tree for reporting stats and other info */
  struct kobject  kobj;
  /** The kernel layer owning this work queue */
  KernelLayer    *owner;
};

typedef struct simpleWorkQueue     SimpleWorkQueue;
typedef struct roundRobinWorkQueue RoundRobinWorkQueue;

struct simpleWorkQueue {
  /** Common work queue bits */
  KvdoWorkQueue            common;
  /** A copy of .thread->pid, for safety in the sysfs support */
  atomic_t                 threadID;
  /**
   * Number of priorities actually used, so we don't keep re-checking unused
   * funnel queues.
   **/
  unsigned int             numPriorityLists;
  /**
   * Map from action codes to priorities.
   *
   * This mapping can be changed at run time in internal builds, for tuning
   * purposes.
   **/
  uint8_t                  priorityMap[WORK_QUEUE_ACTION_COUNT];
  /** The funnel queues */
  FunnelQueue             *priorityLists[WORK_QUEUE_PRIORITY_COUNT];
  /** The kernel thread */
  struct task_struct      *thread;
  /** Life cycle functions, etc */
  const KvdoWorkQueueType *type;
  /** Opaque private data pointer, defined by higher level code */
  void                    *private;
  /** In a subordinate work queue, a link back to the round-robin parent */
  KvdoWorkQueue           *parentQueue;
  /** Padding for cache line separation */
  char                     pad[CACHE_LINE_BYTES - sizeof(KvdoWorkQueue *)];
  /** Lock protecting delayedItems, priorityMap, numPriorityLists, started */
  spinlock_t               lock;
  /** Any worker threads (zero or one) waiting for new work to do */
  wait_queue_head_t        waitingWorkerThreads;
  /**
   * Hack to reduce wakeup calls if the worker thread is running. See comments
   * in workQueue.c.
   *
   * There is a lot of redundancy with "firstWakeup", though, and the pair
   * should be re-examined.
   **/
  atomic_t                 idle;
  /** Wait list for synchronization during worker thread startup */
  wait_queue_head_t        startWaiters;
  /** Worker thread status (boolean) */
  bool                     started;

  /** List of delayed work items; usually only one, if any */
  KvdoWorkItemList         delayedItems;
  /**
   * Timer for pulling delayed work items off their list and submitting them to
   * run.
   *
   * If the spinlock "lock" above is not held, this timer is scheduled (or
   * currently firing and the callback about to acquire the lock) iff
   * delayedItems is nonempty.
   **/
  struct timer_list        delayedItemsTimer;

  /**
   * Timestamp (ns) from the submitting thread that decided to wake us up; also
   * used as a flag to indicate whether a wakeup is needed.
   *
   * Written by submitting threads with atomic64_cmpxchg, and by the worker
   * thread setting to 0.
   *
   * If the value is 0, the worker is probably asleep; the submitting thread
   * stores a non-zero value and becomes responsible for calling wake_up on the
   * worker thread. If the value is non-zero, either the worker is running or
   * another thread has the responsibility for issuing the wakeup.
   *
   * The "sleep" mode has periodic wakeups and the worker thread may happen to
   * wake up while a work item is being enqueued. If that happens, the wakeup
   * may be unneeded but will be attempted anyway.
   *
   * So the return value from cmpxchg(firstWakeup,0,nonzero) can always be
   * done, and will tell the submitting thread whether to issue the wakeup or
   * not; cmpxchg is atomic, so no other synchronization is needed.
   *
   * A timestamp is used rather than, say, 1, so that the worker thread can
   * record stats on how long it takes to actually get the worker thread
   * running.
   *
   * There is some redundancy between this and "idle" above.
   **/
  atomic64_t               firstWakeup;
  /** Padding for cache line separation */
  char                     pad2[CACHE_LINE_BYTES - sizeof(atomic64_t)];
  /** Scheduling and work-function statistics */
  KvdoWorkQueueStats       stats;
  /** Last time (ns) the scheduler actually woke us up */
  uint64_t                 mostRecentWakeup;
};

struct roundRobinWorkQueue {
  /** Common work queue bits */
  KvdoWorkQueue     common;
  /** Simple work queues, for actually getting stuff done */
  SimpleWorkQueue **serviceQueues;
  /** Number of subordinate work queues */
  unsigned int      numServiceQueues;
  /** Padding for cache line separation */
  char              pad[CACHE_LINE_BYTES - sizeof(unsigned int)];
  /**
   * Rotor used for dispatching across subordinate service queues.
   *
   * Used and updated by submitting threads. (Not atomically or with locking,
   * because we don't really care about it being precise, only about getting a
   * roughly even spread; if an increment is missed here and there, it's not a
   * problem.)
   **/
  unsigned int      serviceQueueRotor;
};

static inline SimpleWorkQueue *asSimpleWorkQueue(KvdoWorkQueue *queue)
{
  return ((queue == NULL)
          ? NULL
          : container_of(queue, SimpleWorkQueue, common));
}

static inline const SimpleWorkQueue *
asConstSimpleWorkQueue(const KvdoWorkQueue *queue)
{
  return ((queue == NULL)
          ? NULL
          : container_of(queue, SimpleWorkQueue, common));
}

static inline RoundRobinWorkQueue *asRoundRobinWorkQueue(KvdoWorkQueue *queue)
{
  return ((queue == NULL)
          ? NULL
          : container_of(queue, RoundRobinWorkQueue, common));
}

#endif // WORK_QUEUE_INTERNALS_H
