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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/workItemStats.c#4 $
 */

#include "workItemStats.h"

#include "atomic.h"
#include "logger.h"

/**
 * Scan the work queue stats table for the provided work function and
 * priority value. If it's not found, see if an empty slot is
 * available.
 *
 * @param table       The work queue's function table
 * @param work        The function we want to record stats for
 * @param priority    The priority of the work item
 *
 * @return   The index of the slot to use (matching or empty), or
 *           NUM_WORK_QUEUE_ITEM_STATS if the table is full of
 *           non-matching entries.
 **/
static inline unsigned int scanStatTable(const KvdoWorkFunctionTable *table,
                                         KvdoWorkFunction             work,
                                         unsigned int                 priority)
{
  unsigned int i;
  /*
   * See comments in getStatTableIndex regarding order of memory
   * accesses. Work function first, then a barrier, then priority.
   */
  for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
    if (table->functions[i] == NULL) {
      return i;
    } else if (table->functions[i] == work) {
      smp_rmb();
      if (table->priorities[i] == priority) {
        return i;
      }
    }
  }
  return NUM_WORK_QUEUE_ITEM_STATS;
}

/**
 * Scan the work queue stats table for the provided work function and
 * priority value. Assign an empty slot if necessary.
 *
 * @param stats       The stats structure
 * @param work        The function we want to record stats for
 * @param priority    The priority of the work item
 *
 * @return   The index of the matching slot, or NUM_WORK_QUEUE_ITEM_STATS
 *           if the table is full of non-matching entries.
 **/
static unsigned int getStatTableIndex(KvdoWorkItemStats *stats,
                                      KvdoWorkFunction   work,
                                      unsigned int       priority)
{
  KvdoWorkFunctionTable *functionTable = &stats->functionTable;

  unsigned int index = scanStatTable(functionTable, work, priority);
  if (unlikely(index == NUM_WORK_QUEUE_ITEM_STATS)
      || likely(functionTable->functions[index] != NULL)) {
    return index;
  }

  unsigned long flags = 0;
  // The delayed-work-item processing uses queue->lock in some cases,
  // and one case may call into this function, so we can't reuse
  // queue->lock here.
  spin_lock_irqsave(&functionTable->lock, flags);
  // Recheck now that we've got the lock...
  index = scanStatTable(functionTable, work, priority);
  if ((index == NUM_WORK_QUEUE_ITEM_STATS)
      || (functionTable->functions[index] != NULL)) {
    spin_unlock_irqrestore(&functionTable->lock, flags);
    return index;
  }

  /*
   * An uninitialized priority is indistinguishable from a zero
   * priority. So store the priority first, and enforce the ordering,
   * so that a non-null work function pointer indicates we've finished
   * filling in the value. (And, to make this work, we have to read
   * the work function first and priority second, when comparing.)
   */
  functionTable->priorities[index] = priority;
  smp_wmb();
  functionTable->functions[index] = work;
  spin_unlock_irqrestore(&functionTable->lock, flags);
  return index;
}

/**
 * Get counters on work items, identified by index into the internal
 * array.
 *
 * @param [in]  stats         The collected statistics
 * @param [in]  index         The index
 * @param [out] enqueuedPtr   The total work items enqueued
 * @param [out] processedPtr  The number of work items processed
 * @param [out] pendingPtr    The number of work items still pending
 **/
static void getWorkItemCountsByItem(const KvdoWorkItemStats *stats,
                                    unsigned int             index,
                                    uint64_t                *enqueuedPtr,
                                    uint64_t                *processedPtr,
                                    unsigned int            *pendingPtr)
{
  uint64_t enqueued  = atomic64_read(&stats->enqueued[index]);
  uint64_t processed = stats->times[index].count;
  unsigned int pending;
  if (enqueued < processed) {
    // Probably just out of sync.
    pending = 1;
  } else {
    pending = enqueued - processed;
    // Pedantic paranoia: Check for overflow of the 32-bit "pending".
    if ((pending + processed) < enqueued) {
      pending = UINT_MAX;
    }
  }
  *enqueuedPtr  = enqueued;
  *processedPtr = processed;
  *pendingPtr   = pending;
}

/**
 * Get counters on work items not covered by any index value.
 *
 * @param [in]  stats         The collected statistics
 * @param [out] enqueuedPtr   The total work items enqueued
 * @param [out] processedPtr  The number of work items processed
 **/
static void getOtherWorkItemCounts(const KvdoWorkItemStats *stats,
                                   uint64_t                *enqueuedPtr,
                                   uint64_t                *processedPtr)
{
  unsigned int pending;
  getWorkItemCountsByItem(stats, NUM_WORK_QUEUE_ITEM_STATS,
                          enqueuedPtr, processedPtr, &pending);
}

/**
 * Get timing stats on work items, identified by index into the
 * internal array.
 *
 * @param [in]  stats  The collected statistics
 * @param [in]  index  The index into the array
 * @param [out] min    The minimum execution time
 * @param [out] mean   The mean execution time
 * @param [out] max    The maximum execution time
 **/
static void getWorkItemTimesByItem(const KvdoWorkItemStats *stats,
                                   unsigned int             index,
                                   uint64_t                *min,
                                   uint64_t                *mean,
                                   uint64_t                *max)
{
  *min  = stats->times[index].min;
  *mean = getSampleAverage(&stats->times[index]);
  *max  = stats->times[index].max;
}

/**********************************************************************/
void updateWorkItemStatsForEnqueue(KvdoWorkItemStats *stats,
                                   KvdoWorkItem      *item,
                                   int                priority)
{
  item->statTableIndex = getStatTableIndex(stats, item->statsFunction,
                                           priority);
  atomic64_add(1, &stats->enqueued[item->statTableIndex]);
}

/**********************************************************************/
char *getFunctionName(void *pointer, char *buffer, size_t bufferLength)
{
  if (pointer == NULL) {
    /*
     * Format "%ps" logs a null pointer as "(null)" with a bunch of
     * leading spaces. We sometimes use this when logging lots of
     * data; don't be so verbose.
     */
    strncpy(buffer, "-", bufferLength);
  } else {
    /*
     * Use a non-const array instead of a string literal below to
     * defeat gcc's format checking, which doesn't understand that
     * "%ps" actually does support a precision spec in Linux kernel
     * code.
     */
    static char truncatedFunctionNameFormatString[] = "%.*ps";
    snprintf(buffer, bufferLength,
             truncatedFunctionNameFormatString,
             bufferLength - 1,
             pointer);

    char *space = strchr(buffer, ' ');
    if (space != NULL) {
      *space = '\0';
    }
  }

  return buffer;
}

/**********************************************************************/
size_t formatWorkItemStats(const KvdoWorkItemStats *stats,
                           char                    *buffer,
                           size_t                   length)
{
  const KvdoWorkFunctionTable *functionIDs = &stats->functionTable;
  size_t currentOffset = 0;

  uint64_t enqueued, processed;
  int i;
  for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
    if (functionIDs->functions[i] == NULL) {
      break;
    }
    if (atomic64_read(&stats->enqueued[i]) == 0) {
      continue;
    }
    /*
     * The reporting of all of "pending", "enqueued" and "processed"
     * here seems redundant, but "pending" is limited to 0 in the case
     * where "processed" exceeds "enqueued", either through current
     * activity and a lack of synchronization when fetching stats, or
     * a coding bug. This report is intended largely for debugging, so
     * we'll go ahead and print the not-necessarily-redundant values.
     */
    unsigned int pending;
    getWorkItemCountsByItem(stats, i, &enqueued, &processed, &pending);

    // Format: fn prio enq proc timeo [ min max mean ]
    if (ENABLE_PER_FUNCTION_TIMING_STATS) {
      uint64_t min, mean, max;
      getWorkItemTimesByItem(stats, i, &min, &mean, &max);
      currentOffset += snprintf(buffer + currentOffset,
                                length - currentOffset,
                                "%-36ps %d %10" PRIu64 " %10" PRIu64
                                " %10" PRIu64 " %10" PRIu64 " %10" PRIu64
                                "\n",
                                functionIDs->functions[i],
                                functionIDs->priorities[i],
                                enqueued, processed,
                                min, max, mean);
    } else {
      currentOffset += snprintf(buffer + currentOffset,
                                length - currentOffset,
                                "%-36ps %d %10" PRIu64 " %10" PRIu64
                                "\n",
                                functionIDs->functions[i],
                                functionIDs->priorities[i],
                                enqueued, processed);
    }
    if (currentOffset >= length) {
      break;
    }
  }
  if ((i == NUM_WORK_QUEUE_ITEM_STATS) && (currentOffset < length)) {
    uint64_t enqueued, processed;
    getOtherWorkItemCounts(stats, &enqueued, &processed);
    if (enqueued > 0) {
      currentOffset += snprintf(buffer + currentOffset,
                                length - currentOffset,
                                "%-36s %d %10" PRIu64 " %10" PRIu64
                                "\n",
                                "OTHER", 0,
                                enqueued, processed);
    }
  }
  return currentOffset;
}

/**********************************************************************/
void logWorkItemStats(const KvdoWorkItemStats *stats)
{
  uint64_t totalEnqueued = 0;
  uint64_t totalProcessed = 0;

  const KvdoWorkFunctionTable *functionIDs = &stats->functionTable;

  int i;
  for (i = 0; i < NUM_WORK_QUEUE_ITEM_STATS; i++) {
    if (functionIDs->functions[i] == NULL) {
      break;
    }
    if (atomic64_read(&stats->enqueued[i]) == 0) {
      continue;
    }
    /*
     * The reporting of all of "pending", "enqueued" and "processed"
     * here seems redundant, but "pending" is limited to 0 in the case
     * where "processed" exceeds "enqueued", either through current
     * activity and a lack of synchronization when fetching stats, or
     * a coding bug. This report is intended largely for debugging, so
     * we'll go ahead and print the not-necessarily-redundant values.
     */
    uint64_t enqueued, processed;
    unsigned int pending;
    getWorkItemCountsByItem(stats, i, &enqueued, &processed, &pending);
    totalEnqueued  += enqueued;
    totalProcessed += processed;

    static char work[256]; // arbitrary size
    getFunctionName(functionIDs->functions[i], work, sizeof(work));

    if (ENABLE_PER_FUNCTION_TIMING_STATS) {
      uint64_t min, mean, max;
      getWorkItemTimesByItem(stats, i, &min, &mean, &max);
      logInfo("  priority %d: %u pending"
              " %" PRIu64 " enqueued %" PRIu64 " processed"
              " %s"
              " times %" PRIu64 "/%" PRIu64 "/%" PRIu64 "ns",
              functionIDs->priorities[i],
              pending, enqueued, processed, work,
              min, mean, max);
    } else {
      logInfo("  priority %d: %u pending"
              " %" PRIu64 " enqueued %" PRIu64 " processed"
              " %s",
              functionIDs->priorities[i],
              pending, enqueued, processed, work);
    }
  }
  if (i == NUM_WORK_QUEUE_ITEM_STATS) {
    uint64_t enqueued, processed;
    getOtherWorkItemCounts(stats, &enqueued, &processed);
    if (enqueued > 0) {
      totalEnqueued  += enqueued;
      totalProcessed += processed;
      logInfo("  ... others: %" PRIu64 " enqueued %" PRIu64 " processed",
              enqueued, processed);
    }
  }
  logInfo("  total: %llu enqueued %llu processed",
          totalEnqueued, totalProcessed);
}
