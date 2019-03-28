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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.c#2 $
 */

#include "adminState.h"

#include "permassert.h"

#include "completion.h"
#include "types.h"

/**********************************************************************/
static bool isDrainOperation(AdminStateCode operation)
{
  switch (operation) {
  case ADMIN_STATE_FLUSHING:
  case ADMIN_STATE_SAVING:
  case ADMIN_STATE_SUSPENDING:
    return true;

  default:
    return false;
  }
}

/**********************************************************************/
bool startDraining(AdminState     *state,
                   AdminStateCode  operation,
                   VDOCompletion  *waiter)
{
  ASSERT_LOG_ONLY(isDrainOperation(operation),
                  "startDraining() called with drain operation");
  if (state->state != ADMIN_STATE_NORMAL_OPERATION) {
    finishCompletion(waiter, VDO_INVALID_ADMIN_STATE);
    return false;
  }

  if (state->waiter != NULL) {
    finishCompletion(waiter, VDO_COMPONENT_BUSY);
    return false;
  }

  state->state  = operation;
  state->waiter = waiter;
  return true;
}

/**********************************************************************/
bool finishDraining(AdminState *state)
{
  return finishDrainingWithResult(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finishDrainingWithResult(AdminState *state, int result)
{
  switch (state->state) {
  case ADMIN_STATE_FLUSHING:
    state->state = ADMIN_STATE_NORMAL_OPERATION;
    break;

  case ADMIN_STATE_SAVING:
    state->state = ADMIN_STATE_SAVED;
    break;

  case ADMIN_STATE_SUSPENDING:
    state->state = ADMIN_STATE_SUSPENDED;
    break;

  default:
    return false;
  }

  releaseCompletionWithResult(&state->waiter, result);
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
