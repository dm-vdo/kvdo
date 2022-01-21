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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/workQueue.c#26 $
 */

#include "workQueue.h"

#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/percpu.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"
#include "stringUtils.h"

#include "statusCodes.h"

#include "workItemStats.h"
#include "workQueueInternals.h"
#include "workQueueStats.h"
#include "workQueueSysfs.h"

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
 * @return A subordinate work queue
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
 * @return a simple work queue
 **/
static inline struct simple_work_queue *
pick_simple_queue(struct vdo_work_queue *queue)
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
 * @return a work item pointer, or NULL
 **/
static struct vdo_work_item *
poll_for_work_item(struct simple_work_queue *queue)
{
	struct vdo_work_item *item = NULL;
	int i;

	for (i = READ_ONCE(queue->num_priority_lists) - 1; i >= 0; i--) {
		struct funnel_queue_entry *link =
			funnel_queue_poll(queue->priority_lists[i]);
		if (link != NULL) {
			item = container_of(link,
					    struct vdo_work_item,
					    work_queue_entry_link);
			break;
		}
	}

	return item;
}

/**
 * Add a work item into the queue and wake the worker thread if it is waiting.
 *
 * @param queue  The work queue
 * @param item   The work item to add
 **/
static void enqueue_work_queue_item(struct simple_work_queue *queue,
				    struct vdo_work_item *item)
{
	unsigned int priority;

	ASSERT_LOG_ONLY(item->my_queue == NULL,
			"item %px (fn %px/%px) to enqueue (%px) is not already queued (%px)",
			item, item->work, item->stats_function, queue,
			item->my_queue);
	if (ASSERT(item->action < WORK_QUEUE_ACTION_COUNT,
		   "action is in range for queue") != VDO_SUCCESS) {
		item->action = 0;
	}
	priority = READ_ONCE(queue->priority_map[item->action]);

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
	 * number of thread wakeups. Using an idle flag, and careful ordering
	 * using memory barriers, we should be able to determine when the
	 * worker thread might be asleep or going to sleep. We use cmpxchg to
	 * try to take ownership (vs other producer threads) of the
	 * responsibility for waking the worker thread, so multiple wakeups
	 * aren't tried at once.
	 *
	 * This was tuned for some x86 boxes that were handy; it's untested
	 * whether doing the read first is any better or worse for other
	 * platforms, even other x86 configurations.
	 */
	smp_mb();
	if ((atomic_read(&queue->idle) != 1) ||
	    (atomic_cmpxchg(&queue->idle, 1, 0) != 1)) {
		return;
	}

	atomic64_cmpxchg(&queue->first_wakeup, 0, ktime_get_ns());

	// Despite the name, there's a maximum of one thread in this list.
	wake_up(&queue->waiting_worker_threads);
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
 * @param queue  The work queue to wait on
 *
 * @return the next work item, or NULL to indicate shutdown is requested
 **/
static struct vdo_work_item *
wait_for_next_work_item(struct simple_work_queue *queue)
{
	struct vdo_work_item *item;
	DEFINE_WAIT(wait);

	while (true) {
		uint64_t time_before_schedule, schedule_time_ns, run_time_ns;

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
		smp_mb(); // store-load barrier between "idle" and funnel queue

		item = poll_for_work_item(queue);
		if (item != NULL) {
			break;
		}

		/*
		 * We need to check for thread-stop after setting
		 * TASK_INTERRUPTIBLE state up above. Otherwise, schedule()
		 * will put the thread to sleep and might miss a wakeup from
		 * kthread_stop() call in finish_work_queue().
		 */
		if (kthread_should_stop()) {
			break;
		}

		time_before_schedule = ktime_get_ns();
		run_time_ns = time_before_schedule - queue->most_recent_wakeup;
		// These stats are read from other threads, but are only
		// written by this thread.
		WRITE_ONCE(queue->stats.waits, queue->stats.waits + 1);
		WRITE_ONCE(queue->stats.run_time,
			   queue->stats.run_time + run_time_ns);

		schedule();

		queue->most_recent_wakeup = ktime_get_ns();
		schedule_time_ns = (queue->most_recent_wakeup
				    - time_before_schedule);
		enter_histogram_sample(queue->stats.schedule_time_histogram,
				       schedule_time_ns / 1000);

		/*
		 * Check again before resetting first_wakeup for more accurate
		 * stats. If it was a spurious wakeup, continue looping.
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
		smp_rmb();
		if (first_wakeup != 0) {
			enter_histogram_sample(
				queue->stats.wakeup_latency_histogram,
				(ktime_get_ns() - first_wakeup) / 1000);
			enter_histogram_sample(
				queue->stats.wakeup_queue_length_histogram,
				count_vdo_work_items_pending(
					&queue->stats.work_item_stats));
		}
	}
	finish_wait(&queue->waiting_worker_threads, &wait);
	atomic_set(&queue->idle, 0);

	return item;
}

/**
 * Execute a work item from a work queue, and do associated bookkeeping.
 *
 * @param queue  the work queue the item is from
 * @param item   the work item to run
 **/
static void process_work_item(struct simple_work_queue *queue,
			      struct vdo_work_item *item)
{
	uint64_t dequeue_time = update_stats_for_dequeue(&queue->stats, item);
	// Save the index, so we can use it after the work function.
	unsigned int index = item->stat_table_index;

	if (ASSERT(item->my_queue == &queue->common,
		   "item %px from queue %px marked as being in this queue (%px)",
		   item, queue, item->my_queue) == UDS_SUCCESS) {
		item->my_queue = NULL;
	}

	item->work(item);
	// We just surrendered control of the work item; no more access.
	item = NULL;

	update_vdo_work_item_stats_for_work_time(&queue->stats.work_item_stats,
						 index,
						 dequeue_time);
}

/**
 * Yield the CPU to the scheduler and update queue statistics accordingly.
 *
 * @param queue  The active queue
 **/
static void yield_to_scheduler(struct simple_work_queue *queue)
{
	unsigned int queue_length;
	uint64_t run_time_ns, reschedule_time_ns;
	uint64_t time_before_reschedule, time_after_reschedule;
	struct vdo_work_queue_stats *stats = &queue->stats;

	/*
	 * Record the queue length we have *before* rescheduling.
	 * N.B.: We compute the pending count info here without any
	 * synchronization, but it's for stats reporting only, so being
	 * imprecise isn't too big a deal.
	 */
	queue_length = count_vdo_work_items_pending(&stats->work_item_stats);

	time_before_reschedule = ktime_get_ns();
	cond_resched();
	time_after_reschedule = ktime_get_ns();

	enter_histogram_sample(stats->reschedule_queue_length_histogram,
			       queue_length);

	run_time_ns = time_before_reschedule - queue->most_recent_wakeup;
	enter_histogram_sample(stats->run_time_before_reschedule_histogram,
			       run_time_ns / 1000);
	WRITE_ONCE(stats->run_time, stats->run_time + run_time_ns);

	reschedule_time_ns = time_after_reschedule - time_before_reschedule;
	enter_histogram_sample(stats->reschedule_time_histogram,
			       reschedule_time_ns / 1000);
	WRITE_ONCE(stats->reschedule_time,
		   stats->reschedule_time + reschedule_time_ns);

	queue->most_recent_wakeup = time_after_reschedule;
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
	run_start_hook(queue);

	while (true) {
		struct vdo_work_item *item = poll_for_work_item(queue);
		if (item == NULL) {
			item = wait_for_next_work_item(queue);
		}

		if (item == NULL) {
			// No work items but kthread_should_stop was triggered.
			break;
		}

		process_work_item(queue, item);

		/*
		 * Be friendly to a CPU that has other work to do, if the
		 * kernel has told us to. This speeds up some performance
		 * tests; that "other work" might include other VDO threads.
		 */
		if (need_resched()) {
			yield_to_scheduler(queue);
		}
	}

	run_finish_hook(queue);
}

/**
 * Initialize per-thread data for a new worker thread and run the work queue.
 * Called in a new thread created by kthread_run().
 *
 * @param ptr  A pointer to the vdo_work_queue to run.
 *
 * @return 0 (indicating success to kthread_run())
 **/
static int work_queue_runner(void *ptr)
{
	struct simple_work_queue *queue = ptr;
	unsigned long flags;

	queue->stats.start_time = queue->most_recent_wakeup = ktime_get_ns();

	spin_lock_irqsave(&queue->lock, flags);
	queue->started = true;
	spin_unlock_irqrestore(&queue->lock, flags);

	wake_up(&queue->start_waiters);
	service_work_queue(queue);

	return 0;
}

// Preparing work items

/**********************************************************************/
void setup_work_item(struct vdo_work_item *item,
		     vdo_work_function work,
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

// Creation & teardown

/**********************************************************************/
static bool queue_started(struct simple_work_queue *queue)
{
	unsigned long flags;
	bool started;

	spin_lock_irqsave(&queue->lock, flags);
	started = queue->started;
	spin_unlock_irqrestore(&queue->lock, flags);

	return started;
}

/**
 * Create a simple work queue with a worker thread.
 *
 * @param [in]  thread_name_prefix The per-device prefix to use in
 *                                 thread names
 * @param [in]  name               The queue name
 * @param [in]  owner              The VDO owning the work queue
 * @param [in]  private            Private data of the queue for use by work
 *                                 items or other queue-specific functions
 * @param [in]  type               The work queue type defining the lifecycle
 *                                 functions, queue actions, priorities, and
 *                                 timeout behavior
 * @param [out] queue_ptr          Where to store the queue handle
 *
 * @return VDO_SUCCESS or an error code
 **/
static int make_simple_work_queue(const char *thread_name_prefix,
				  const char *name,
				  struct vdo *owner,
				  void *private,
				  const struct vdo_work_queue_type *type,
				  struct simple_work_queue **queue_ptr)
{
	struct simple_work_queue *queue;
	unsigned int num_priority_lists = 1;
	int i;
	struct task_struct *thread = NULL;

	int result = UDS_ALLOCATE(1,
				  struct simple_work_queue,
				  "simple work queue",
				  &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	queue->type = type;
	queue->private = private;
	queue->common.owner = owner;

	for (i = 0; i < WORK_QUEUE_ACTION_COUNT; i++) {
		const struct vdo_work_queue_action *action =
			&queue->type->action_table[i];
		unsigned int code, priority;
		if (action->name == NULL) {
			break;
		}
		code = action->code;
		priority = action->priority;

		result = ASSERT(
			code < WORK_QUEUE_ACTION_COUNT,
			"invalid action code %u in work queue initialization",
			code);
		if (result != VDO_SUCCESS) {
			UDS_FREE(queue);
			return result;
		}
		result = ASSERT(
			priority < WORK_QUEUE_PRIORITY_COUNT,
			"invalid action priority %u in work queue initialization",
			priority);
		if (result != VDO_SUCCESS) {
			UDS_FREE(queue);
			return result;
		}
		queue->priority_map[code] = priority;
		if (num_priority_lists <= priority) {
			num_priority_lists = priority + 1;
		}
	}

	result = uds_duplicate_string(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		UDS_FREE(queue);
		return -ENOMEM;
	}

	init_waitqueue_head(&queue->waiting_worker_threads);
	init_waitqueue_head(&queue->start_waiters);
	spin_lock_init(&queue->lock);

	queue->num_priority_lists = num_priority_lists;
	for (i = 0; i < WORK_QUEUE_PRIORITY_COUNT; i++) {
		result = make_funnel_queue(&queue->priority_lists[i]);
		if (result != UDS_SUCCESS) {
			free_simple_work_queue(queue);
			return result;
		}
	}
	result = initialize_work_queue_stats(&queue->stats, NULL);
	if (result != 0) {
		uds_log_error("Cannot initialize statistics tracking: %d",
			      result);
		free_simple_work_queue(queue);
		return result;
	}

	queue->started = false;

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
	WRITE_ONCE(queue->thread_pid, thread->pid);

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
		    struct vdo *owner,
		    void *private,
		    const struct vdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct vdo_work_queue **queue_ptr)
{
	struct round_robin_work_queue *queue;
	int result;
	char thread_name[TASK_COMM_LEN];
	unsigned int i;

	if (thread_count == 1) {
		struct simple_work_queue *simple_queue;
		void *context = (thread_privates != NULL) ? thread_privates[0] :
							    private;
		result = make_simple_work_queue(thread_name_prefix,
					        name,
					        owner,
					        context,
					        type,
					        &simple_queue);
		if (result == VDO_SUCCESS) {
			*queue_ptr = &simple_queue->common;
		}
		return result;
	}

	result = UDS_ALLOCATE(1, struct round_robin_work_queue,
			      "round-robin work queue", &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(thread_count,
			      struct simple_work_queue *,
			      "subordinate work queues",
			      &queue->service_queues);
	if (result != UDS_SUCCESS) {
		UDS_FREE(queue);
		return result;
	}

	queue->num_service_queues = thread_count;
	queue->common.round_robin_mode = true;
	queue->common.owner = owner;

	result = uds_duplicate_string(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		UDS_FREE(queue->service_queues);
		UDS_FREE(queue);
		return -ENOMEM;
	}

	*queue_ptr = &queue->common;

	for (i = 0; i < thread_count; i++) {
		void *context = (thread_privates != NULL) ? thread_privates[i] :
							    private;
		snprintf(thread_name, sizeof(thread_name), "%s%u", name, i);
		result = make_simple_work_queue(thread_name_prefix,
						thread_name,
						owner,
						context,
						type,
						&queue->service_queues[i]);
		if (result != VDO_SUCCESS) {
			queue->num_service_queues = i;
			// Destroy previously created subordinates.
			free_work_queue(UDS_FORGET(*queue_ptr));
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
	if (queue->thread == NULL) {
		return;
	}

	// Reduces (but does not eliminate) the chance of the sysfs support
	// reporting the pid even after the thread is gone.
	WRITE_ONCE(queue->thread_pid, 0);

	// Tells the worker thread to shut down and waits for it to exit.
	kthread_stop(queue->thread);
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
void finish_work_queue(struct vdo_work_queue *queue)
{
	if (queue == NULL) {
		return;
	}

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
	UDS_FREE(queue->common.name);
	UDS_FREE(queue);
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
	unsigned int i;

	queue->service_queues = NULL;

	for (i = 0; i < count; i++) {
		free_simple_work_queue(queue_table[i]);
	}
	UDS_FREE(queue_table);
	UDS_FREE(queue->common.name);
	UDS_FREE(queue);
}

/**********************************************************************/
void free_work_queue(struct vdo_work_queue *queue)
{
	if (queue == NULL) {
		return;
	}

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
	const char *thread_status = "no threads";
	char task_state_report = '-';

	if (queue->thread != NULL) {
		task_state_report = task_state_to_char(queue->thread);
		thread_status = atomic_read(&queue->idle) ? "idle" : "running";
	}

	uds_log_info("workQ %px (%s) %u entries %llu waits, %s (%c)",
		     &queue->common,
		     queue->common.name,
		     count_vdo_work_items_pending(&queue->stats.work_item_stats),
		     READ_ONCE(queue->stats.waits),
		     thread_status,
		     task_state_report);

	log_vdo_work_item_stats(&queue->stats.work_item_stats);
	log_work_queue_stats(queue);

	// ->lock spin lock status?
	// ->waiting_worker_threads wait queue status? anyone waiting?
}

/**********************************************************************/
void dump_work_queue(struct vdo_work_queue *queue)
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
void dump_work_item_to_buffer(struct vdo_work_item *item,
			      char *buffer,
			      size_t length)
{
	size_t current_length =
		scnprintf(buffer,
			  length,
			  "%.*s/",
			  TASK_COMM_LEN,
			  item->my_queue == NULL ? "-" : item->my_queue->name);
	if (current_length < length) {
		vdo_get_function_name(item->stats_function,
				      buffer + current_length,
				      length - current_length);
	}
}

// Work submission

/**********************************************************************/
void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_work_item *item)
{
	enqueue_work_queue_item(pick_simple_queue(queue), item);
}

// Misc


/**
 * Return the work queue pointer recorded at initialization time in
 * the work-queue stack handle initialized on the stack of the current
 * thread, if any.
 *
 * @return the work queue pointer, or NULL
 **/
static struct simple_work_queue *get_current_thread_work_queue(void)
{
	/*
	 * The kthreadd process has the PF_KTHREAD flag set but a null
	 * "struct kthread" pointer, which breaks the (initial)
	 * implementation of kthread_func, which assumes the pointer
	 * is always non-null. This matters if memory reclamation is
	 * triggered and causes calls into VDO that get
	 * here. [VDO-5194]
	 *
	 * There might also be a similar reclamation issue in the
	 * usermodehelper code path before exec is called, and/or
	 * kthread setup when allocating the kthread struct itself.
	 *
	 * So we check for the null pointer first. The kthread code
	 * overloads the set_child_tid field to use for its pointer in
	 * PF_KTHREAD processes. (If PF_KTHREAD is clear, kthread_func
	 * will return null anyway so we needn't worry about that
	 * case.)
	 *
	 * FIXME: When submitting upstream, make sure kthread_func is
	 * fixed instead, and drop this check.
	 */
	if (current->set_child_tid == NULL) {
		return NULL;
	}

	if (kthread_func(current) != work_queue_runner) {
		// Not a VDO workQueue thread.
		return NULL;
	}
	return kthread_data(current);
}

/**********************************************************************/
struct vdo_work_queue *get_current_work_queue(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue == NULL) ? NULL : &queue->common;
}

/**********************************************************************/
struct vdo *get_work_queue_owner(struct vdo_work_queue *queue)
{
	return queue->owner;
}

/**********************************************************************/
void *get_work_queue_private_data(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue != NULL) ? queue->private : NULL;
}
