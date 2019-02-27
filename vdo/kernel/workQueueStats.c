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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueStats.c#4 $
 */

#include "workQueueStats.h"

#include "atomic.h"
#include "logger.h"
#include "workItemStats.h"
#include "workQueueInternals.h"

/**********************************************************************/
int initialize_work_queue_stats(KvdoWorkQueueStats *stats,
                                struct kobject     *queue_kobject)
{
  spin_lock_init(&stats->workItemStats.function_table.lock);
  if (ENABLE_PER_FUNCTION_TIMING_STATS) {
    for (int i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
      init_simple_stats(&stats->workItemStats.times[i]);
    }
  }

  stats->queueTimeHistogram
    = make_logarithmic_histogram(queue_kobject, "queue_time",
                                 "Queue Time", "work items", "wait time",
                                 "microseconds", 9);
  if (stats->queueTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->rescheduleQueueLengthHistogram
    = make_logarithmic_histogram(queue_kobject, "reschedule_queue_length",
                                 "Reschedule Queue Length", "calls",
                                 "queued work items", NULL, 4);
  if (stats->rescheduleQueueLengthHistogram == NULL) {
    return -ENOMEM;
  }

  stats->rescheduleTimeHistogram
    = make_logarithmic_histogram(queue_kobject, "reschedule_time",
                                 "Reschedule Time", "calls",
                                 "sleep interval", "microseconds", 9);
  if (stats->rescheduleTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->runTimeBeforeRescheduleHistogram
    = make_logarithmic_histogram(queue_kobject, "run_time_before_reschedule",
                                 "Run Time Before Reschedule",
                                 "calls", "run time", "microseconds", 9);
  if (stats->runTimeBeforeRescheduleHistogram == NULL) {
    return -ENOMEM;
  }

  stats->scheduleTimeHistogram
    = make_logarithmic_histogram(queue_kobject, "schedule_time",
                                 "Schedule Time",
                                 "calls", "sleep interval", "microseconds", 9);
  if (stats->scheduleTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->wakeupLatencyHistogram
    = make_logarithmic_histogram(queue_kobject, "wakeup_latency",
                                 "Wakeup Latency",
                                 "wakeups", "latency", "microseconds", 9);
  if (stats->wakeupLatencyHistogram == NULL) {
    return -ENOMEM;
  }

  stats->wakeupQueueLengthHistogram
    = make_logarithmic_histogram(queue_kobject, "wakeup_queue_length",
                                 "Wakeup Queue Length", "wakeups",
                                 "queued work items", NULL, 4);
  if (stats->wakeupQueueLengthHistogram == NULL) {
    return -ENOMEM;
  }

  return 0;
}

/**********************************************************************/
void cleanup_work_queue_stats(KvdoWorkQueueStats *stats)
{
  free_histogram(&stats->queueTimeHistogram);
  free_histogram(&stats->rescheduleQueueLengthHistogram);
  free_histogram(&stats->rescheduleTimeHistogram);
  free_histogram(&stats->runTimeBeforeRescheduleHistogram);
  free_histogram(&stats->scheduleTimeHistogram);
  free_histogram(&stats->wakeupLatencyHistogram);
  free_histogram(&stats->wakeupQueueLengthHistogram);
}

/**********************************************************************/
static uint64_t get_total_processed(const SimpleWorkQueue *queue)
{
  uint64_t total_processed = 0;
  for (int i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
    total_processed += queue->stats.workItemStats.times[i].count;
  }
  return total_processed;
}

/**********************************************************************/
void log_work_queue_stats(const SimpleWorkQueue *queue)
{
  uint64_t runtime_ns = 0;
  if (queue->thread != NULL) {
    runtime_ns += queue->thread->se.sum_exec_runtime;
  }

  unsigned long ns_per_work_item = 0;
  uint64_t total_processed = get_total_processed(queue);
  if (total_processed > 0) {
    ns_per_work_item = runtime_ns / total_processed;
  }
  unsigned long runtime_ms = runtime_ns / 1000;
  logInfo("workQ %" PRIptr " (%s) thread cpu usage %lu.%06lus, %" PRIu64
          " tasks, %lu.%03luus/task",
          queue,
          queue->common.name,
          runtime_ms / 1000000, runtime_ms % 1000000,
          total_processed,
          ns_per_work_item / 1000, ns_per_work_item % 1000);
}

/**********************************************************************/
ssize_t format_run_time_stats(const KvdoWorkQueueStats *stats, char *buffer)
{
  // Get snapshots of all three at approximately the same time.
  uint64_t start_time = stats->startTime;
  uint64_t run_time = atomic64_read(&stats->runTime);
  uint64_t reschedule_time = atomic64_read(&stats->rescheduleTime);
  loadFence();                  // rdtsc barrier
  uint64_t now = currentTime(CT_MONOTONIC);
  uint64_t lifetime = now - start_time;

  return sprintf(buffer,
                 "%llu %llu %llu\n",
                 lifetime, run_time, reschedule_time);
}
