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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/completion.h#12 $
 */

#ifndef COMPLETION_H
#define COMPLETION_H

#include "permassert.h"

#include "physicalLayer.h"
#include "ringNode.h"
#include "types.h"

typedef enum __attribute__((packed)) {
  // Keep UNSET_COMPLETION_TYPE at the top.
  UNSET_COMPLETION_TYPE = 0,

  // Keep the rest of these in sorted order. If you add or remove an entry,
  // be sure to update the corresponding list in completion.c.
  ACTION_COMPLETION,
  ADMIN_COMPLETION,
  ASYNC_ACTION_CONTEXT,
  BLOCK_ALLOCATOR_COMPLETION,
  BLOCK_MAP_RECOVERY_COMPLETION,
  CHECK_IDENTIFIER_COMPLETION,
  EXTERNAL_COMPLETION,
  FLUSH_NOTIFICATION_COMPLETION,
  GENERATION_FLUSHED_COMPLETION,
  HEARTBEAT_COMPLETION,
  LOCK_COUNTER_COMPLETION,
  PARTITION_COPY_COMPLETION,
  READ_ONLY_MODE_COMPLETION,
  READ_ONLY_REBUILD_COMPLETION,
  RECOVERY_COMPLETION,
  REFERENCE_COUNT_REBUILD_COMPLETION,
  SLAB_SCRUBBER_COMPLETION,
  SUB_TASK_COMPLETION,
  TEST_COMPLETION,                      // each unit test may define its own
  VDO_COMMAND_COMPLETION,
  VDO_COMMAND_SUB_COMPLETION,
  VDO_EXTENT_COMPLETION,
  VDO_PAGE_COMPLETION,
  VIO_COMPLETION,
  WRAPPING_COMPLETION,

  // Keep MAX_COMPLETION_TYPE at the bottom.
  MAX_COMPLETION_TYPE
} VDOCompletionType;

/**
 * An asynchronous VDO operation.
 *
 * @param completion    the completion of the operation
 **/
typedef void VDOAction(struct vdo_completion *completion);

struct vdo_completion {
  /** The type of completion this is */
  VDOCompletionType  type;

  /**
   * <code>true</code> once the processing of the operation is complete.
   * This flag should not be used by waiters external to the VDO base as
   * it is used to gate calling the callback.
   **/
  bool               complete;

  /**
   * If true, queue this completion on the next callback invocation, even if
   * it is already running on the correct thread.
   **/
  bool               requeue;

  /** The ID of the thread which should run the next callback */
  ThreadID           callbackThreadID;

  /** The result of the operation */
  int                result;

  /** The physical layer on which this completion operates */
  PhysicalLayer     *layer;

  /** The callback which will be called once the operation is complete */
  VDOAction         *callback;

  /** The callback which, if set, will be called if an error result is set */
  VDOAction         *errorHandler;

  /** The parent object, if any, that spawned this completion */
  void              *parent;

  /** The enqueueable for this completion (may be NULL) */
  Enqueueable       *enqueueable;
};

/**
 * Actually run the callback. This function must be called from the correct
 * callback thread.
 **/
static inline void runCallback(struct vdo_completion *completion)
{
  if ((completion->result != VDO_SUCCESS)
      && (completion->errorHandler != NULL)) {
    completion->errorHandler(completion);
    return;
  }

  completion->callback(completion);
}

/**
 * Set the result of a completion. Older errors will not be masked.
 *
 * @param completion The completion whose result is to be set
 * @param result     The result to set
 **/
void setCompletionResult(struct vdo_completion *completion, int result);

/**
 * Initialize a completion to a clean state, for reused completions.
 *
 * @param completion The completion to initialize
 * @param type       The type of the completion
 * @param layer      The physical layer of the completion
 **/
void initializeCompletion(struct vdo_completion *completion,
                          VDOCompletionType      type,
                          PhysicalLayer         *layer);

/**
 * Initialize a completion to a clean state and make an enqueueable for it.
 *
 * @param completion The completion to initialize
 * @param type       The type of the completion
 * @param layer      The physical layer of the completion
 *
 * @return VDO_SUCCESS or an error
 **/
int initializeEnqueueableCompletion(struct vdo_completion *completion,
                                    VDOCompletionType      type,
                                    PhysicalLayer         *layer)
  __attribute__((warn_unused_result));

/**
 * Reset a completion to a clean state, while keeping
 * the type, layer and parent information.
 *
 * @param completion the completion to reset
 **/
void resetCompletion(struct vdo_completion *completion);

/**
 * Invoke the callback of a completion. If called on the correct thread (i.e.
 * the one specified in the completion's callbackThreadID field), the
 * completion will be run immediately. Otherwise, the completion will be
 * enqueued on the correct callback thread.
 **/
void invokeCallback(struct vdo_completion *completion);

/**
 * Continue processing a completion by setting the current result and calling
 * invokeCallback().
 *
 * @param completion  The completion to continue
 * @param result      The current result (will not mask older errors)
 **/
void continueCompletion(struct vdo_completion *completion, int result);

/**
 * Complete a completion.
 *
 * @param completion  The completion to complete
 **/
void completeCompletion(struct vdo_completion *completion);

/**
 * Finish a completion.
 *
 * @param completion The completion to finish
 * @param result     The result of the completion (will not mask older errors)
 **/
static inline void finishCompletion(struct vdo_completion *completion,
                                    int                    result)
{
  setCompletionResult(completion, result);
  completeCompletion(completion);
}

/**
 * Complete a completion and NULL out the reference to it.
 *
 * @param completionPtr  A pointer to the completion to release
 **/
void releaseCompletion(struct vdo_completion **completionPtr);

/**
 * Finish a completion and NULL out the reference to it.
 *
 * @param completionPtr  A pointer to the completion to release
 * @param result         The result of the completion
 **/
void releaseCompletionWithResult(struct vdo_completion **completionPtr,
                                 int                     result);

/**
 * A callback to finish the parent of a completion.
 *
 * @param completion  The completion which has finished and whose parent should
 *                    be finished
 **/
void finishParentCallback(struct vdo_completion *completion);

/**
 * Error handler which preserves an error in the parent (if there is one),
 * and then resets the failing completion and calls its non-error callback.
 *
 * @param completion  The completion which failed
 **/
void preserveErrorAndContinue(struct vdo_completion *completion);

/**
 * A callback which does nothing. This callback is intended to be set as an
 * error handler in the case where an error should do nothing.
 *
 * @param completion  The completion being called back
 **/
static inline
void noopCallback(struct vdo_completion *completion __attribute__((unused)))
{
}

/**
 * Destroy the enqueueable associated with this completion.
 *
 * @param completion  The completion
 **/
void destroyEnqueueable(struct vdo_completion *completion);

/**
 * Assert that a completion is of the correct type
 *
 * @param actual    The actual completion type
 * @param expected  The expected completion type
 *
 * @return          VDO_SUCCESS or VDO_PARAMETER_MISMATCH
 **/
int assertCompletionType(VDOCompletionType actual,
                         VDOCompletionType expected);

/**
 * Return the name of a completion type.
 *
 * @param completionType        the completion type
 *
 * @return a pointer to a static string; if the completionType is unknown
 *         this is to a static buffer that may be overwritten.
 **/
const char *getCompletionTypeName(VDOCompletionType completionType);

/**
 * Set the callback for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param threadID    The ID of the thread on which the callback should run
 **/
static inline void setCallback(struct vdo_completion *completion,
                               VDOAction             *callback,
                               ThreadID               threadID)
{
  completion->callback         = callback;
  completion->callbackThreadID = threadID;
}

/**
 * Set the callback for a completion and invoke it immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param threadID    The ID of the thread on which the callback should run
 **/
static inline void launchCallback(struct vdo_completion *completion,
                                  VDOAction             *callback,
                                  ThreadID               threadID)
{
  setCallback(completion, callback, threadID);
  invokeCallback(completion);
}

/**
 * Set the callback and parent for a completion.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param threadID    The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void setCallbackWithParent(struct vdo_completion *completion,
                                         VDOAction             *callback,
                                         ThreadID               threadID,
                                         void                  *parent)
{
  setCallback(completion, callback, threadID);
  completion->parent = parent;
}

/**
 * Set the callback and parent for a completion and invoke the callback
 * immediately.
 *
 * @param completion  The completion
 * @param callback    The callback to register
 * @param threadID    The ID of the thread on which the callback should run
 * @param parent      The new parent of the completion
 **/
static inline void launchCallbackWithParent(struct vdo_completion *completion,
                                            VDOAction             *callback,
                                            ThreadID               threadID,
                                            void                  *parent)
{
  setCallbackWithParent(completion, callback, threadID, parent);
  invokeCallback(completion);
}

/**
 * Prepare a completion for launch. Reset it, and then set its callback, error
 * handler, callback thread, and parent.
 *
 * @param completion    The completion
 * @param callback      The callback to register
 * @param errorHandler  The error handler to register
 * @param threadID      The ID of the thread on which the callback should run
 * @param parent        The new parent of the completion
 **/
static inline void prepareCompletion(struct vdo_completion *completion,
                                     VDOAction             *callback,
                                     VDOAction             *errorHandler,
                                     ThreadID               threadID,
                                     void                  *parent)
{
  resetCompletion(completion);
  setCallbackWithParent(completion, callback, threadID, parent);
  completion->errorHandler = errorHandler;
}

/**
 * Prepare a completion for launch ensuring that it will always be requeued.
 * Reset it, and then set its callback, error handler, callback thread, and
 * parent.
 *
 * @param completion    The completion
 * @param callback      The callback to register
 * @param errorHandler  The error handler to register
 * @param threadID      The ID of the thread on which the callback should run
 * @param parent        The new parent of the completion
 **/
static inline void prepareForRequeue(struct vdo_completion *completion,
                                     VDOAction             *callback,
                                     VDOAction             *errorHandler,
                                     ThreadID               threadID,
                                     void                  *parent)
{
  prepareCompletion(completion, callback, errorHandler, threadID, parent);
  completion->requeue = true;
}

/**
 * Prepare a completion for launch which will complete its parent when
 * finished.
 *
 * @param completion  The completion
 * @param parent      The parent to complete
 **/
static inline void prepareToFinishParent(struct vdo_completion *completion,
                                         struct vdo_completion *parent)
{
  prepareCompletion(completion, finishParentCallback, finishParentCallback,
                    parent->callbackThreadID, parent);
}

#endif // COMPLETION_H
