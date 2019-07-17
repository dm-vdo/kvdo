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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.c#12 $
 */

#include "adminState.h"

#include "logger.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"

/**********************************************************************/
const char *getAdminStateCodeName(AdminStateCode code)
{
  switch (code) {
  case ADMIN_STATE_NORMAL_OPERATION:
    return "ADMIN_STATE_NORMAL_OPERATION";

  case ADMIN_STATE_OPERATING:
    return "ADMIN_STATE_OPERATING";

  case ADMIN_STATE_FORMATTING:
    return "ADMIN_STATE_FORMATTING";

  case ADMIN_STATE_LOADING:
    return "ADMIN_STATE_LOADING";

  case ADMIN_STATE_LOADING_FOR_RECOVERY:
    return "ADMIN_STATE_LOADING_FOR_RECOVERY";

  case ADMIN_STATE_LOADING_FOR_REBUILD:
    return "ADMIN_STATE_LOADING_FOR_REBUILD";

  case ADMIN_STATE_WAITING_FOR_RECOVERY:
    return "ADMIN_STATE_WAITING_FOR_RECOVERY";

  case ADMIN_STATE_RECOVERING:
    return "ADMIN_STATE_RECOVERING";

  case ADMIN_STATE_REBUILDING:
    return "ADMIN_STATE_REBUILDING";

  case ADMIN_STATE_SAVING:
    return "ADMIN_STATE_SAVING";

  case ADMIN_STATE_SAVED:
    return "ADMIN_STATE_SAVED";

  case ADMIN_STATE_SCRUBBING:
    return "ADMIN_STATE_SCRUBBING";

  case ADMIN_STATE_SAVE_FOR_SCRUBBING:
    return "ADMIN_STATE_SAVE_FOR_SCRUBBING";

  case ADMIN_STATE_SUSPENDING:
    return "ADMIN_STATE_SUSPENDING";

  case ADMIN_STATE_SUSPENDED:
    return "ADMIN_STATE_SUSPENDED";

  case ADMIN_STATE_SUSPENDED_OPERATION:
    return "ADMIN_STATE_SUSPENDED_OPERATION";

  case ADMIN_STATE_RESUMING:
    return "ADMIN_STATE_RESUMING";

  default:
    return "INVALID ADMIN_STATE";
  }
}

/**********************************************************************/
const char *getAdminStateName(const AdminState *state)
{
  return getAdminStateCodeName(state->state);
}

/**********************************************************************/
static AdminStateCode getNextState(AdminStateCode previousState,
                                   AdminStateCode operation)
{
  if (isQuiescingCode(operation)) {
    return ((operation & ADMIN_TYPE_MASK) | ADMIN_FLAG_QUIESCENT);
  }

  if (operation == ADMIN_STATE_SUSPENDED_OPERATION) {
    return previousState;
  }

  return ADMIN_STATE_NORMAL_OPERATION;
}

/**
 * Begin an operation if it may be started given the current state.
 *
 * @param state      The AdminState
 * @param operation  The operation to begin
 * @param waiter     A completion to notify when the operation is complete; may
 *                   be NULL
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int beginOperation(AdminState     *state,
                          AdminStateCode  operation,
                          VDOCompletion  *waiter)
{
  int result;
  if (isOperating(state)
      || (isQuiescent(state) && !isQuiescentOperation(operation))) {
    result = logErrorWithStringError(VDO_INVALID_ADMIN_STATE,
                                     "Can't start %s from %s",
                                     getAdminStateCodeName(operation),
                                     getAdminStateName(state));
  } else if (state->waiter != NULL) {
    result = logErrorWithStringError(VDO_COMPONENT_BUSY,
                                     "Can't start %s with extant waiter",
                                     getAdminStateCodeName(operation));
  } else {
    state->waiter    = waiter;
    state->nextState = getNextState(state->state, operation);
    state->state     = operation;
    return VDO_SUCCESS;
  }

  if (waiter != NULL) {
    finishCompletion(waiter, result);
  }

  return result;
}

/**
 * Finish an operation if one is in progress. If there is a waiter, it will be
 * notified.
 *
 * @param state   The AdminState
 * @param result  The result of the operation
 *
 * @return <code>true</code> if an operation was in progress and has been
 *         finished.
 **/
static bool endOperation(AdminState *state, int result)
{
  if (!isOperating(state)) {
    return false;
  }

  state->state = state->nextState;
  releaseCompletionWithResult(&state->waiter, result);
  return true;
}

/**
 * Check the result of a state validation. If the result failed, log an invalid
 * state error and, if there is a waiter, notify it.
 *
 * @param valid   <code>true</code> if the code is of an appropriate type
 * @param code    The code which failed to be of the correct type
 * @param what    What the code failed to be, for logging
 * @param waiter  The completion to notify of the error; may be NULL
 *
 * @return The result of the check
 **/
static bool checkCode(bool            valid,
                      AdminStateCode  code,
                      const char     *what,
                      VDOCompletion  *waiter)
{
  if (valid) {
    return true;
  }

  int result = logErrorWithStringError(VDO_INVALID_ADMIN_STATE,
                                       "%s is not a %s",
                                       getAdminStateCodeName(code), what);
  if (waiter != NULL) {
    finishCompletion(waiter, result);
  }

  return false;
}

/**********************************************************************/
bool assertDrainOperation(AdminStateCode operation, VDOCompletion *waiter)
{
  return checkCode(isDrainOperation(operation), operation, "drain operation",
                   waiter);
}

/**********************************************************************/
bool startDraining(AdminState     *state,
                   AdminStateCode  operation,
                   VDOCompletion  *waiter)
{
  return (assertDrainOperation(operation, waiter)
          && (beginOperation(state, operation, waiter) == VDO_SUCCESS));
}

/**********************************************************************/
bool finishDraining(AdminState *state)
{
  return finishDrainingWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishDrainingWithResult(AdminState *state, int result)
{
  return (isDraining(state) && endOperation(state, result));
}

/**********************************************************************/
bool assertLoadOperation(AdminStateCode operation, VDOCompletion *waiter)
{
  return checkCode(isLoadOperation(operation), operation, "load operation",
                   waiter);
}

/**********************************************************************/
bool startLoading(AdminState     *state,
                  AdminStateCode  operation,
                  VDOCompletion  *waiter)
{
  return (assertLoadOperation(operation, waiter)
          && (beginOperation(state, operation, waiter) == VDO_SUCCESS));
}

/**********************************************************************/
bool finishLoading(AdminState *state)
{
  return finishLoadingWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishLoadingWithResult(AdminState *state, int result)
{
  return (isLoading(state) && endOperation(state, result));
}

/**
 * Check whether an AdminStateCode is a resume operation.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to notify if the operation is not a resume
 *                   operation; may be NULL
 *
 * @return <code>true</code> if the code is a resume operation
 **/
static bool assertResumeOperation(AdminStateCode operation,
                                  VDOCompletion *waiter)
{
  return checkCode(isResumeOperation(operation), operation, "resume operation",
                   waiter);
}

/**********************************************************************/
bool startResuming(AdminState     *state,
                   AdminStateCode  operation,
                   VDOCompletion  *waiter)
{
  return (assertResumeOperation(operation, waiter)
          && (beginOperation(state, operation, waiter) == VDO_SUCCESS));
}

/**********************************************************************/
bool finishResuming(AdminState *state)
{
  return finishResumingWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishResumingWithResult(AdminState *state, int result)
{
  return (isResuming(state) && endOperation(state, result));
}

/**********************************************************************/
bool resumeIfQuiescent(AdminState *state)
{
  if (!isQuiescent(state)) {
    return false;
  }

  state->state = ADMIN_STATE_NORMAL_OPERATION;
  return true;
}

/**
 * Check whether an AdminStateCode is an operation.
 *
 * @param code    The operation to check
 * @param waiter  The completion to notify if the code is not an operation; may
 *                be NULL
 *
 * @return <code>true</code> if the code is an operation
 **/
static bool assertOperation(AdminStateCode code, VDOCompletion *waiter)
{
  return checkCode(isOperation(code), code, "operation", waiter);
}

/**********************************************************************/
int startOperation(AdminState *state, AdminStateCode operation)
{
  return (assertOperation(operation, NULL)
          ? beginOperation(state, operation, NULL) : VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
bool startOperationWithWaiter(AdminState     *state,
                              AdminStateCode  operation,
                              VDOCompletion  *waiter)
{
  return (assertOperation(operation, waiter)
          && (beginOperation(state, operation, waiter) == VDO_SUCCESS));
}

/**********************************************************************/
bool finishOperation(AdminState *state)
{
  return finishOperationWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishOperationWithResult(AdminState *state, int result)
{
  return endOperation(state, result);
}
