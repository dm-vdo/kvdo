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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueStats.h#17 $
 */

#ifndef WORK_QUEUE_STATS_H
#define WORK_QUEUE_STATS_H

#include "workQueue.h"

#include "timeUtils.h"

#include "histogram.h"
#include "workItemStats.h"

// Defined in workQueueInternals.h after inclusion of workQueueStats.h.
struct simple_work_queue;

/*
 * Tracking statistics.
 *
 * Cache line contention issues:
 *
 * In work_item_stats, there are read-only fields accessed mostly by
 * work submitters, then fields updated by the work submitters (for
 * which there will be contention), then fields rarely if ever updated
 * (more than two cache lines' worth), then fields updated only by the
 * worker thread. The trailing fields here are updated only by the
 * worker thread.
 */
struct vdo_work_queue_stats {
	// Per-work-function counters and optional nanosecond timing data
	struct vdo_work_item_stats work_item_stats;
	// How often we go to sleep waiting for work
	uint64_t waits;

	// Run time data, for monitoring utilization levels.

	// Thread start time, from which we can compute lifetime thus far.
	uint64_t start_time;
	/*
	 * Time the thread has not been blocked waiting for a new work item,
	 * nor in cond_resched(). This will include time the thread has been
	 * blocked by some kernel function invoked by the work functions
	 * (e.g., waiting for socket buffer space).
	 *
	 * This is not redundant with run_time_before_reschedule_histogram, as
	 * the latter doesn't count run time not followed by a cond_resched
	 * call.
	 */
	atomic64_t run_time;
	// Time the thread has been suspended via cond_resched().
	// (Duplicates data hidden within reschedule_time_histogram.)
	atomic64_t reschedule_time;

	// Histogram of the queue times of work items (microseconds)
	struct histogram *queue_time_histogram;
	// How busy we are when cond_resched is called
	struct histogram *reschedule_queue_length_histogram;
	// Histogram of the time cond_resched makes us sleep for (microseconds)
	struct histogram *reschedule_time_histogram;
	// Histogram of the run time between cond_resched calls (microseconds)
	struct histogram *run_time_before_reschedule_histogram;
	// Histogram of the time schedule_timeout lets us sleep for
	// (microseconds)
	struct histogram *schedule_time_histogram;
	// How long from thread wakeup call to thread actually running
	// (microseconds)
	struct histogram *wakeup_latency_histogram;
	// How much work is pending by the time we start running
	struct histogram *wakeup_queue_length_histogram;
};

/**
 * Initialize the work queue's statistics tracking.
 *
 * @param stats           The statistics structure
 * @param queue_kobject   The sysfs directory kobject for the work queue
 *
 * @return  0 or a kernel error code
 **/
int __must_check
initialize_work_queue_stats(struct vdo_work_queue_stats *stats,
			    struct kobject *queue_kobject);

/**
 * Tear down any allocated storage or objects for statistics tracking.
 *
 * @param stats  The statistics structure
 **/
void cleanup_work_queue_stats(struct vdo_work_queue_stats *stats);

/**
 * Update the work queue statistics tracking to note the enqueueing of
 * a work item.
 *
 * @param stats     The statistics structure
 * @param item      The work item being enqueued
 * @param priority  The priority of the work item
 **/
static inline void update_stats_for_enqueue(struct vdo_work_queue_stats *stats,
					    struct vdo_work_item *item,
					    int priority)
{
	update_work_item_stats_for_enqueue(&stats->work_item_stats, item,
					   priority);
	item->enqueue_time = ktime_get_ns();
}

/**
 * Update the work queue statistics tracking to note the dequeueing of
 * a work item.
 *
 * @param stats  The statistics structure
 * @param item   The work item being enqueued
 **/
static inline void update_stats_for_dequeue(struct vdo_work_queue_stats *stats,
					    struct vdo_work_item *item)
{
	update_work_item_stats_for_dequeue(&stats->work_item_stats, item);
	enter_histogram_sample(stats->queue_time_histogram,
			       (ktime_get_ns() - item->enqueue_time) / 1000);
	item->enqueue_time = 0;
}

/**
 * Write the work queue's accumulated statistics to the kernel log.
 *
 * The queue pointer is needed so that its address and name can be
 * logged along with the statistics.
 *
 * @param queue  The work queue
 **/
void log_work_queue_stats(const struct simple_work_queue *queue);

/**
 * Format the thread lifetime, run time, and suspend time into a
 * supplied buffer for reporting via sysfs.
 *
 * @param [in]  stats   The stats structure containing the run-time info
 * @param [out] buffer  The buffer in which to report the info
 **/
ssize_t format_run_time_stats(const struct vdo_work_queue_stats *stats,
			      char *buffer);

#endif // WORK_QUEUE_STATS_H
