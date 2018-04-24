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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/workQueue.c#3 $
 */

#include "workQueue.h"

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/version.h>

#include "atomic.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"

#include "numeric.h"
#include "workItemStats.h"
#include "workQueueHandle.h"
#include "workQueueInternals.h"
#include "workQueueStats.h"
#include "workQueueSysfs.h"

enum {
  // Time between work queue heartbeats in usec. The default kernel
  // configurations generally have 1ms or 4ms tick rates, so let's make this a
  // multiple for accuracy.
  FUNNEL_HEARTBEAT_INTERVAL = 4000,

  // Time to wait for a work queue to flush remaining items during shutdown.
  // Specified in milliseconds.
  FUNNEL_FINISH_SLEEP = 5000,
};

static struct mutex queueDataLock;
static SimpleWorkQueue queueData;

static void freeSimpleWorkQueue(SimpleWorkQueue *queue);
static void finishSimpleWorkQueue(SimpleWorkQueue *queue);

// work item lists (used for delayed work items)

/**********************************************************************/
static void initializeWorkItemList(KvdoWorkItemList *list)
{
  list->tail = NULL;
}

/**********************************************************************/
static void addToWorkItemList(KvdoWorkItemList *list, KvdoWorkItem *item)
{
  if (list->tail == NULL) {
    item->next = item;
  } else {
    KvdoWorkItem *head = list->tail->next;
    list->tail->next = item;
    item->next = head;
  }
  list->tail = item;
}

/**********************************************************************/
static bool isWorkItemListEmpty(KvdoWorkItemList *list)
{
  return list->tail == NULL;
}

/**********************************************************************/
static KvdoWorkItem *workItemListPoll(KvdoWorkItemList *list)
{
  KvdoWorkItem *tail = list->tail;
  if (tail == NULL) {
    return NULL;
  }
  // Extract and return head of list.
  KvdoWorkItem *head = tail->next;
  // Only one entry?
  if (head == tail) {
    list->tail = NULL;
  } else {
    tail->next = head->next;
  }
  head->next = NULL;
  return head;
}

/**********************************************************************/
static KvdoWorkItem *workItemListPeek(KvdoWorkItemList *list)
{
  KvdoWorkItem *tail = list->tail;
  return tail ? tail->next : NULL;
}

// Finding the SimpleWorkQueue to actually operate on.

/**
 * Pick the next subordinate service queue in rotation.
 *
 * This doesn't need to be 100% precise in distributing work items around, so
 * playing loose with concurrent field modifications isn't going to hurt us.
 * (Avoiding the atomic ops may help us a bit in performance, but we'll still
 * have contention over the fields.)
 *
 * @param queue  The round-robin-type work queue
 *
 * @return  A subordinate work queue
 **/
static inline SimpleWorkQueue *nextServiceQueue(RoundRobinWorkQueue *queue)
{
  unsigned int index = (queue->serviceQueueRotor++ % queue->numServiceQueues);
  return queue->serviceQueues[index];
}

/**
 * Find a simple work queue on which to operate.
 *
 * If the argument is already a simple work queue, use it. If it's a
 * round-robin work queue, pick the next subordinate service queue and use it.
 *
 * @param queue  a work queue (round-robin or simple)
 *
 * @return  a simple work queue
 **/
static inline SimpleWorkQueue *pickSimpleQueue(KvdoWorkQueue *queue)
{
  return (queue->roundRobinMode
          ? nextServiceQueue(asRoundRobinWorkQueue(queue))
          : asSimpleWorkQueue(queue));
}

// Processing normal work items.

/**
 * Scan the work queue's work item lists, and dequeue and return the next
 * waiting work item, if any.
 *
 * We scan the funnel queues from highest priority to lowest, once; there is
 * therefore a race condition where a high-priority work item can be enqueued
 * followed by a lower-priority one, and we'll grab the latter (but we'll catch
 * the high-priority item on the next call). If strict enforcement of
 * priorities becomes necessary, this function will need fixing.
 *
 * @param queue  the work queue
 *
 * @return  a work item pointer, or NULL
 **/
static KvdoWorkItem *pollForWorkItem(SimpleWorkQueue *queue)
{
  KvdoWorkItem *item = NULL;
  for (int i = queue->numPriorityLists - 1; i >= 0; i--) {
    FunnelQueueEntry *link = funnelQueuePoll(queue->priorityLists[i]);
    if (link != NULL) {
      item = container_of(link, KvdoWorkItem, workQueueEntryLink);
      break;
    }
  }

  return item;
}

/**
 * Add a work item into the queue, and inform the caller of any additional
 * processing necessary.
 *
 * If the worker thread may not be awake, true is returned, and the caller
 * should attempt a wakeup.
 *
 * @param queue  The work queue
 * @param item   The work item to add
 *
 * @return  true iff the caller should wake the worker thread
 **/
__attribute__((warn_unused_result))
static bool enqueueWorkQueueItem(SimpleWorkQueue *queue, KvdoWorkItem *item)
{
  ASSERT_LOG_ONLY(item->myQueue == NULL,
                  "item %p (fn %p/%p) to enqueue (%p) is not already queued "
                  "(%p)", item, item->work, item->statsFunction, queue,
                  item->myQueue);
  if (ASSERT(item->action < WORK_QUEUE_ACTION_COUNT,
             "action is in range for queue") != VDO_SUCCESS) {
    item->action = 0;
  }
  unsigned int priority = queue->priorityMap[item->action];

  // Update statistics.
  updateStatsForEnqueue(&queue->stats, item, priority);

  item->myQueue = &queue->common;

  // Funnel queue handles the synchronization for the put.
  funnelQueuePut(queue->priorityLists[priority], &item->workQueueEntryLink);

  /*
   * Due to how funnel-queue synchronization is handled (just atomic
   * operations), the simplest safe implementation here would be to wake-up any
   * waiting threads after enqueueing each item. Even if the funnel queue is
   * not empty at the time of adding an item to the queue, the consumer thread
   * may not see this since it is not guaranteed to have the same view of the
   * queue as a producer thread.
   *
   * However, the above is wasteful so instead we attempt to minimize the
   * number of thread wakeups. This is normally unsafe due to the above
   * consumer-producer synchronization constraints. To correct this a timeout
   * mechanism is used to wake the thread periodically to handle the occasional
   * race condition that triggers and results in this thread not being woken
   * properly.
   *
   * In most cases, the above timeout will not occur prior to some other work
   * item being added after the queue is set to idle state, so thread wakeups
   * will generally be triggered much faster than this interval. The timeout
   * provides protection against the cases where more work items are either not
   * added or are added too infrequently.
   *
   * This is also why we can get away with the normally-unsafe optimization for
   * the common case by checking queue->idle first without synchronization. The
   * race condition exists, but another work item getting enqueued can wake us
   * up, and if we don't get that either, we still have the timeout to fall
   * back on.
   */
  return ((atomic_read(&queue->idle) == 1)
          && (atomic_cmpxchg(&queue->idle, 1, 0) == 1));
}

/**
 * Compute an approximate indication of the number of pending work items.
 *
 * No synchronization is used, so it's guaranteed to be correct only if there
 * is no activity.
 *
 * @param queue  The work queue to examine
 *
 * @return  the estimate of the number of pending work items
 **/
static unsigned int getPendingCount(SimpleWorkQueue *queue)
{
  KvdoWorkItemStats *stats = &queue->stats.workItemStats;
  long long pending = 0;
  for (int i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
    pending += atomic64_read(&stats->enqueued[i]);
    pending -= stats->times[i].count;
  }
  if (pending < 0) {
    /*
     * If we fetched numbers that were changing, we can get negative results.
     * Just return an indication that there's some activity.
     */
    pending = 1;
  }
  return pending;
}

/**
 * Run any start hook that may be defined for the work queue.
 *
 * @param queue  The work queue
 **/
static void runStartHook(SimpleWorkQueue *queue)
{
  if (queue->type->start != NULL) {
    queue->type->start(queue->private);
  }
}

/**
 * Run any finish hook that may be defined for the work queue.
 *
 * @param queue  The work queue
 **/
static void runFinishHook(SimpleWorkQueue *queue)
{
  if (queue->type->finish != NULL) {
    queue->type->finish(queue->private);
  }
}

/**
 * If the work queue has a suspend hook, invoke it, and when it finishes, check
 * again for any pending work items.
 *
 * We assume a check for pending work items has just been done and turned up
 * empty; so, if no suspend hook exists, we can just return NULL without doing
 * another check.
 *
 * @param [in]     queue  The work queue preparing to suspend
 *
 * @return  the newly found work item, if any
 **/
static KvdoWorkItem *runSuspendHook(SimpleWorkQueue *queue)
{
  if (queue->type->suspend == NULL) {
    return NULL;
  }

  queue->type->suspend(queue->private);
  return pollForWorkItem(queue);
}

/**
 * Check whether a work queue has delayed work items pending.
 *
 * @param queue  The work queue
 *
 * @return true iff delayed work items are pending
 **/
static bool hasDelayedWorkItems(SimpleWorkQueue *queue)
{
  bool result;
  unsigned long flags;
  spin_lock_irqsave(&queue->lock, flags);
  result = !isWorkItemListEmpty(&queue->delayedItems);
  spin_unlock_irqrestore(&queue->lock, flags);
  return result;
}

/**
 * Wait for the next work item to process, or until kthread_should_stop
 * indicates that it's time for us to shut down.
 *
 * If kthread_should_stop says it's time to stop but we have pending work
 * items, return a work item.
 *
 * Update statistics relating to scheduler interactions.
 *
 * @param [in]     queue            The work queue to wait on
 * @param [in]     timeoutInterval  How long to wait each iteration
 *
 * @return  the next work item, or NULL to indicate shutdown is requested
 **/
static KvdoWorkItem *waitForNextWorkItem(SimpleWorkQueue *queue,
                                         TimeoutJiffies   timeoutInterval)
{
  KvdoWorkItem *item = runSuspendHook(queue);
  if (item != NULL) {
    return item;
  }

  DEFINE_WAIT(wait);
  while (true) {
    atomic64_set(&queue->firstWakeup, 0);
    prepare_to_wait(&queue->waitingWorkerThreads, &wait, TASK_INTERRUPTIBLE);
    /*
     * Don't set the idle flag until a wakeup will not be lost.
     *
     * Force synchronization between setting the idle flag and checking the
     * funnel queue; the producer side will do them in the reverse order.
     * (There's still a race condition we've chosen to allow, because we've got
     * a timeout below that unwedges us if we hit it, but this may narrow the
     * window a little.)
     */
    atomic_set(&queue->idle, 1);
    memoryFence(); // store-load barrier between "idle" and funnel queue

    item = pollForWorkItem(queue);
    if (item != NULL) {
      break;
    }

    /*
     * We need to check for thread-stop after setting TASK_INTERRUPTIBLE state
     * up above. Otherwise, schedule() will put the thread to sleep and might
     * miss a wakeup from kthread_stop() call in finishWorkQueue().
     *
     * If there are delayed work items, we need to wait for them to
     * get run. Then, when we check kthread_should_stop again, we'll
     * finally exit.
     */
    if (kthread_should_stop() && !hasDelayedWorkItems(queue)) {
      /*
       * Recheck once again in case we *just* converted a delayed work item to
       * a regular enqueued work item.
       *
       * It's important that processDelayedWorkItems holds the spin lock until
       * it finishes enqueueing the work item to run.
       *
       * Funnel queues aren't synchronized between producers and consumer.
       * Normally a producer interrupted mid-update can hide a later producer's
       * entry until the first completes. This would be a problem, except that
       * when kthread_stop is called, we should already have ceased adding new
       * work items and have waited for all the regular work items to finish;
       * (recurring) delayed work items should be the only exception.
       *
       * Worker thread shutdown would be simpler if even the delayed work items
       * were required to be completed and not re-queued before shutting down a
       * work queue.
       */
      item = pollForWorkItem(queue);
      break;
    }

    /*
     * We don't need to update the wait count atomically since this is the only
     * place it is modified and there is only one thread involved.
     */
    queue->stats.waits++;
    uint64_t timeBeforeSchedule = currentTime(CT_MONOTONIC);
    atomic64_add(timeBeforeSchedule - queue->mostRecentWakeup,
                 &queue->stats.runTime);
    // Wake up often, to address the missed-wakeup race.
    schedule_timeout(timeoutInterval);
    queue->mostRecentWakeup = currentTime(CT_MONOTONIC);
    uint64_t callDurationNS = queue->mostRecentWakeup - timeBeforeSchedule;
    enterHistogramSample(queue->stats.scheduleTimeHistogram,
                         callDurationNS / 1000);

    /*
     * Check again before resetting firstWakeup for more accurate
     * stats. (It's still racy, which can't be fixed without requiring
     * tighter synchronization between producer and consumer sides.)
     */
    item = pollForWorkItem(queue);
    if (item != NULL) {
      break;
    }
  }

  if (item != NULL) {
    uint64_t firstWakeup = atomic64_read(&queue->firstWakeup);
    /*
     * We sometimes register negative wakeup latencies without this fencing.
     * Whether it's forcing full serialization between the read of firstWakeup
     * and the "rdtsc" that might be used depending on the clock source that
     * helps, or some extra nanoseconds of delay covering for high-resolution
     * clocks not being quite in sync between CPUs, is not yet clear.
     */
    loadFence();
    if (firstWakeup != 0) {
      enterHistogramSample(queue->stats.wakeupLatencyHistogram,
                           (currentTime(CT_MONOTONIC) - firstWakeup) / 1000);
      enterHistogramSample(queue->stats.wakeupQueueLengthHistogram,
                           getPendingCount(queue));
    }
  }
  finish_wait(&queue->waitingWorkerThreads, &wait);
  atomic_set(&queue->idle, 0);

  return item;
}

/**
 * Get the next work item to process, possibly waiting for one, unless
 * kthread_should_stop indicates that it's time for us to shut down.
 *
 * If kthread_should_stop says it's time to stop but we have pending work
 * items, return a work item.
 *
 * @param [in]     queue            The work queue to wait on
 * @param [in]     timeoutInterval  How long to wait each iteration
 *
 * @return  the next work item, or NULL to indicate shutdown is requested
 **/
static KvdoWorkItem *getNextWorkItem(SimpleWorkQueue *queue,
                                     TimeoutJiffies   timeoutInterval)
{
  KvdoWorkItem *item = pollForWorkItem(queue);
  if (item != NULL) {
    return item;
  }
  return waitForNextWorkItem(queue, timeoutInterval);
}

/**
 * Execute a work item from a work queue, and do associated bookkeeping.
 *
 * @param [in]     queue  the work queue the item is from
 * @param [in]     item   the work item to run
 **/
static void processWorkItem(SimpleWorkQueue *queue,
                            KvdoWorkItem    *item)
{
  if (ASSERT(item->myQueue == &queue->common,
             "item %p from queue %p marked as being in this queue "
             "(%p)", item, queue, item->myQueue) == UDS_SUCCESS) {
    updateStatsForDequeue(&queue->stats, item);
    item->myQueue = NULL;
  }

  // Save the index, so we can use it after the work function.
  unsigned int index = item->statTableIndex;
  uint64_t workStartTime = recordStartTime(index);
  item->work(item);
  // We just surrendered control of the work item; no more access.
  item = NULL;
  updateWorkItemStatsForWorkTime(&queue->stats.workItemStats, index,
                                 workStartTime);

  /*
   * Be friendly to a CPU that has other work to do, if the kernel has told us
   * to. This speeds up some performance tests; that "other work" might include
   * other VDO threads.
   *
   * N.B.: We compute the pending count info here without any synchronization,
   * but it's for stats reporting only, so being imprecise isn't too big a
   * deal, as long as reads and writes are atomic operations.
   */
  if (need_resched()) {
    uint64_t timeBeforeReschedule = currentTime(CT_MONOTONIC);
    // Record the queue length we have *before* rescheduling.
    unsigned int queueLen = getPendingCount(queue);
    cond_resched();
    uint64_t timeAfterReschedule = currentTime(CT_MONOTONIC);

    enterHistogramSample(queue->stats.rescheduleQueueLengthHistogram,
                         queueLen);
    uint64_t runTimeNS = timeBeforeReschedule - queue->mostRecentWakeup;
    enterHistogramSample(queue->stats.runTimeBeforeRescheduleHistogram,
                         runTimeNS / 1000);
    atomic64_add(runTimeNS, &queue->stats.runTime);
    uint64_t callTimeNS = timeAfterReschedule - timeBeforeReschedule;
    enterHistogramSample(queue->stats.rescheduleTimeHistogram,
                         callTimeNS / 1000);
    atomic64_add(callTimeNS, &queue->stats.rescheduleTime);
    queue->mostRecentWakeup = timeAfterReschedule;
  }
}

/**
 * Main loop of the work queue worker thread.
 *
 * Waits for work items and runs them, until told to stop.
 *
 * @param queue  The work queue to run
 **/
static void serviceWorkQueue(SimpleWorkQueue *queue)
{
  TimeoutJiffies timeoutInterval =
    maxLong(2, usecs_to_jiffies(FUNNEL_HEARTBEAT_INTERVAL + 1) - 1);

  runStartHook(queue);

  while (true) {
    KvdoWorkItem *item = getNextWorkItem(queue, timeoutInterval);
    if (item == NULL) {
      // No work items but kthread_should_stop was triggered.
      break;
    }
    // Process the work item
    processWorkItem(queue, item);
  }

  runFinishHook(queue);
}

/**
 * Initialize per-thread data for a new worker thread and run the work queue.
 * Called in a new thread created by kthread_run().
 *
 * @param ptr  A pointer to the KvdoWorkQueue to run.
 *
 * @return  0 (indicating success to kthread_run())
 **/
static int workQueueRunner(void *ptr)
{
  SimpleWorkQueue *queue = ptr;
  kobject_get(&queue->common.kobj);

  WorkQueueStackHandle queueHandle;
  initializeWorkQueueStackHandle(&queueHandle, queue);
  queue->stats.startTime = queue->mostRecentWakeup = currentTime(CT_MONOTONIC);
  atomic_set(&queue->started, true);
  wake_up(&queue->startWaiters);
  serviceWorkQueue(queue);

  // Zero out handle structure for safety.
  memset(&queueHandle, 0, sizeof(queueHandle));

  kobject_put(&queue->common.kobj);
  return 0;
}

// Preparing work items

/**********************************************************************/
void setupWorkItem(KvdoWorkItem     *item,
                   KvdoWorkFunction  work,
                   void             *statsFunction,
                   unsigned int      action)
{
  ASSERT_LOG_ONLY(item->myQueue == NULL,
                  "setupWorkItem not called on enqueued work item");
  item->work           = work;
  item->statsFunction  = ((statsFunction == NULL) ? work : statsFunction);
  item->statTableIndex = 0;
  item->action         = action;
  item->myQueue        = NULL;
  item->executionTime  = 0;
  item->next           = NULL;
}

// Thread management

/**********************************************************************/
static inline void wakeWorkerThread(SimpleWorkQueue *queue)
{
  atomic64_cmpxchg(&queue->firstWakeup, 0, currentTime(CT_MONOTONIC));
  // Despite the name, there's a maximum of one thread in this list.
  wake_up(&queue->waitingWorkerThreads);
}

// Delayed work items

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
/**
 * Timer function invoked when a delayed work item is ready to run.
 *
 * @param timer  The timer which has just finished
 **/
static void processDelayedWorkItems(struct timer_list *timer)
#else
/**
 * Timer function invoked when a delayed work item is ready to run.
 *
 * @param data  The queue pointer, as an unsigned long
 **/
static void processDelayedWorkItems(unsigned long data)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  SimpleWorkQueue *queue = from_timer(queue, timer, delayedItemsTimer);
#else
  SimpleWorkQueue *queue = (SimpleWorkQueue *) data;
#endif
  Jiffies          nextExecutionTime = 0;
  bool             reschedule        = false;
  bool             needsWakeup       = false;

  unsigned long flags;
  spin_lock_irqsave(&queue->lock, flags);
  while (!isWorkItemListEmpty(&queue->delayedItems)) {
    KvdoWorkItem *item = workItemListPeek(&queue->delayedItems);
    if (item->executionTime > jiffies) {
      nextExecutionTime = item->executionTime;
      reschedule = true;
      break;
    }
    workItemListPoll(&queue->delayedItems);
    item->executionTime = 0;    // not actually looked at...
    item->myQueue = NULL;
    needsWakeup |= enqueueWorkQueueItem(queue, item);
  }
  spin_unlock_irqrestore(&queue->lock, flags);
  if (reschedule) {
    mod_timer(&queue->delayedItemsTimer, nextExecutionTime);
  }
  if (needsWakeup) {
    wakeWorkerThread(queue);
  }
}

// Creation & teardown

/**
 * Create a simple work queue with a worker thread.
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
 * @param [out] queuePtr         Where to store the queue handle
 *
 * @return  VDO_SUCCESS or an error code
 **/
static int makeSimpleWorkQueue(const char               *threadNamePrefix,
                               const char               *name,
                               struct kobject           *parentKobject,
                               KernelLayer              *owner,
                               void                     *private,
                               const KvdoWorkQueueType  *type,
                               SimpleWorkQueue         **queuePtr)
{
  SimpleWorkQueue *queue;
  int result = ALLOCATE(1, SimpleWorkQueue, "simple work queue", &queue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  queue->type         = type;
  queue->private      = private;
  queue->common.owner = owner;

  unsigned int numPriorityLists = 1;
  for (int i = 0; i < WORK_QUEUE_ACTION_COUNT; i++) {
    const KvdoWorkQueueAction *action = &queue->type->actionTable[i];
    if (action->name == NULL) {
      break;
    }
    unsigned int code     = action->code;
    unsigned int priority = action->priority;

    result = ASSERT(code < WORK_QUEUE_ACTION_COUNT,
                    "invalid action code %u in work queue initialization",
                    code);
    if (result != VDO_SUCCESS) {
      FREE(queue);
      return result;
    }
    result = ASSERT(priority < WORK_QUEUE_PRIORITY_COUNT,
                    "invalid action priority %u in work queue initialization",
                    priority);
    if (result != VDO_SUCCESS) {
      FREE(queue);
      return result;
    }
    queue->priorityMap[code] = priority;
    if (numPriorityLists <= priority) {
      numPriorityLists = priority + 1;
    }
  }

  result = duplicateString(name, "queue name", &queue->common.name);
  if (result != VDO_SUCCESS) {
    FREE(queue);
    return -ENOMEM;
  }

  init_waitqueue_head(&queue->waitingWorkerThreads);
  init_waitqueue_head(&queue->startWaiters);
  spin_lock_init(&queue->lock);

  initializeWorkItemList(&queue->delayedItems);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
  timer_setup(&queue->delayedItemsTimer, processDelayedWorkItems, 0);
#else
  setup_timer(&queue->delayedItemsTimer, processDelayedWorkItems,
              (unsigned long) queue);
#endif

  queue->numPriorityLists = numPriorityLists;
  for (int i = 0; i < WORK_QUEUE_PRIORITY_COUNT; i++) {
    result = makeFunnelQueue(&queue->priorityLists[i]);
    if (result != UDS_SUCCESS) {
      freeSimpleWorkQueue(queue);
      return result;
    }
  }

  kobject_init(&queue->common.kobj, &simpleWorkQueueKobjType);
  result = kobject_add(&queue->common.kobj, parentKobject, queue->common.name);
  if (result != 0) {
    logError("Cannot add sysfs node: %d", result);
    finishSimpleWorkQueue(queue);
    freeSimpleWorkQueue(queue);
    return result;
  }
  result = initializeWorkQueueStats(&queue->stats, &queue->common.kobj);
  if (result != 0) {
    logError("Cannot initialize statistics tracking: %d", result);
    finishSimpleWorkQueue(queue);
    freeSimpleWorkQueue(queue);
    return result;
  }

  atomic_set(&queue->started, false);
  struct task_struct *thread = NULL;
  thread = kthread_run(workQueueRunner, queue, "%s:%s", threadNamePrefix,
                       queue->common.name);

  if (IS_ERR(thread)) {
    finishSimpleWorkQueue(queue);
    freeSimpleWorkQueue(queue);
    return (int) PTR_ERR(thread);
  }
  queue->thread = thread;
  atomic_set(&queue->threadID, thread->pid);
  /*
   * If we don't wait to ensure the thread is running VDO code, a
   * quick kthread_stop (due to errors elsewhere) could cause it to
   * never get as far as running VDO, skipping the cleanup code.
   *
   * Eventually we should just make that path safe too, and then we
   * won't need this synchronization.
   */
  wait_event(queue->startWaiters, atomic_read(&queue->started) == true);
  *queuePtr = queue;
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeWorkQueue(const char               *threadNamePrefix,
                  const char               *name,
                  struct kobject           *parentKobject,
                  KernelLayer              *owner,
                  void                     *private,
                  const KvdoWorkQueueType  *type,
                  unsigned int              threadCount,
                  KvdoWorkQueue           **queuePtr)
{
  if (threadCount == 1) {
    SimpleWorkQueue *simpleQueue;
    int result = makeSimpleWorkQueue(threadNamePrefix, name, parentKobject,
                                     owner, private, type, &simpleQueue);
    if (result == VDO_SUCCESS) {
      *queuePtr = &simpleQueue->common;
    }
    return result;
  }

  RoundRobinWorkQueue *queue;
  int result = ALLOCATE(1, RoundRobinWorkQueue, "round-robin work queue",
                        &queue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ALLOCATE(threadCount, SimpleWorkQueue *, "subordinate work queues",
                    &queue->serviceQueues);
  if (result != UDS_SUCCESS) {
    FREE(queue);
    return result;
  }

  queue->numServiceQueues      = threadCount;
  queue->common.roundRobinMode = true;
  queue->common.owner          = owner;

  result = duplicateString(name, "queue name", &queue->common.name);
  if (result != VDO_SUCCESS) {
    FREE(queue);
    return -ENOMEM;
  }

  kobject_init(&queue->common.kobj, &roundRobinWorkQueueKobjType);
  result = kobject_add(&queue->common.kobj, parentKobject, queue->common.name);
  if (result != 0) {
    logError("Cannot add sysfs node: %d", result);
    finishWorkQueue(&queue->common);
    kobject_put(&queue->common.kobj);
    return result;
  }

  *queuePtr = &queue->common;

  char threadName[TASK_COMM_LEN];
  for (unsigned int i = 0; i < threadCount; i++) {
    snprintf(threadName, sizeof(threadName), "%s%u", name, i);
    result = makeSimpleWorkQueue(threadNamePrefix, threadName,
                                 &queue->common.kobj, owner, private, type,
                                 &queue->serviceQueues[i]);
    if (result != VDO_SUCCESS) {
      queue->numServiceQueues = i;
      // Destroy previously created subordinates.
      finishWorkQueue(*queuePtr);
      freeWorkQueue(queuePtr);
      return result;
    }
    queue->serviceQueues[i]->parentQueue = *queuePtr;
  }

  return VDO_SUCCESS;
}

/**
 * Shut down a simple work queue's worker thread.
 *
 * @param queue  The work queue to shut down
 **/
static void finishSimpleWorkQueue(SimpleWorkQueue *queue)
{
  // Tell the worker thread to shut down.
  if (queue->thread != NULL) {
    atomic_set(&queue->threadID, 0);
    // Waits for thread to exit.
    kthread_stop(queue->thread);
  }

  queue->thread = NULL;
}

/**
 * Shut down a round-robin work queue's service queues.
 *
 * @param queue  The work queue to shut down
 **/
static void finishRoundRobinWorkQueue(RoundRobinWorkQueue *queue)
{
  SimpleWorkQueue **queueTable = queue->serviceQueues;
  unsigned int      count      = queue->numServiceQueues;

  for (unsigned int i = 0; i < count; i++) {
    finishSimpleWorkQueue(queueTable[i]);
  }
}

/**********************************************************************/
void finishWorkQueue(KvdoWorkQueue *queue)
{
  if (queue->roundRobinMode) {
    finishRoundRobinWorkQueue(asRoundRobinWorkQueue(queue));
  } else {
    finishSimpleWorkQueue(asSimpleWorkQueue(queue));
  }
}

/**
 * Tear down a simple work queue, and decrement the kobject reference
 * count on it.
 *
 * @param queue  The work queue
 **/
static void freeSimpleWorkQueue(SimpleWorkQueue *queue)
{
  for (unsigned int i = 0; i < WORK_QUEUE_PRIORITY_COUNT; i++) {
    freeFunnelQueue(queue->priorityLists[i]);
  }
  cleanupWorkQueueStats(&queue->stats);
  kobject_put(&queue->common.kobj);
}

/**
 * Tear down a round-robin work queue and its service queues, and
 * decrement the kobject reference count on it.
 *
 * @param queue  The work queue
 **/
static void freeRoundRobinWorkQueue(RoundRobinWorkQueue *queue)
{
  SimpleWorkQueue **queueTable = queue->serviceQueues;
  unsigned int      count      = queue->numServiceQueues;

  queue->serviceQueues = NULL;
  for (unsigned int i = 0; i < count; i++) {
    freeSimpleWorkQueue(queueTable[i]);
  }
  FREE(queueTable);
  kobject_put(&queue->common.kobj);
}

/**********************************************************************/
void freeWorkQueue(KvdoWorkQueue **queuePtr)
{
  KvdoWorkQueue *queue = *queuePtr;
  if (queue == NULL) {
    return;
  }
  *queuePtr = NULL;

  if (queue->roundRobinMode) {
    freeRoundRobinWorkQueue(asRoundRobinWorkQueue(queue));
  } else {
    freeSimpleWorkQueue(asSimpleWorkQueue(queue));
  }
}

// Debugging dumps

/**********************************************************************/
static void dumpSimpleWorkQueue(SimpleWorkQueue *queue)
{
  mutex_lock(&queueDataLock);
  // Take a snapshot to reduce inconsistency in logged numbers.
  queueData = *queue;
  const char *threadStatus;

  char taskStateReport = '-';
  if (queueData.thread != NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0)
    taskStateReport = task_state_to_char(queue->thread);
#else
    unsigned int taskState = queue->thread->state & TASK_REPORT;
    taskState &= 0x1ff;
    unsigned int taskStateIndex;
    if (taskState != 0) {
      taskStateIndex = __ffs(taskState)+1;
      BUG_ON(taskStateIndex >= sizeof(TASK_STATE_TO_CHAR_STR));
    } else {
      taskStateIndex = 0;
    }
    taskStateReport = TASK_STATE_TO_CHAR_STR[taskStateIndex];
#endif
  }

  if (queueData.thread == NULL) {
    threadStatus = "no threads";
  } else if (atomic_read(&queueData.idle)) {
    threadStatus = "idle";
  } else {
    threadStatus = "running";
  }
  logInfo("workQ %p (%s) %u entries %llu waits, %s (%c)",
          &queue->common,
          queueData.common.name,
          getPendingCount(&queueData),
          queueData.stats.waits,
          threadStatus,
          taskStateReport);

  logWorkItemStats(&queueData.stats.workItemStats);
  logWorkQueueStats(queue);

  mutex_unlock(&queueDataLock);

  // ->lock spin lock status?
  // ->waitingWorkerThreads wait queue status? anyone waiting?
}

/**********************************************************************/
void dumpWorkQueue(KvdoWorkQueue *queue)
{
  if (queue->roundRobinMode) {
    RoundRobinWorkQueue *roundRobinQueue = asRoundRobinWorkQueue(queue);
    for (unsigned int i = 0; i < roundRobinQueue->numServiceQueues; i++) {
      dumpSimpleWorkQueue(roundRobinQueue->serviceQueues[i]);
    }
  } else {
    dumpSimpleWorkQueue(asSimpleWorkQueue(queue));
  }
}

/**********************************************************************/
void dumpWorkItemToBuffer(KvdoWorkItem *item, char *buffer, size_t length)
{
  size_t currentLength
    = snprintf(buffer, length, "%.*s/", TASK_COMM_LEN,
               item->myQueue == NULL ? "-" : item->myQueue->name);
  if (currentLength < length) {
    getFunctionName(item->statsFunction, buffer + currentLength,
                    length - currentLength);
  }
}

// Work submission

/**********************************************************************/
void enqueueWorkQueue(KvdoWorkQueue *kvdoWorkQueue, KvdoWorkItem *item)
{
  SimpleWorkQueue *queue = pickSimpleQueue(kvdoWorkQueue);

  item->executionTime = 0;

  if (enqueueWorkQueueItem(queue, item)) {
    wakeWorkerThread(queue);
  }
}

/**********************************************************************/
void enqueueWorkQueueDelayed(KvdoWorkQueue *kvdoWorkQueue,
                             KvdoWorkItem  *item,
                             Jiffies        executionTime)
{
  if (executionTime <= jiffies) {
    enqueueWorkQueue(kvdoWorkQueue, item);
    return;
  }

  SimpleWorkQueue *queue             = pickSimpleQueue(kvdoWorkQueue);
  bool             rescheduleTimer   = false;
  unsigned long    flags;

  item->executionTime = executionTime;

  // Lock if the work item is delayed. All delayed items are handled via a
  // single linked list.
  spin_lock_irqsave(&queue->lock, flags);

  if (isWorkItemListEmpty(&queue->delayedItems)) {
    rescheduleTimer = true;
  }
  /*
   * XXX We should keep the list sorted, but at the moment the list won't
   * grow above a single entry anyway.
   */
  item->myQueue = &queue->common;
  addToWorkItemList(&queue->delayedItems, item);

  spin_unlock_irqrestore(&queue->lock, flags);

  if (rescheduleTimer) {
    mod_timer(&queue->delayedItemsTimer, executionTime);
  }
}

// Misc


/**********************************************************************/
KvdoWorkQueue *getCurrentWorkQueue(void)
{
  SimpleWorkQueue *queue = getCurrentThreadWorkQueue();
  return (queue == NULL) ? NULL : &queue->common;
}

/**********************************************************************/
KernelLayer *getWorkQueueOwner(KvdoWorkQueue *queue)
{
  return queue->owner;
}

/**********************************************************************/
void *getWorkQueuePrivateData(void)
{
  SimpleWorkQueue *queue = getCurrentThreadWorkQueue();
  return (queue != NULL) ? queue->private : NULL;
}

/**********************************************************************/
void setWorkQueuePrivateData(void *newData)
{
  SimpleWorkQueue *queue = getCurrentThreadWorkQueue();
  BUG_ON(queue == NULL);
  queue->private = newData;
}

/**********************************************************************/
void initWorkQueueOnce(void)
{
  // We can't use DEFINE_MUTEX because it's not compatible with c99 mode.
  mutex_init(&queueDataLock);
  initWorkQueueStackHandleOnce();
}
