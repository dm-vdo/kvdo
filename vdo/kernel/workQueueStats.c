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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueStats.c#1 $
 */

#include "workQueueStats.h"

#include "atomic.h"
#include "logger.h"
#include "workItemStats.h"
#include "workQueueInternals.h"

/**********************************************************************/
int initializeWorkQueueStats(KvdoWorkQueueStats *stats,
                             struct kobject     *queueKObject)
{
  spin_lock_init(&stats->workItemStats.functionTable.lock);
  if (ENABLE_PER_FUNCTION_TIMING_STATS) {
    for (int i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
      initSimpleStats(&stats->workItemStats.times[i]);
    }
  }

  stats->queueTimeHistogram
    = makeLogarithmicHistogram(queueKObject, "queue_time",
                               "Queue Time", "work items", "wait time",
                               "microseconds", 9);
  if (stats->queueTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->rescheduleQueueLengthHistogram
    = makeLogarithmicHistogram(queueKObject, "reschedule_queue_length",
                               "Reschedule Queue Length", "calls",
                               "queued work items", NULL, 4);
  if (stats->rescheduleQueueLengthHistogram == NULL) {
    return -ENOMEM;
  }

  stats->rescheduleTimeHistogram
    = makeLogarithmicHistogram(queueKObject, "reschedule_time",
                               "Reschedule Time", "calls",
                               "sleep interval", "microseconds", 9);
  if (stats->rescheduleTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->runTimeBeforeRescheduleHistogram
    = makeLogarithmicHistogram(queueKObject, "run_time_before_reschedule",
                               "Run Time Before Reschedule",
                               "calls", "run time", "microseconds", 9);
  if (stats->runTimeBeforeRescheduleHistogram == NULL) {
    return -ENOMEM;
  }

  stats->scheduleTimeHistogram
    = makeLogarithmicHistogram(queueKObject, "schedule_time",
                               "Schedule Time",
                               "calls", "sleep interval", "microseconds", 9);
  if (stats->scheduleTimeHistogram == NULL) {
    return -ENOMEM;
  }

  stats->wakeupLatencyHistogram
    = makeLogarithmicHistogram(queueKObject, "wakeup_latency",
                               "Wakeup Latency",
                               "wakeups", "latency", "microseconds", 9);
  if (stats->wakeupLatencyHistogram == NULL) {
    return -ENOMEM;
  }

  stats->wakeupQueueLengthHistogram
    = makeLogarithmicHistogram(queueKObject, "wakeup_queue_length",
                               "Wakeup Queue Length", "wakeups",
                               "queued work items", NULL, 4);
  if (stats->wakeupQueueLengthHistogram == NULL) {
    return -ENOMEM;
  }

  return 0;
}

/**********************************************************************/
void cleanupWorkQueueStats(KvdoWorkQueueStats *stats)
{
  freeHistogram(&stats->queueTimeHistogram);
  freeHistogram(&stats->rescheduleQueueLengthHistogram);
  freeHistogram(&stats->rescheduleTimeHistogram);
  freeHistogram(&stats->runTimeBeforeRescheduleHistogram);
  freeHistogram(&stats->scheduleTimeHistogram);
  freeHistogram(&stats->wakeupLatencyHistogram);
  freeHistogram(&stats->wakeupQueueLengthHistogram);
}

/**********************************************************************/
static uint64_t getTotalProcessed(const SimpleWorkQueue *queue)
{
  uint64_t totalProcessed = 0;
  for (int i = 0; i < NUM_WORK_QUEUE_ITEM_STATS + 1; i++) {
    totalProcessed += queue->stats.workItemStats.times[i].count;
  }
  return totalProcessed;
}

/**********************************************************************/
void logWorkQueueStats(const SimpleWorkQueue *queue)
{
  uint64_t runtimeNS = 0;
  if (queue->thread != NULL) {
    runtimeNS += queue->thread->se.sum_exec_runtime;
  }

  unsigned long nsPerWorkItem = 0;
  uint64_t totalProcessed = getTotalProcessed(queue);
  if (totalProcessed > 0) {
    nsPerWorkItem = runtimeNS / totalProcessed;
  }
  unsigned long runtimeMS = runtimeNS / 1000;
  logInfo("workQ %" PRIptr " (%s) thread cpu usage %lu.%06lus, %" PRIu64
          " tasks, %lu.%03luus/task",
          queue,
          queue->common.name,
          runtimeMS / 1000000, runtimeMS % 1000000,
          totalProcessed,
          nsPerWorkItem / 1000, nsPerWorkItem % 1000);
}

/**********************************************************************/
ssize_t formatRunTimeStats(const KvdoWorkQueueStats *stats, char *buffer)
{
  // Get snapshots of all three at approximately the same time.
  uint64_t startTime = stats->startTime;
  uint64_t runTime = atomic64_read(&stats->runTime);
  uint64_t rescheduleTime = atomic64_read(&stats->rescheduleTime);
  loadFence();                  // rdtsc barrier
  uint64_t now = currentTime(CT_MONOTONIC);
  uint64_t lifetime = now - startTime;

  return sprintf(buffer,
                 "%llu %llu %llu\n",
                 lifetime, runTime, rescheduleTime);
}
