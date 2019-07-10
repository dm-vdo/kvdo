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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.c#6 $
 */

#include "adminState.h"

#include "logger.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"

/**
 * Get the name of an AdminStateCode for logging purposes.
 *
 * @param code  The AdminStateCode
 *
 * @return The name of the state's code
 **/
static const char *getAdminStateCodeName(AdminStateCode code)
{
  switch (code) {
  case ADMIN_STATE_NORMAL_OPERATION:
    return "ADMIN_STATE_NORMAL_OPERATION";

  case ADMIN_STATE_OPERATING:
    return "ADMIN_STATE_OPERATING";

  case ADMIN_STATE_FLUSHING:
    return "ADMIN_STATE_FLUSHING";

  case ADMIN_STATE_REBUILDING:
    return "ADMIN_STATE_REBUILDING";

  case ADMIN_STATE_SAVING:
    return "ADMIN_STATE_SAVING";

  case ADMIN_STATE_SAVED:
    return "ADMIN_STATE_SAVED";

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
bool startDraining(AdminState     *state,
                   AdminStateCode  operation,
                   VDOCompletion  *waiter)
{
  int result;
  if (state->waiter != NULL) {
    result = VDO_COMPONENT_BUSY;
  } else {
    result = startOperation(state, operation);
  }

  if (result == VDO_SUCCESS) {
    state->waiter = waiter;
    return true;
  }

  if (waiter != NULL) {
    finishCompletion(waiter, result);
  }

  return false;
}

/**********************************************************************/
bool finishDraining(AdminState *state)
{
  return finishDrainingWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishDrainingWithResult(AdminState *state, int result)
{
  if (!isDraining(state) || !finishOperation(state)) {
    return false;
  }

  releaseCompletionWithResult(&state->waiter, result);
  return true;
}

/**********************************************************************/
int startOperation(AdminState *state, AdminStateCode operation)
{
  if (isOperating(state)
      || (isQuiescent(state) && !isQuiescentOperation(operation))) {
    return logErrorWithStringError(VDO_INVALID_ADMIN_STATE,
                                   "Can't start %s from %s",
                                   getAdminStateCodeName(operation),
                                   getAdminStateName(state));
  }

  ASSERT_LOG_ONLY((state->waiter == NULL), "no waiter at start of operation");
  state->state = operation;
  return VDO_SUCCESS;
}

/**********************************************************************/
bool finishOperation(AdminState *state)
{
  if (!isOperating(state)) {
    return false;
  }

  state->state = (isQuiescing(state)
                  ? (state->state & ADMIN_TYPE_MASK) | ADMIN_FLAG_QUIESCENT
                  : ADMIN_STATE_NORMAL_OPERATION);
  return true;
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

/**********************************************************************/
void setOperationWaiter(AdminState *state, VDOCompletion *waiter)
{
  int result;
  if (!isOperating(state)) {
    result = logErrorWithStringError(VDO_INVALID_ADMIN_STATE,
                                     "No operation to wait on");
  } else if (state->waiter != NULL) {
    result = logErrorWithStringError(VDO_COMPONENT_BUSY,
                                     "Operation already has a waiter");
  } else {
    state->waiter = waiter;
    return;
  }

  finishCompletion(waiter, result);
}
