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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.c#3 $
 */

#include "adminCompletion.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "atomic.h"
#include "completion.h"
#include "types.h"
#include "vdoInternal.h"

/**
 * Check that an AdminCompletion's type is as expected.
 *
 * @param completion  The AdminCompletion to check
 * @param expected    The expected type
 **/
static void assertAdminOperationType(AdminCompletion    *completion,
                                     AdminOperationType  expected)
{
  ASSERT_LOG_ONLY(completion->type == expected,
                  "admin operation type is %u instead of %u",
                  completion->type, expected);
}

/**********************************************************************/
AdminCompletion *adminCompletionFromSubTask(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(AdminCompletion, completion) == 0);
  assertCompletionType(completion->type, SUB_TASK_COMPLETION);
  VDOCompletion *parent = completion->parent;
  assertCompletionType(parent->type, ADMIN_COMPLETION);
  return (AdminCompletion *) parent;
}

/**********************************************************************/
VDO *vdoFromAdminSubTask(VDOCompletion      *completion,
                         AdminOperationType  expected)
{
  AdminCompletion *adminCompletion = adminCompletionFromSubTask(completion);
  assertAdminOperationType(adminCompletion, expected);
  return adminCompletion->completion.parent;
}

/**********************************************************************/
int initializeAdminCompletion(VDO *vdo, AdminCompletion *adminCompletion)
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
void uninitializeAdminCompletion(AdminCompletion *adminCompletion)
{
  destroyEnqueueable(&adminCompletion->subTaskCompletion);
  destroyEnqueueable(&adminCompletion->completion);
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
  AdminCompletion *adminCompletion = &vdo->adminCompletion;
  prepareAdminSubTaskOnThread(vdo, callback, errorHandler,
                              adminCompletion->completion.callbackThreadID);
}

/**
 * Callback for admin operations which will notify the layer that the operation
 * is complete.
 *
 * @param completion  The admin completion
 **/
static void adminOperationCallback(VDOCompletion *completion)
{
  completion->layer->completeAdminOperation(completion->layer);
}

/**********************************************************************/
int performAdminOperation(VDO                *vdo,
                          AdminOperationType  type,
                          VDOAction          *action,
                          VDOAction          *errorHandler)
{
  AdminCompletion *adminCompletion = &vdo->adminCompletion;
  if (!compareAndSwapBool(&adminCompletion->busy, false, true)) {
    return logErrorWithStringError(VDO_COMPONENT_BUSY,
                                   "Can't start admin operation of type %u, "
                                   "another operation is already in progress",
                                   type);
  }

  prepareCompletion(&adminCompletion->completion, adminOperationCallback,
                    adminOperationCallback,
                    getAdminThread(getThreadConfig(vdo)), vdo);
  adminCompletion->type  = type;
  adminCompletion->phase = 0;
  prepareAdminSubTask(vdo, action, errorHandler);

  PhysicalLayer *layer = vdo->layer;
  layer->enqueue(adminCompletion->subTaskCompletion.enqueueable);
  layer->waitForAdminOperation(layer);
  int result = adminCompletion->completion.result;
  atomicStoreBool(&adminCompletion->busy, false);
  return result;
}
