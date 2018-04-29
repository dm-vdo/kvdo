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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/workItemStats.h#1 $
 */

#ifndef WORK_ITEM_STATS_H
#define WORK_ITEM_STATS_H

#include "timeUtils.h"

#include "workQueue.h"

enum {
  // Whether to enable tracking of per-work-function run-time stats.
  ENABLE_PER_FUNCTION_TIMING_STATS = 0,
  // How many work function/priority pairs to track call stats for
  NUM_WORK_QUEUE_ITEM_STATS        = 18,
};

typedef struct simpleStats {
  uint64_t count;
  uint64_t sum;
  uint64_t min;
  uint64_t max;
} SimpleStats;

/*
 * We track numbers of work items handled (and optionally the
 * wall-clock time to run the work functions), broken down by
 * individual work functions (or alternate functions that the caller
 * wants recorded, like the VIO completion callback function if we're
 * just enqueueing a work function that invokes that indirectly) and
 * priority.
 *
 * The first part of this structure manages the function/priority
 * pairs, and is read frequently but updated rarely (once for each
 * pair, plus possibly spin lock contention).
 *
 * The second part holds counters, and is updated often; different
 * parts are updated by various threads as described below. The last
 * element of each array, index NUM_WORK_QUEUE_ITEM_STATS, is updated
 * only if we have filled the arrays and can't add the current work
 * function/priority. See how the statTableIndex field is set in
 * workItemStats.c.
 *
 * All fields may additionally be read when reporting statistics
 * (including optionally reporting stats when the worker thread shuts
 * down), but that's rare and shouldn't significantly affect cache
 * contention issues.
 *
 * There is no "pending" count per work function here. For reporting
 * statistics, it can be approximated by looking at the other fields.
 * Do not rely on them being precise and synchronized, though.
 */
typedef struct kvdoWorkItemStatsFunctionTable {
  /*
   * The spin lock is used to protect .functions and .priorities
   * during updates. All three are modified by producers (enqueueing
   * threads) but only rarely. The .functions and .priorities arrays
   * are read by producers very frequently.
   */
  spinlock_t       lock;
  KvdoWorkFunction functions[NUM_WORK_QUEUE_ITEM_STATS];
  uint8_t          priorities[NUM_WORK_QUEUE_ITEM_STATS];
} KvdoWorkFunctionTable;

typedef struct kvdoWorkItemStats {
  /*
   * Table of functions and priorities, for determining the index to
   * use into the counter arrays below.
   *
   * This table is read by producers (usually multiple entries) for
   * every work item enqueued, and when reporting stats. It is updated
   * by producers, and only the first time a new (work-function,
   * priority) combination is seen.
   */
  KvdoWorkFunctionTable  functionTable;
  // Skip to (somewhere on) the next cache line
  char                   pad[CACHE_LINE_BYTES - sizeof(atomic64_t)];
  /*
   * The .enqueued field is updated by producers only, once per work
   * item processed; __sync operations are used to update these
   * values.
   */
  atomic64_t             enqueued[NUM_WORK_QUEUE_ITEM_STATS + 1];
  /*
   * The .timedOut field is updated by producers (if the timeout has
   * already passed) and a consumer; in this case the consumer is the
   * alarm function (in whatever context). Updates should be rare.
   */
  atomic64_t             timedOut[NUM_WORK_QUEUE_ITEM_STATS + 1];
  /*
   * These values are updated only by the consumer (worker thread). We
   * overload the .times[].count field as a count of items processed,
   * so if we're not doing the optional processing-time tracking
   * (controlled via an option in workQueue.c), we need to explicitly
   * update the count.
   *
   * Since only one thread can ever update these values, no
   * synchronization is used.
   */
  SimpleStats            times[NUM_WORK_QUEUE_ITEM_STATS + 1];
} KvdoWorkItemStats;

/**
 * Initialize a statistics structure for tracking sample
 * values. Assumes the storage was already zeroed out at allocation
 * time.
 *
 * @param stats    The statistics structure
 **/
static inline void initSimpleStats(SimpleStats *stats)
{
  // Assume other fields are initialized to zero at allocation.
  stats->min = UINT64_MAX;
}

/**
 * Update the statistics being tracked for a new sample value.
 *
 * @param stats    The statistics structure
 * @param value    The new value to be folded in
 **/
static inline void addSample(SimpleStats *stats, uint64_t value)
{
  stats->count++;
  stats->sum += value;
  if (stats->min > value) {
    stats->min = value;
  }
  if (stats->max < value) {
    stats->max = value;
  }
}

/**
 * Return the average of the samples collected.
 *
 * @param stats    The statistics structure
 *
 * @return         The average sample value
 **/
static inline uint64_t getSampleAverage(const SimpleStats *stats)
{
  uint64_t slop = stats->count / 2;
  return (stats->sum + slop) / stats->count;
}

/**
 * Update all work queue statistics (work-item and otherwise) after
 * enqueueing a work item.
 *
 * @param  stats     The statistics structure
 * @param  item      The work item enqueued
 * @param  priority  The work item's priority
 **/
void updateWorkItemStatsForEnqueue(KvdoWorkItemStats *stats,
                                   KvdoWorkItem      *item,
                                   int                priority);

/**
 * Update all work queue statistics (work-item and otherwise) after enqueueing
 * a work item.
 *
 * This is a very lightweight function (after optimizing away conditionals and
 * no-ops) and is called for every work item processed, hence the inline
 * definition.
 *
 * This function requires that recordStartTime and
 * updateWorkItemStatsForWorkTime below both get called as well; in some cases
 * counters may be updated in updateWorkItemStatsForWorkTime rather than here.
 *
 * @param  stats  The statistics structure
 * @param  item   The work item enqueued
 **/
static inline void updateWorkItemStatsForDequeue(KvdoWorkItemStats *stats,
                                                 KvdoWorkItem      *item)
{
  // The times[].count field is overloaded as a count of items
  // processed.
  if (!ENABLE_PER_FUNCTION_TIMING_STATS) {
    stats->times[item->statTableIndex].count++;
  } else {
    // In this case, updateWorkItemStatsForWorkTime will bump the counter.
  }
}

/**
 * Update all work queue statistics (work-item and otherwise) after
 * timing out on a work item.
 *
 * @param  stats  The statistics structure
 * @param  item   The work item enqueued
 **/
void updateWorkItemStatsForTimeout(KvdoWorkItemStats *stats,
                                   KvdoWorkItem      *item);

/**
 * Record the starting time for processing a work item, if timing
 * stats are enabled and if we haven't run out of room for recording
 * stats in the table.
 *
 * @param  index  The work item's index into the internal array
 *
 * @return    The current time, or zero
 **/
static inline uint64_t recordStartTime(unsigned int index)
{
  return (ENABLE_PER_FUNCTION_TIMING_STATS ? currentTime(CT_MONOTONIC) : 0);
}

/**
 * Update the work queue statistics with the wall-clock time for
 * processing a work item, if timing stats are enabled and if we
 * haven't run out of room for recording stats in the table.
 *
 * @param  stats      The statistics structure
 * @param  index      The work item's index into the internal array
 * @param  startTime  The start time as reported by recordStartTime
 **/
static inline void updateWorkItemStatsForWorkTime(KvdoWorkItemStats *stats,
                                                  unsigned int       index,
                                                  uint64_t           startTime)
{
  if (ENABLE_PER_FUNCTION_TIMING_STATS) {
    uint64_t endTime = currentTime(CT_MONOTONIC);
    addSample(&stats->times[index], endTime - startTime);
  }
}

/**
 * Convert the pointer into a string representation, using a function
 * name if available.
 *
 * @param pointer       The pointer to be converted
 * @param buffer        The output buffer
 * @param bufferLength  The size of the output buffer
 **/
char *getFunctionName(void *pointer, char *buffer, size_t bufferLength);

/**
 * Dump statistics broken down by work function and priority into the
 * kernel log.
 *
 * @param  stats  The statistics structure
 **/
void logWorkItemStats(const KvdoWorkItemStats *stats);

/**
 * Format counters for per-work-function stats for reporting via /sys.
 *
 * @param [in]  stats   The statistics structure
 * @param [out] buffer  The output buffer
 * @param [in]  length  The size of the output buffer
 *
 * @return  The size of the string actually written
 **/
size_t formatWorkItemStats(const KvdoWorkItemStats *stats,
                           char                    *buffer,
                           size_t                   length);

#endif // WORK_ITEM_STATS_H
