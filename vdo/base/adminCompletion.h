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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/adminCompletion.h#4 $
 */

#ifndef ADMIN_COMPLETION_H
#define ADMIN_COMPLETION_H

#include "atomic.h"
#include "completion.h"
#include "types.h"

typedef enum adminOperationType {
  ADMIN_OPERATION_UNKNOWN = 0,
  ADMIN_OPERATION_GROW_LOGICAL,
  ADMIN_OPERATION_GROW_PHYSICAL,
  ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
  ADMIN_OPERATION_LOAD,
  ADMIN_OPERATION_RESUME,
  ADMIN_OPERATION_SAVE,
  ADMIN_OPERATION_SUSPEND,
} AdminOperationType;

typedef struct adminCompletion AdminCompletion;

/**
 * A function which gets the ID of the thread on which the current phase of an
 * admin operation should be run.
 *
 * @param adminCompletion The AdminCompletion
 *
 * @return The ID of the thread on which the current phase should be performed
 **/
typedef ThreadID ThreadIDGetterForPhase(AdminCompletion *adminCompletion);

struct adminCompletion {
  /** The completion */
  VDOCompletion           completion;
  /** The sub-task completion */
  VDOCompletion           subTaskCompletion;
  /** Whether this completion is in use */
  AtomicBool              busy;
  /** The operation type */
  AdminOperationType      type;
  /** Method to get the ThreadID for the current phase */
  ThreadIDGetterForPhase *getThreadID;
  /** The current phase of the operation */
  uint32_t                phase;
};

/**
 * Check that an AdminCompletion's type is as expected.
 *
 * @param completion  The AdminCompletion to check
 * @param expected    The expected type
 **/
void assertAdminOperationType(AdminCompletion    *completion,
                              AdminOperationType  expected);

/**
 * Convert the sub-task completion of an AdminCompletion to an AdminCompletion.
 *
 * @param completion  the AdminCompletion's sub-task completion
 *
 * @return The sub-task completion as its enclosing AdminCompletion
 **/
AdminCompletion *adminCompletionFromSubTask(VDOCompletion *completion)
  __attribute__((warn_unused_result));

/**
 * Assert that we are operating on the correct thread for the current phase.
 *
 * @param adminCompletion  The AdminCompletion to check
 * @param what             The method doing the phase check
 * @param phaseNames       The names of the phases of the current operation
 **/
void assertAdminPhaseThread(AdminCompletion *adminCompletion,
                            const char      *what,
                            const char      *phaseNames[]);

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
 * Initialize an admin completion.
 *
 * @param vdo               The VDO which owns the completion
 * @param adminCompletion   The AdminCompletion to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
int initializeAdminCompletion(VDO *vdo, AdminCompletion *adminCompletion)
  __attribute__((warn_unused_result));

/**
 * Clean up an admin completion's resources.
 *
 * @param adminCompletion  The AdminCompletion to uninitialize
 **/
void uninitializeAdminCompletion(AdminCompletion *adminCompletion);

/**
 * Reset an AdminCompletion's sub-task completion.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The sub-task completion for the convenience of callers
 **/
VDOCompletion *resetAdminSubTask(VDOCompletion *completion);

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
 * Perform an administrative operation (load, suspend, grow logical, or grow
 * physical). This method should not be called from base threads unless it is
 * certain the calling thread won't be needed to perform the operation. It may
 * (and should) be called from non-base threads.
 *
 * @param vdo             The VDO on which to perform the operation
 * @param type            The type of operation to perform
 * @param threadIDGetter  A function for getting the ID of the thread on which
 *                        a given phase should be run
 * @param action          The action which starts the operation
 * @param errorHandler    The error handler for the operation
 *
 * @return The result of the operation
 **/
int performAdminOperation(VDO                    *vdo,
                          AdminOperationType      type,
                          ThreadIDGetterForPhase *threadIDGetter,
                          VDOAction              *action,
                          VDOAction              *errorHandler)
  __attribute__((warn_unused_result));

#endif /* ADMIN_COMPLETION_H */
