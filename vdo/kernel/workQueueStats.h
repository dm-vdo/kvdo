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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/workQueueStats.h#2 $
 */

#ifndef WORK_QUEUE_STATS_H
#define WORK_QUEUE_STATS_H

#include "workQueue.h"

#include "timeUtils.h"

#include "histogram.h"
#include "workItemStats.h"

// Defined in workQueueInternals.h after inclusion of workQueueStats.h.
struct simpleWorkQueue;

/*
 * Tracking statistics.
 *
 * Cache line contention issues:
 *
 * In workItemStats, there are read-only fields accessed mostly by
 * work submitters, then fields updated by the work submitters (for
 * which there will be contention), then fields rarely if ever updated
 * (more than two cache lines' worth), then fields updated only by the
 * worker thread. The trailing fields here are updated only by the
 * worker thread.
 */
typedef struct kvdoWorkQueueStats {
  // Per-work-function counters and optional nanosecond timing data
  KvdoWorkItemStats  workItemStats;
  // How often we go to sleep waiting for work
  uint64_t           waits;

  // Run time data, for monitoring utilization levels.

  // Thread start time, from which we can compute lifetime thus far.
  uint64_t           startTime;
  /*
   * Time the thread has not been blocked waiting for a new work item,
   * nor in cond_resched(). This will include time the thread has been
   * blocked by some kernel function invoked by the work functions
   * (e.g., waiting for socket buffer space).
   *
   * This is not redundant with runTimeBeforeRescheduleHistogram, as
   * the latter doesn't count run time not followed by a cond_resched
   * call.
   */
  atomic64_t         runTime;
  // Time the thread has been suspended via cond_resched().
  // (Duplicates data hidden within rescheduleTimeHistogram.)
  atomic64_t         rescheduleTime;

  // Histogram of the queue times of work items (microseconds)
  Histogram         *queueTimeHistogram;
  // How busy we are when cond_resched is called
  Histogram         *rescheduleQueueLengthHistogram;
  // Histogram of the time cond_resched makes us sleep for (microseconds)
  Histogram         *rescheduleTimeHistogram;
  // Histogram of the run time between cond_resched calls (microseconds)
  Histogram         *runTimeBeforeRescheduleHistogram;
  // Histogram of the time schedule_timeout lets us sleep for (microseconds)
  Histogram         *scheduleTimeHistogram;
  // How long from thread wakeup call to thread actually running (microseconds)
  Histogram         *wakeupLatencyHistogram;
  // How much work is pending by the time we start running
  Histogram         *wakeupQueueLengthHistogram;
} KvdoWorkQueueStats;

/**
 * Initialize the work queue's statistics tracking.
 *
 * @param stats         The statistics structure
 * @param queueKObject  The sysfs directory kobject for the work queue
 *
 * @return  0 or a kernel error code
 **/
int initializeWorkQueueStats(KvdoWorkQueueStats *stats,
                             struct kobject     *queueKObject)
  __attribute__((warn_unused_result));

/**
 * Tear down any allocated storage or objects for statistics tracking.
 *
 * @param stats  The statistics structure
 **/
void cleanupWorkQueueStats(KvdoWorkQueueStats *stats);

/**
 * Update the work queue statistics tracking to note the enqueueing of
 * a work item.
 *
 * @param stats     The statistics structure
 * @param item      The work item being enqueued
 * @param priority  The priority of the work item
 **/
static inline void updateStatsForEnqueue(KvdoWorkQueueStats *stats,
                                         KvdoWorkItem       *item,
                                         int                 priority)
{
  updateWorkItemStatsForEnqueue(&stats->workItemStats, item, priority);
  item->enqueueTime = currentTime(CLOCK_MONOTONIC);
}

/**
 * Update the work queue statistics tracking to note the dequeueing of
 * a work item.
 *
 * @param stats  The statistics structure
 * @param item   The work item being enqueued
 **/
static inline void updateStatsForDequeue(KvdoWorkQueueStats *stats,
                                         KvdoWorkItem       *item)
{
  updateWorkItemStatsForDequeue(&stats->workItemStats, item);
  enterHistogramSample(stats->queueTimeHistogram,
                       (currentTime(CLOCK_MONOTONIC) - item->enqueueTime) / 1000);
  item->enqueueTime = 0;
}

/**
 * Write the work queue's accumulated statistics to the kernel log.
 *
 * The queue pointer is needed so that its address and name can be
 * logged along with the statistics.
 *
 * @param queue  The work queue
 **/
void logWorkQueueStats(const struct simpleWorkQueue *queue);

/**
 * Format the thread lifetime, run time, and suspend time into a
 * supplied buffer for reporting via sysfs.
 *
 * @param [in]  stats   The stats structure containing the run-time info
 * @param [out] buffer  The buffer in which to report the info
 **/
ssize_t formatRunTimeStats(const KvdoWorkQueueStats *stats, char *buffer);

#endif // WORK_QUEUE_STATS_H
