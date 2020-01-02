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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.h#7 $
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

struct admin_completion;

/**
 * A function which gets the ID of the thread on which the current phase of an
 * admin operation should be run.
 *
 * @param adminCompletion The struct admin_completion
 *
 * @return The ID of the thread on which the current phase should be performed
 **/
typedef ThreadID
ThreadIDGetterForPhase(struct admin_completion *adminCompletion);

struct admin_completion {
  /** The completion */
  struct vdo_completion   completion;
  /** The sub-task completion */
  struct vdo_completion   subTaskCompletion;
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
 * Check that an admin_completion's type is as expected.
 *
 * @param completion  The admin_completion to check
 * @param expected    The expected type
 **/
void assertAdminOperationType(struct admin_completion    *completion,
                              AdminOperationType          expected);

/**
 * Convert the sub-task completion of an admin_completion to an
 * admin_completion.
 *
 * @param completion  the admin_completion's sub-task completion
 *
 * @return The sub-task completion as its enclosing admin_completion
 **/
struct admin_completion *
adminCompletionFromSubTask(struct vdo_completion *completion)
  __attribute__((warn_unused_result));

/**
 * Assert that we are operating on the correct thread for the current phase.
 *
 * @param adminCompletion  The admin_completion to check
 * @param what             The method doing the phase check
 * @param phaseNames       The names of the phases of the current operation
 **/
void assertAdminPhaseThread(struct admin_completion *adminCompletion,
                            const char              *what,
                            const char              *phaseNames[]);

/**
 * Get the vdo from the sub-task completion of its admin_completion.
 *
 * @param completion  the sub-task completion
 * @param expected    the expected operation type of the admin_completion
 *
 * @return The vdo
 **/
struct vdo *vdoFromAdminSubTask(struct vdo_completion *completion,
                                AdminOperationType     expected)
  __attribute__((warn_unused_result));

/**
 * Initialize an admin completion.
 *
 * @param vdo               The vdo which owns the completion
 * @param adminCompletion   The admin_completion to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
int initializeAdminCompletion(struct vdo              *vdo,
                              struct admin_completion *adminCompletion)
  __attribute__((warn_unused_result));

/**
 * Clean up an admin completion's resources.
 *
 * @param adminCompletion  The admin_completion to uninitialize
 **/
void uninitializeAdminCompletion(struct admin_completion *adminCompletion);

/**
 * Reset an admin_completion's sub-task completion.
 *
 * @param completion  The admin_completion's sub-task completion
 *
 * @return The sub-task completion for the convenience of callers
 **/
struct vdo_completion *resetAdminSubTask(struct vdo_completion *completion);

/**
 * Prepare the sub-task completion of a vdo's admin_completion
 *
 * @param vdo           The vdo
 * @param callback      The callback for the sub-task
 * @param errorHandler  The error handler for the sub-task
 * @param threadID      The ID of the thread on which to run the callback
 **/
void prepareAdminSubTaskOnThread(struct vdo *vdo,
                                 VDOAction  *callback,
                                 VDOAction  *errorHandler,
                                 ThreadID    threadID);

/**
 * Prepare the sub-task completion of a vdo's admin_completion to run on the
 * same thread as the admin_completion's main completion.
 *
 * @param vdo           The vdo
 * @param callback      The callback for the sub-task
 * @param errorHandler  The error handler for the sub-task
 **/
void prepareAdminSubTask(struct vdo *vdo,
                         VDOAction  *callback,
                         VDOAction  *errorHandler);

/**
 * Perform an administrative operation (load, suspend, grow logical, or grow
 * physical). This method should not be called from base threads unless it is
 * certain the calling thread won't be needed to perform the operation. It may
 * (and should) be called from non-base threads.
 *
 * @param vdo             The vdo on which to perform the operation
 * @param type            The type of operation to perform
 * @param threadIDGetter  A function for getting the ID of the thread on which
 *                        a given phase should be run
 * @param action          The action which starts the operation
 * @param errorHandler    The error handler for the operation
 *
 * @return The result of the operation
 **/
int performAdminOperation(struct vdo             *vdo,
                          AdminOperationType      type,
                          ThreadIDGetterForPhase *threadIDGetter,
                          VDOAction              *action,
                          VDOAction              *errorHandler)
  __attribute__((warn_unused_result));

#endif /* ADMIN_COMPLETION_H */
