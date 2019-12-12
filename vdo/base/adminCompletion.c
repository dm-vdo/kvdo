/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.c#6 $
 */

#include "adminCompletion.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "atomic.h"
#include "completion.h"
#include "types.h"
#include "vdoInternal.h"

/**********************************************************************/
void assertAdminOperationType(struct admin_completion    *completion,
                              AdminOperationType          expected)
{
  ASSERT_LOG_ONLY(completion->type == expected,
                  "admin operation type is %u instead of %u",
                  completion->type, expected);
}

/**********************************************************************/
struct admin_completion *
adminCompletionFromSubTask(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct admin_completion, completion) == 0);
  assertCompletionType(completion->type, SUB_TASK_COMPLETION);
  struct vdo_completion *parent = completion->parent;
  assertCompletionType(parent->type, ADMIN_COMPLETION);
  return (struct admin_completion *) parent;
}

/**********************************************************************/
void assertAdminPhaseThread(struct admin_completion *adminCompletion,
                            const char              *what,
                            const char              *phaseNames[])
{
  ThreadID expected = adminCompletion->getThreadID(adminCompletion);
  ASSERT_LOG_ONLY((getCallbackThreadID() == expected),
                  "%s on correct thread for %s",
                  what, phaseNames[adminCompletion->phase]);
}

/**********************************************************************/
VDO *vdoFromAdminSubTask(struct vdo_completion *completion,
                         AdminOperationType     expected)
{
  struct admin_completion *adminCompletion
    = adminCompletionFromSubTask(completion);
  assertAdminOperationType(adminCompletion, expected);
  return adminCompletion->completion.parent;
}

/**********************************************************************/
int initializeAdminCompletion(VDO                     *vdo,
			      struct admin_completion *adminCompletion)
{
  int result = initializeEnqueueableCompletion(&adminCompletion->completion,
                                               ADMIN_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&adminCompletion->subTaskCompletion,
                                           SUB_TASK_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    uninitializeAdminCompletion(adminCompletion);
    return result;
  }

  atomicStoreBool(&adminCompletion->busy, false);

  return VDO_SUCCESS;
}

/**********************************************************************/
void uninitializeAdminCompletion(struct admin_completion *adminCompletion)
{
  destroyEnqueueable(&adminCompletion->subTaskCompletion);
  destroyEnqueueable(&adminCompletion->completion);
}

/**********************************************************************/
struct vdo_completion *resetAdminSubTask(struct vdo_completion *completion)
{
  struct admin_completion *adminCompletion
    = adminCompletionFromSubTask(completion);
  resetCompletion(completion);
  completion->callbackThreadID = adminCompletion->getThreadID(adminCompletion);
  return completion;
}

/**********************************************************************/
void prepareAdminSubTaskOnThread(VDO       *vdo,
                                 VDOAction *callback,
                                 VDOAction *errorHandler,
                                 ThreadID   threadID)
{
  prepareForRequeue(&vdo->adminCompletion.subTaskCompletion, callback,
                    errorHandler, threadID, &vdo->adminCompletion);
}

/**********************************************************************/
void prepareAdminSubTask(VDO       *vdo,
                         VDOAction *callback,
                         VDOAction *errorHandler)
{
  struct admin_completion *adminCompletion = &vdo->adminCompletion;
  prepareAdminSubTaskOnThread(vdo, callback, errorHandler,
                              adminCompletion->completion.callbackThreadID);
}

/**
 * Callback for admin operations which will notify the layer that the operation
 * is complete.
 *
 * @param completion  The admin completion
 **/
static void adminOperationCallback(struct vdo_completion *completion)
{
  completion->layer->completeAdminOperation(completion->layer);
}

/**********************************************************************/
int performAdminOperation(VDO                    *vdo,
                          AdminOperationType      type,
                          ThreadIDGetterForPhase *threadIDGetter,
                          VDOAction              *action,
                          VDOAction              *errorHandler)
{
  struct admin_completion *adminCompletion = &vdo->adminCompletion;
  if (!compareAndSwapBool(&adminCompletion->busy, false, true)) {
    return logErrorWithStringError(VDO_COMPONENT_BUSY,
                                   "Can't start admin operation of type %u, "
                                   "another operation is already in progress",
                                   type);
  }

  prepareCompletion(&adminCompletion->completion, adminOperationCallback,
                    adminOperationCallback,
                    getAdminThread(getThreadConfig(vdo)), vdo);
  adminCompletion->type        = type;
  adminCompletion->getThreadID = threadIDGetter;
  adminCompletion->phase       = 0;
  prepareAdminSubTask(vdo, action, errorHandler);

  PhysicalLayer *layer = vdo->layer;
  layer->enqueue(adminCompletion->subTaskCompletion.enqueueable);
  layer->waitForAdminOperation(layer);
  int result = adminCompletion->completion.result;
  atomicStoreBool(&adminCompletion->busy, false);
  return result;
}
