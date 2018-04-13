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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/adminCompletion.h#1 $
 */

#ifndef ADMIN_COMPLETION_H
#define ADMIN_COMPLETION_H

#include "completion.h"
#include "types.h"

typedef enum adminOperationType {
  ADMIN_OPERATION_UNKNOWN = 0,
  ADMIN_OPERATION_CLOSE,
  ADMIN_OPERATION_GROW_LOGICAL,
  ADMIN_OPERATION_GROW_PHYSICAL,
  ADMIN_OPERATION_LOAD,
} AdminOperationType;

/**
 * Get the VDO from the sub-task completion of its AdminCompletion.
 *
 * @param completion  the sub-task completion
 * @param expected    the expected operation type of the AdminCompletion
 *
 * @return The VDO
 **/
VDO *vdoFromAdminSubTask(VDOCompletion      *completion,
                         AdminOperationType  expected)
  __attribute__((warn_unused_result));

/**
 * Create an admin completion.
 *
 * @param [in]  layer               The layer on which the admin completion
 *                                  will be enqueued
 * @param [out] adminCompletionPtr  A pointer to hold the new completion
 *
 * @return VDO_SUCCESS or an error
 **/
int makeAdminCompletion(PhysicalLayer    *layer,
                        AdminCompletion **adminCompletionPtr)
  __attribute__((warn_unused_result));

/**
 * Free an admin completion and NULL out the pointer to it.
 *
 * @param adminCompletionPtr  A pointer to the completion to free
 **/
void freeAdminCompletion(AdminCompletion **adminCompletionPtr);

/**
 * Prepare the sub-task completion of a VDO's AdminCompletion
 *
 * @param vdo           The VDO
 * @param callback      The callback for the sub-task
 * @param errorHandler  The error handler for the sub-task
 * @param threadID      The ID of the thread on which to run the callback
 **/
void prepareAdminSubTaskOnThread(VDO       *vdo,
                                 VDOAction *callback,
                                 VDOAction *errorHandler,
                                 ThreadID   threadID);

/**
 * Prepare the sub-task completion of a VDO's AdminCompletion to run on the
 * same thread as the AdminCompletion's main completion.
 *
 * @param vdo           The VDO
 * @param callback      The callback for the sub-task
 * @param errorHandler  The error handler for the sub-task
 **/
void prepareAdminSubTask(VDO       *vdo,
                         VDOAction *callback,
                         VDOAction *errorHandler);

/**
 * Perform an administrative operation (load, close, grow logical, or grow
 * physical). This method should not be called from base threads unless it
 * is certain the calling thread won't be needed to perform the operation. It
 * may (and should) be called from non-base threads.
 *
 * @param vdo     The VDO on which to perform the operation
 * @param type    The type of operation to perform
 * @param action  The action which starts the operation
 *
 * @return The result of the operation
 **/
int performAdminOperation(VDO *vdo, AdminOperationType type, VDOAction *action)
  __attribute__((warn_unused_result));

#endif /* ADMIN_COMPLETION_H */
