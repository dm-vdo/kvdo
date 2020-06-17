/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueue.c#30 $
 */

#include "workQueue.h"

#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/version.h>

#include "atomic.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"
#include "stringUtils.h"

#include "workItemStats.h"
#include "workQueueHandle.h"
#include "workQueueInternals.h"
#include "workQueueStats.h"
#include "workQueueSysfs.h"

enum {
	/*
	 * Time between work queue heartbeats in usec. The default kernel
	 * configurations generally have 1ms or 4ms tick rates, so let's make
	 * this a multiple for accuracy.
	 */
	FUNNEL_HEARTBEAT_INTERVAL = 4000,

	/*
	 * Time to wait for a work queue to flush remaining items during
	 * shutdown.  Specified in milliseconds.
	 */
	FUNNEL_FINISH_SLEEP = 5000,
};

static struct mutex queue_data_lock;
static struct simple_work_queue queue_data;

static DEFINE_PER_CPU(unsigned int, service_queue_rotor);

static void free_simple_work_queue(struct simple_work_queue *queue);
static void finish_simple_work_queue(struct simple_work_queue *queue);

// Finding the simple_work_queue to actually operate on.

/**
 * Pick the subordinate service queue to use, distributing the work evenly
 * across them.
 *
 * @param queue  The round-robin-type work queue
 *
 * @return  A subordinate work queue
 **/
static inline struct simple_work_queue *
next_service_queue(struct round_robin_work_queue *queue)
{
	/*
	 * It shouldn't be a big deal if the same rotor gets used for multiple
	 * work queues. Any patterns that might develop are likely to be
	 * disrupted by random ordering of multiple work items and migration
	 * between cores, unless the load is so light as to be regular in
	 * ordering of tasks and the threads are confined to individual cores;
	 * with a load that light we won't care.
	 */
	unsigned int rotor = this_cpu_inc_return(service_queue_rotor);
	unsigned int index = rotor % queue->num_service_queues;

	return queue->service_queues[index];
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
static inline struct simple_work_queue *
pick_simple_queue(struct kvdo_work_queue *queue)
{
	return (queue->round_robin_mode ?
			next_service_queue(as_round_robin_work_queue(queue)) :
			as_simple_work_queue(queue));
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
static struct kvdo_work_item *
poll_for_work_item(struct simple_work_queue *queue)
{
	struct kvdo_work_item *item = NULL;
	int i;

	for (i = READ_ONCE(queue->num_priority_lists) - 1; i >= 0; i--) {
		struct funnel_queue_entry *link =
			funnel_queue_poll(queue->priority_lists[i]);
		if (link != NULL) {
			item = container_of(link,
					    struct kvdo_work_item,
					    work_queue_entry_link);
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
static bool __must_check
enqueue_work_queue_item(struct simple_work_queue *queue,
			struct kvdo_work_item *item)
{
	ASSERT_LOG_ONLY(item->my_queue == NULL,
			"item %" PRIptr " (fn %" PRIptr "/%" PRIptr ") to enqueue (%" PRIptr ") is not already queued (%" PRIptr ")",
			item, item->work, item->stats_function, queue,
			item->my_queue);
	if (ASSERT(item->action < WORK_QUEUE_ACTION_COUNT,
		   "action is in range for queue") != VDO_SUCCESS) {
		item->action = 0;
	}
	unsigned int priority = READ_ONCE(queue->priority_map[item->action]);

	// Update statistics.
	update_stats_for_enqueue(&queue->stats, item, priority);

	item->my_queue = &queue->common;

	// Funnel queue handles the synchronization for the put.
	funnel_queue_put(queue->priority_lists[priority],
			 &item->work_queue_entry_link);

	/*
	 * Due to how funnel queue synchronization is handled (just atomic
	 * operations), the simplest safe implementation here would be to
	 * wake-up any waiting threads after enqueueing each item. Even if the
	 * funnel queue is not empty at the time of adding an item to the queue,
	 * the consumer thread may not see this since it is not guaranteed to
	 * have the same view of the queue as a producer thread.
	 *
	 * However, the above is wasteful so instead we attempt to minimize the
	 * number of thread wakeups. This is normally unsafe due to the above
	 * consumer-producer synchronization constraints. To correct this a
	 * timeout mechanism is used to wake the thread periodically to handle
	 * the occasional race condition that triggers and results in this
	 * thread not being woken properly.
	 *
	 * In most cases, the above timeout will not occur prior to some other
	 * work item being added after the queue is set to idle state, so thread
	 * wakeups will generally be triggered much faster than this interval.
	 * The timeout provides protection against the cases where more work
	 * items are either not added or are added too infrequently.
	 *
	 * This is also why we can get away with the normally-unsafe
	 * optimization for the common case by checking queue->idle first
	 * without synchronization. The race condition exists, but another work
	 * item getting enqueued can wake us up, and if we don't get that
	 * either, we still have the timeout to fall back on.
	 *
	 * Developed and tuned for some x86 boxes; untested whether this is any
	 * better or worse for other platforms, with or without the explicit
	 * memory barrier.
	 */
	smp_mb();
	return ((atomic_read(&queue->idle) == 1) &&
		(atomic_cmpxchg(&queue->idle, 1, 0) == 1));
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
static unsigned int get_pending_count(struct simple_work_queue *queue)
{
	struct kvdo_work_item_stats *stats = &queue->stats.work_item_stats;
	long long pending = 0;
	int i;

	for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
		pending += atomic64_read(&stats->enqueued[i]);
		pending -= stats->times[i].count;
	}
	if (pending < 0) {
		/*
		 * If we fetched numbers that were changing, we can get negative
		 * results. Just return an indication that there's some
		 * activity.
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
static void run_start_hook(struct simple_work_queue *queue)
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
static void run_finish_hook(struct simple_work_queue *queue)
{
	if (queue->type->finish != NULL) {
		queue->type->finish(queue->private);
	}
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
 * @param [in]     queue             The work queue to wait on
 * @param [in]     timeout_interval  How long to wait each iteration
 *
 * @return  the next work item, or NULL to indicate shutdown is requested
 **/
static struct kvdo_work_item *
wait_for_next_work_item(struct simple_work_queue *queue,
			TimeoutJiffies timeout_interval)
{
	struct kvdo_work_item *item = poll_for_work_item(queue);

	if (item != NULL) {
		return item;
	}

	DEFINE_WAIT(wait);

	while (true) {
		atomic64_set(&queue->first_wakeup, 0);
		prepare_to_wait(&queue->waiting_worker_threads,
				&wait,
				TASK_INTERRUPTIBLE);
		/*
		 * Don't set the idle flag until a wakeup will not be lost.
		 *
		 * Force synchronization between setting the idle flag and
		 * checking the funnel queue; the producer side will do them in
		 * the reverse order. (There's still a race condition we've
		 * chosen to allow, because we've got a timeout below that
		 * unwedges us if we hit it, but this may narrow the window a
		 * little.)
		 */
		atomic_set(&queue->idle, 1);
		memoryFence(); // store-load barrier between "idle" and funnel
			       // queue

		item = poll_for_work_item(queue);
		if (item != NULL) {
			break;
		}

		/*
		 * We need to check for thread-stop after setting
		 * TASK_INTERRUPTIBLE state up above. Otherwise, schedule() will
		 * put the thread to sleep and might miss a wakeup from
		 * kthread_stop() call in finish_work_queue().
		 */
		if (kthread_should_stop()) {
			break;
		}

		/*
		 * We don't need to update the wait count atomically since this
		 * is the only place it is modified and there is only one thread
		 * involved.
		 */
		queue->stats.waits++;
		uint64_t time_before_schedule = ktime_get_ns();

		atomic64_add(time_before_schedule - queue->most_recent_wakeup,
			     &queue->stats.run_time);
		// Wake up often, to address the missed-wakeup race.
		schedule_timeout(timeout_interval);
		queue->most_recent_wakeup = ktime_get_ns();
		uint64_t call_duration_ns =
			queue->most_recent_wakeup - time_before_schedule;
		enter_histogram_sample(queue->stats.schedule_time_histogram,
				       call_duration_ns / 1000);

		/*
		 * Check again before resetting first_wakeup for more accurate
		 * stats. (It's still racy, which can't be fixed without
		 * requiring tighter synchronization between producer and
		 * consumer sides.)
		 */
		item = poll_for_work_item(queue);
		if (item != NULL) {
			break;
		}
	}

	if (item != NULL) {
		uint64_t first_wakeup = atomic64_read(&queue->first_wakeup);
		/*
		 * We sometimes register negative wakeup latencies without this
		 * fencing. Whether it's forcing full serialization between the
		 * read of first_wakeup and the "rdtsc" that might be used
		 * depending on the clock source that helps, or some extra
		 * nanoseconds of delay covering for high-resolution clocks not
		 * being quite in sync between CPUs, is not yet clear.
		 */
		loadFence();
		if (first_wakeup != 0) {
			enter_histogram_sample(
				queue->stats.wakeup_latency_histogram,
				(ktime_get_ns() - first_wakeup) / 1000);
			enter_histogram_sample(
				queue->stats.wakeup_queue_length_histogram,
				get_pending_count(queue));
		}
	}
	finish_wait(&queue->waiting_worker_threads, &wait);
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
 * @param [in]     queue             The work queue to wait on
 * @param [in]     timeout_interval  How long to wait each iteration
 *
 * @return  the next work item, or NULL to indicate shutdown is requested
 **/
static struct kvdo_work_item *
get_next_work_item(struct simple_work_queue *queue,
		   TimeoutJiffies timeout_interval)
{
	struct kvdo_work_item *item = poll_for_work_item(queue);

	if (item != NULL) {
		return item;
	}
	return wait_for_next_work_item(queue, timeout_interval);
}

/**
 * Execute a work item from a work queue, and do associated bookkeeping.
 *
 * @param [in]     queue  the work queue the item is from
 * @param [in]     item   the work item to run
 **/
static void process_work_item(struct simple_work_queue *queue,
			      struct kvdo_work_item *item)
{
	if (ASSERT(item->my_queue == &queue->common,
		   "item %" PRIptr " from queue %" PRIptr " marked as being in this queue (%" PRIptr ")",
		   item, queue, item->my_queue) == UDS_SUCCESS) {
		update_stats_for_dequeue(&queue->stats, item);
		item->my_queue = NULL;
	}

	// Save the index, so we can use it after the work function.
	unsigned int index = item->stat_table_index;
	uint64_t work_start_time = record_start_time(index);

	item->work(item);
	// We just surrendered control of the work item; no more access.
	item = NULL;
	update_work_item_stats_for_work_time(&queue->stats.work_item_stats,
					     index,
					     work_start_time);

	/*
	 * Be friendly to a CPU that has other work to do, if the kernel has
	 * told us to. This speeds up some performance tests; that "other work"
	 * might include other VDO threads.
	 *
	 * N.B.: We compute the pending count info here without any
	 * synchronization, but it's for stats reporting only, so being
	 * imprecise isn't too big a deal, as long as reads and writes are
	 * atomic operations.
	 */
	if (need_resched()) {
		uint64_t time_before_reschedule = ktime_get_ns();
		// Record the queue length we have *before* rescheduling.
		unsigned int queue_len = get_pending_count(queue);

		cond_resched();
		uint64_t time_after_reschedule = ktime_get_ns();

		enter_histogram_sample(
			queue->stats.reschedule_queue_length_histogram,
			queue_len);
		uint64_t run_time_ns =
			time_before_reschedule - queue->most_recent_wakeup;
		enter_histogram_sample(
			queue->stats.run_time_before_reschedule_histogram,
			run_time_ns / 1000);
		atomic64_add(run_time_ns, &queue->stats.run_time);
		uint64_t call_time_ns =
			time_after_reschedule - time_before_reschedule;
		enter_histogram_sample(queue->stats.reschedule_time_histogram,
				       call_time_ns / 1000);
		atomic64_add(call_time_ns, &queue->stats.reschedule_time);
		queue->most_recent_wakeup = time_after_reschedule;
	}
}

/**
 * Main loop of the work queue worker thread.
 *
 * Waits for work items and runs them, until told to stop.
 *
 * @param queue  The work queue to run
 **/
static void service_work_queue(struct simple_work_queue *queue)
{
	TimeoutJiffies timeout_interval =
		max_long(2,
			 usecs_to_jiffies(FUNNEL_HEARTBEAT_INTERVAL + 1) - 1);

	run_start_hook(queue);

	while (true) {
		struct kvdo_work_item *item =
			get_next_work_item(queue, timeout_interval);
		if (item == NULL) {
			// No work items but kthread_should_stop was triggered.
			break;
		}
		// Process the work item
		process_work_item(queue, item);
	}

	run_finish_hook(queue);
}

/**
 * Initialize per-thread data for a new worker thread and run the work queue.
 * Called in a new thread created by kthread_run().
 *
 * @param ptr  A pointer to the kvdo_work_queue to run.
 *
 * @return  0 (indicating success to kthread_run())
 **/
static int work_queue_runner(void *ptr)
{
	struct simple_work_queue *queue = ptr;

	kobject_get(&queue->common.kobj);

	struct work_queue_stack_handle queue_handle;

	initialize_work_queue_stack_handle(&queue_handle, queue);
	queue->stats.start_time = queue->most_recent_wakeup = ktime_get_ns();

	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	queue->started = true;
	spin_unlock_irqrestore(&queue->lock, flags);
	wake_up(&queue->start_waiters);
	service_work_queue(queue);

	// Zero out handle structure for safety.
	memset(&queue_handle, 0, sizeof(queue_handle));

	kobject_put(&queue->common.kobj);
	return 0;
}

// Preparing work items

/**********************************************************************/
void setup_work_item(struct kvdo_work_item *item,
		     KvdoWorkFunction work,
		     void *stats_function,
		     unsigned int action)
{
	ASSERT_LOG_ONLY(item->my_queue == NULL,
			"setup_work_item not called on enqueued work item");
	item->work = work;
	item->stats_function =
		((stats_function == NULL) ? work : stats_function);
	item->stat_table_index = 0;
	item->action = action;
	item->my_queue = NULL;
}

// Thread management

/**********************************************************************/
static inline void wake_worker_thread(struct simple_work_queue *queue)
{
	smp_mb();
	atomic64_cmpxchg(&queue->first_wakeup, 0, ktime_get_ns());
	// Despite the name, there's a maximum of one thread in this list.
	wake_up(&queue->waiting_worker_threads);
}

// Creation & teardown

/**********************************************************************/
static bool queue_started(struct simple_work_queue *queue)
{
	unsigned long flags;

	spin_lock_irqsave(&queue->lock, flags);
	bool started = queue->started;

	spin_unlock_irqrestore(&queue->lock, flags);
	return started;
}

/**
 * Create a simple work queue with a worker thread.
 *
 * @param [in]  thread_name_prefix The per-device prefix to use in
 *                                 thread names
 * @param [in]  name               The queue name
 * @param [in]  parent_kobject     The parent sysfs node
 * @param [in]  owner              The kernel layer owning the work queue
 * @param [in]  private            Private data of the queue for use by work
 *                                 items or other queue-specific functions
 * @param [in]  type               The work queue type defining the lifecycle
 *                                 functions, queue actions, priorities, and
 *                                 timeout behavior
 * @param [out] queue_ptr          Where to store the queue handle
 *
 * @return  VDO_SUCCESS or an error code
 **/
static int make_simple_work_queue(const char *thread_name_prefix,
				  const char *name,
				  struct kobject *parent_kobject,
				  struct kernel_layer *owner,
				  void *private,
				  const struct kvdo_work_queue_type *type,
				  struct simple_work_queue **queue_ptr)
{
	struct simple_work_queue *queue;
	int result = ALLOCATE(1,
			      struct simple_work_queue,
			      "simple work queue",
			      &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	queue->type = type;
	queue->private = private;
	queue->common.owner = owner;

	unsigned int num_priority_lists = 1;
	int i;

	for (i = 0; i < WORK_QUEUE_ACTION_COUNT; i++) {
		const struct kvdo_work_queue_action *action =
			&queue->type->action_table[i];
		if (action->name == NULL) {
			break;
		}
		unsigned int code = action->code;
		unsigned int priority = action->priority;

		result = ASSERT(
			code < WORK_QUEUE_ACTION_COUNT,
			"invalid action code %u in work queue initialization",
			code);
		if (result != VDO_SUCCESS) {
			FREE(queue);
			return result;
		}
		result = ASSERT(
			priority < WORK_QUEUE_PRIORITY_COUNT,
			"invalid action priority %u in work queue initialization",
			priority);
		if (result != VDO_SUCCESS) {
			FREE(queue);
			return result;
		}
		queue->priority_map[code] = priority;
		if (num_priority_lists <= priority) {
			num_priority_lists = priority + 1;
		}
	}

	result = duplicateString(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		FREE(queue);
		return -ENOMEM;
	}

	init_waitqueue_head(&queue->waiting_worker_threads);
	init_waitqueue_head(&queue->start_waiters);
	spin_lock_init(&queue->lock);

	kobject_init(&queue->common.kobj, &simple_work_queue_kobj_type);
	result = kobject_add(&queue->common.kobj,
			     parent_kobject,
			     queue->common.name);
	if (result != 0) {
		logError("Cannot add sysfs node: %d", result);
		free_simple_work_queue(queue);
		return result;
	}
	queue->num_priority_lists = num_priority_lists;
	for (i = 0; i < WORK_QUEUE_PRIORITY_COUNT; i++) {
		result = make_funnel_queue(&queue->priority_lists[i]);
		if (result != UDS_SUCCESS) {
			free_simple_work_queue(queue);
			return result;
		}
	}
	result =
		initialize_work_queue_stats(&queue->stats, &queue->common.kobj);
	if (result != 0) {
		logError("Cannot initialize statistics tracking: %d", result);
		free_simple_work_queue(queue);
		return result;
	}

	queue->started = false;
	struct task_struct *thread = NULL;

	thread = kthread_run(work_queue_runner,
			     queue,
			     "%s:%s",
			     thread_name_prefix,
			     queue->common.name);

	if (IS_ERR(thread)) {
		free_simple_work_queue(queue);
		return (int) PTR_ERR(thread);
	}
	queue->thread = thread;
	atomic_set(&queue->thread_id, thread->pid);
	/*
	 * If we don't wait to ensure the thread is running VDO code, a
	 * quick kthread_stop (due to errors elsewhere) could cause it to
	 * never get as far as running VDO, skipping the cleanup code.
	 *
	 * Eventually we should just make that path safe too, and then we
	 * won't need this synchronization.
	 */
	wait_event(queue->start_waiters, queue_started(queue) == true);
	*queue_ptr = queue;
	return UDS_SUCCESS;
}

/**********************************************************************/
int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct kobject *parent_kobject,
		    struct kernel_layer *owner,
		    void *private,
		    const struct kvdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct kvdo_work_queue **queue_ptr)
{
	if (thread_count == 1) {
		void *context = (thread_privates != NULL) ? thread_privates[0] :
							    private;
		struct simple_work_queue *simple_queue;
		int result =
			make_simple_work_queue(thread_name_prefix,
					       name,
					       parent_kobject,
					       owner,
					       context,
					       type,
					       &simple_queue);
		if (result == VDO_SUCCESS) {
			*queue_ptr = &simple_queue->common;
		}
		return result;
	}

	struct round_robin_work_queue *queue;
	int result = ALLOCATE(1,
			      struct round_robin_work_queue,
			      "round-robin work queue",
			      &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ALLOCATE(thread_count,
			  struct simple_work_queue *,
			  "subordinate work queues",
			  &queue->service_queues);
	if (result != UDS_SUCCESS) {
		FREE(queue);
		return result;
	}

	queue->num_service_queues = thread_count;
	queue->common.round_robin_mode = true;
	queue->common.owner = owner;

	result = duplicateString(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		FREE(queue->service_queues);
		FREE(queue);
		return -ENOMEM;
	}

	kobject_init(&queue->common.kobj, &round_robin_work_queue_kobj_type);
	result = kobject_add(&queue->common.kobj,
			     parent_kobject,
			     queue->common.name);
	if (result != 0) {
		logError("Cannot add sysfs node: %d", result);
		finish_work_queue(&queue->common);
		kobject_put(&queue->common.kobj);
		return result;
	}

	*queue_ptr = &queue->common;

	char thread_name[TASK_COMM_LEN];
	unsigned int i;

	for (i = 0; i < thread_count; i++) {
		snprintf(thread_name, sizeof(thread_name), "%s%u", name, i);
		void *context = (thread_privates != NULL) ? thread_privates[i] :
							    private;
		result = make_simple_work_queue(thread_name_prefix,
						thread_name,
						&queue->common.kobj,
						owner,
						context,
						type,
						&queue->service_queues[i]);
		if (result != VDO_SUCCESS) {
			queue->num_service_queues = i;
			// Destroy previously created subordinates.
			finish_work_queue(*queue_ptr);
			free_work_queue(queue_ptr);
			return result;
		}
		queue->service_queues[i]->parent_queue = *queue_ptr;
	}

	return VDO_SUCCESS;
}

/**
 * Shut down a simple work queue's worker thread.
 *
 * @param queue  The work queue to shut down
 **/
static void finish_simple_work_queue(struct simple_work_queue *queue)
{
	// Tell the worker thread to shut down.
	if (queue->thread != NULL) {
		atomic_set(&queue->thread_id, 0);
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
static void finish_round_robin_work_queue(struct round_robin_work_queue *queue)
{
	struct simple_work_queue **queue_table = queue->service_queues;
	unsigned int count = queue->num_service_queues;

	unsigned int i;

	for (i = 0; i < count; i++) {
		finish_simple_work_queue(queue_table[i]);
	}
}

/**********************************************************************/
void finish_work_queue(struct kvdo_work_queue *queue)
{
	if (queue->round_robin_mode) {
		finish_round_robin_work_queue(as_round_robin_work_queue(queue));
	} else {
		finish_simple_work_queue(as_simple_work_queue(queue));
	}
}

/**
 * Tear down a simple work queue, and decrement the kobject reference
 * count on it.
 *
 * @param queue  The work queue
 **/
static void free_simple_work_queue(struct simple_work_queue *queue)
{
	unsigned int i;

	for (i = 0; i < WORK_QUEUE_PRIORITY_COUNT; i++) {
		free_funnel_queue(queue->priority_lists[i]);
	}
	cleanup_work_queue_stats(&queue->stats);
	kobject_put(&queue->common.kobj);
}

/**
 * Tear down a round-robin work queue and its service queues, and
 * decrement the kobject reference count on it.
 *
 * @param queue  The work queue
 **/
static void free_round_robin_work_queue(struct round_robin_work_queue *queue)
{
	struct simple_work_queue **queue_table = queue->service_queues;
	unsigned int count = queue->num_service_queues;

	queue->service_queues = NULL;
	unsigned int i;

	for (i = 0; i < count; i++) {
		free_simple_work_queue(queue_table[i]);
	}
	FREE(queue_table);
	kobject_put(&queue->common.kobj);
}

/**********************************************************************/
void free_work_queue(struct kvdo_work_queue **queue_ptr)
{
	struct kvdo_work_queue *queue = *queue_ptr;

	if (queue == NULL) {
		return;
	}
	*queue_ptr = NULL;

	finish_work_queue(queue);

	if (queue->round_robin_mode) {
		free_round_robin_work_queue(as_round_robin_work_queue(queue));
	} else {
		free_simple_work_queue(as_simple_work_queue(queue));
	}
}

// Debugging dumps

/**********************************************************************/
static void dump_simple_work_queue(struct simple_work_queue *queue)
{
	mutex_lock(&queue_data_lock);
	// Take a snapshot to reduce inconsistency in logged numbers.
	queue_data = *queue;
	const char *thread_status;

	char task_state_report = '-';

	if (queue_data.thread != NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		task_state_report = task_state_to_char(queue->thread);
#else
		unsigned int task_state = queue->thread->state & TASK_REPORT;

		task_state &= 0x1ff;
		unsigned int task_state_index;

		if (task_state != 0) {
			task_state_index = __ffs(task_state) + 1;
			BUG_ON(task_state_index >=
			       sizeof(TASK_STATE_TO_CHAR_STR));
		} else {
			task_state_index = 0;
		}
		task_state_report = TASK_STATE_TO_CHAR_STR[task_state_index];
#endif
	}

	if (queue_data.thread == NULL) {
		thread_status = "no threads";
	} else if (atomic_read(&queue_data.idle)) {
		thread_status = "idle";
	} else {
		thread_status = "running";
	}
	logInfo("workQ %" PRIptr " (%s) %u entries %llu waits, %s (%c)",
		&queue->common,
		queue_data.common.name,
		get_pending_count(&queue_data),
		queue_data.stats.waits,
		thread_status,
		task_state_report);

	log_work_item_stats(&queue_data.stats.work_item_stats);
	log_work_queue_stats(queue);

	mutex_unlock(&queue_data_lock);

	// ->lock spin lock status?
	// ->waiting_worker_threads wait queue status? anyone waiting?
}

/**********************************************************************/
void dump_work_queue(struct kvdo_work_queue *queue)
{
	if (queue->round_robin_mode) {
		struct round_robin_work_queue *round_robin_queue =
			as_round_robin_work_queue(queue);
		unsigned int i;

		for (i = 0;
		     i < round_robin_queue->num_service_queues; i++) {
			dump_simple_work_queue(round_robin_queue->service_queues[i]);
		}
	} else {
		dump_simple_work_queue(as_simple_work_queue(queue));
	}
}

/**********************************************************************/
void dump_work_item_to_buffer(struct kvdo_work_item *item,
			      char *buffer,
			      size_t length)
{
	size_t current_length =
		snprintf(buffer,
			 length,
			 "%.*s/",
			 TASK_COMM_LEN,
			 item->my_queue == NULL ? "-" : item->my_queue->name);
	if (current_length < length) {
		get_function_name(item->stats_function,
				  buffer + current_length,
				  length - current_length);
	}
}

// Work submission

/**********************************************************************/
void enqueue_work_queue(struct kvdo_work_queue *kvdoWorkQueue,
			struct kvdo_work_item *item)
{
	struct simple_work_queue *queue = pick_simple_queue(kvdoWorkQueue);

	if (enqueue_work_queue_item(queue, item)) {
		wake_worker_thread(queue);
	}
}

// Misc


/**********************************************************************/
struct kvdo_work_queue *get_current_work_queue(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue == NULL) ? NULL : &queue->common;
}

/**********************************************************************/
struct kernel_layer *get_work_queue_owner(struct kvdo_work_queue *queue)
{
	return queue->owner;
}

/**********************************************************************/
void *get_work_queue_private_data(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue != NULL) ? queue->private : NULL;
}

/**********************************************************************/
void init_work_queue_once(void)
{
	// We can't use DEFINE_MUTEX because it's not compatible with c99 mode.
	mutex_init(&queue_data_lock);
	init_work_queue_stack_handle_once();
}
