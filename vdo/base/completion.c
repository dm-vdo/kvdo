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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/completion.c#1 $
 */

#include "completion.h"

#include "logger.h"
#include "statusCodes.h"

static const char *VDO_COMPLETION_TYPE_NAMES[] = {
  // Keep UNSET_COMPLETION_TYPE at the top.
  "UNSET_COMPLETION_TYPE",

  // Keep the rest of these in sorted order. If you add or remove an entry,
  // be sure to update the corresponding list in completion.h.
  "ADMIN_COMPLETION",
  "ASYNC_ACTION_CONTEXT",
  "BLOCK_ALLOCATOR_COMPLETION",
  "BLOCK_MAP_COMPLETION",
  "BLOCK_MAP_RECOVERY_COMPLETION",
  "BLOCK_MAP_ZONE_COMPLETION",
  "CHECK_IDENTIFIER_COMPLETION",
  "EXTERNAL_COMPLETION",
  "FLUSH_NOTIFICATION_COMPLETION",
  "GENERATION_FLUSHED_COMPLETION",
  "HEARTBEAT_COMPLETION",
  "LOCK_COUNTER_COMPLETION",
  "PARTITION_COPY_COMPLETION",
  "READ_ONLY_MODE_COMPLETION",
  "READ_ONLY_REBUILD_COMPLETION",
  "RECOVERY_COMPLETION",
  "RECOVERY_JOURNAL_COMPLETION",
  "REFERENCE_COUNTS_COMPLETION",
  "REFERENCE_COUNT_REBUILD_COMPLETION",
  "SLAB_COMPLETION",
  "SLAB_DEPOT_COMPLETION",
  "SLAB_JOURNAL_COMPLETION",
  "SLAB_REBUILD_COMPLETION",
  "SLAB_SCRUBBER_COMPLETION",
  "SLAB_SUMMARY_COMPLETION",
  "SUB_TASK_COMPLETION",
  "TEST_COMPLETION",
  "VDO_COMMAND_COMPLETION",
  "VDO_COMMAND_SUB_COMPLETION",
  "VDO_EXTENT_COMPLETION",
  "VDO_PAGE_COMPLETION",
  "VIO_COMPLETION",
  "WAIT_FOR_READ_ONLY_MODE_COMPLETION",
  "WRAPPING_COMPLETION",
};

/**********************************************************************/
void initializeCompletion(VDOCompletion         *completion,
                          VDOCompletionType      type,
                          PhysicalLayer         *layer)
{
  memset(completion, 0, sizeof(*completion));
  completion->layer = layer;
  completion->type  = type;
  resetCompletion(completion);
}

/**********************************************************************/
int initializeEnqueueableCompletion(VDOCompletion      *completion,
                                    VDOCompletionType   type,
                                    PhysicalLayer      *layer)
{
  initializeCompletion(completion, type, layer);
  return ((layer->createEnqueueable == NULL)
          ? VDO_SUCCESS : layer->createEnqueueable(completion));
}

/**********************************************************************/
void resetCompletion(VDOCompletion *completion)
{
  completion->result   = VDO_SUCCESS;
  completion->complete = false;
}

/**
 * Assert that a completion is not complete.
 *
 * @param completion The completion to check
 **/
static inline void assertIncomplete(VDOCompletion *completion)
{
  ASSERT_LOG_ONLY(!completion->complete, "completion is not complete");
}

/**********************************************************************/
void setCompletionResult(VDOCompletion *completion, int result)
{
  assertIncomplete(completion);
  if (completion->result == VDO_SUCCESS) {
    completion->result = result;
  }
}

/**
 * Check whether a completion's callback must be enqueued, or if it can be run
 * on the current thread. Side effect: clears the requeue flag if it is set,
 * so the caller MUST requeue if this returns true.
 *
 * @param completion  The completion whose callback is to be invoked
 *
 * @return <code>false</code> if the callback must be run on this thread
 *         <code>true</code>  if the callback must be enqueued
 **/
__attribute__((warn_unused_result))
static inline bool requiresEnqueue(VDOCompletion *completion)
{
  if (completion->requeue) {
    completion->requeue = false;
    return true;
  }

  ThreadID callbackThread = completion->callbackThreadID;
  return (callbackThread != completion->layer->getCurrentThreadID());
}

/**********************************************************************/
void invokeCallback(VDOCompletion *completion)
{
  if (requiresEnqueue(completion)) {
    if (completion->enqueueable != NULL) {
      completion->layer->enqueue(completion->enqueueable);
      return;
    }
    ASSERT_LOG_ONLY(false,
                    "non-enqueueable completion (type %s) on correct thread",
                    getCompletionTypeName(completion->type));
  }

  runCallback(completion);
}

/**********************************************************************/
void continueCompletion(VDOCompletion *completion, int result)
{
  setCompletionResult(completion, result);
  invokeCallback(completion);
}

/**********************************************************************/
void completeCompletion(VDOCompletion *completion)
{
  assertIncomplete(completion);
  completion->complete = true;
  if (completion->callback != NULL) {
    invokeCallback(completion);
  }
}

/**********************************************************************/
void finishParentCallback(VDOCompletion *completion)
{
  finishCompletion((VDOCompletion *) completion->parent, completion->result);
}

/**********************************************************************/
const char *getCompletionTypeName(VDOCompletionType completionType)
{
  // Try to catch failures to update the array when the enum values change.
  STATIC_ASSERT(COUNT_OF(VDO_COMPLETION_TYPE_NAMES)
                == (MAX_COMPLETION_TYPE - UNSET_COMPLETION_TYPE));

  if (completionType >= MAX_COMPLETION_TYPE) {
    static char numeric[100];
    snprintf(numeric, 99, "%d (%#x)", completionType, completionType);
    return numeric;
  }

  return VDO_COMPLETION_TYPE_NAMES[completionType];
}

/**********************************************************************/
void destroyEnqueueable(VDOCompletion *completion)
{
  if ((completion == NULL) || (completion->layer == NULL)
      || (completion->layer->destroyEnqueueable == NULL)) {
    return;
  }

  completion->layer->destroyEnqueueable(&completion->enqueueable);
}

/**********************************************************************/
int assertCompletionType(VDOCompletionType actual,
                         VDOCompletionType expected)
{
  return ASSERT((expected == actual),
                "completion type is %s instead of %s",
                getCompletionTypeName(actual),
                getCompletionTypeName(expected));
}
