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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#7 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

typedef enum {
  ADMIN_TYPE_NORMAL = 0,
  ADMIN_TYPE_FLUSH,
  ADMIN_TYPE_SAVE,
  ADMIN_TYPE_SUSPEND,
  ADMIN_TYPE_MASK      = 0xff,

  ADMIN_FLAG_DRAINING  = 0x100,
  ADMIN_FLAG_QUIESCING = 0x200,
  ADMIN_FLAG_QUIESCENT = 0x400,

  ADMIN_STATE_NORMAL_OPERATION = ADMIN_TYPE_NORMAL,
  ADMIN_STATE_FLUSHING         = ADMIN_FLAG_DRAINING | ADMIN_TYPE_FLUSH,
  ADMIN_STATE_SAVING           = (ADMIN_FLAG_DRAINING | ADMIN_FLAG_QUIESCING
                                  | ADMIN_TYPE_SAVE),
  ADMIN_STATE_SAVED            = ADMIN_FLAG_QUIESCENT | ADMIN_TYPE_SAVE,
  ADMIN_STATE_SUSPENDING       = (ADMIN_FLAG_DRAINING | ADMIN_FLAG_QUIESCING
                                  | ADMIN_TYPE_SUSPEND),
  ADMIN_STATE_SUSPENDED        = ADMIN_FLAG_QUIESCENT | ADMIN_TYPE_SUSPEND,
} AdminStateCode;

typedef struct {
  /** The current administrative state */
  AdminStateCode  state;
  /** A completion waiting on a state change */
  VDOCompletion  *waiter;
} AdminState;

/**
 * Get the name of an AdminState's code for logging purposes.
 *
 * @param state  The AdminState
 *
 * @return The name of the state's code
 **/
const char *getAdminStateName(const AdminState *state)
  __attribute__((warn_unused_result));

/**
 * Check whether an AdminState is in normal operation.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is in normal operation
 **/
__attribute__((warn_unused_result))
static inline bool isOperatingNormally(AdminState *state)
{
  return (state->state == ADMIN_STATE_NORMAL_OPERATION);
}

/**
 * Check whether an AdminState is suspending.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is suspending
 **/
__attribute__((warn_unused_result))
static inline bool isSuspending(AdminState *state)
{
  return (state->state == ADMIN_STATE_SUSPENDING);
}

/**
 * Check whether an AdminState is draining.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is draining
 **/
__attribute__((warn_unused_result))
static inline bool isDraining(AdminState *state)
{
  return ((state->state & ADMIN_FLAG_DRAINING) == ADMIN_FLAG_DRAINING);
}

/**
 * Check whether an AdminState is quiescing.
 * @return <code>true</code> if the state is quiescing
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescing(AdminState *state)
{
  return ((state->state & ADMIN_FLAG_QUIESCING) == ADMIN_FLAG_QUIESCING);
}

/**
 * Check whether an AdminState is quiescent.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> is the state is quiescent
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescent(AdminState *state)
{
  return ((state->state & ADMIN_FLAG_QUIESCENT) == ADMIN_FLAG_QUIESCENT);
}

/**
 * Initiate a drain operation if the current state permits it.
 *
 * @param state      The AdminState
 * @param operation  The type of drain to initiate
 * @param waiter     The completion to notify when the drain is complete (may be
 *                   NULL)
 *
 * @return <code>true</code> if the drain was initiated, if not the waiter
 *         will be notified
 **/
bool startDraining(AdminState     *state,
                   AdminStateCode  operation,
                   VDOCompletion  *waiter);

/**
 * Finish a drain operation if one was in progress.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state was draining; will notify the waiter
 *         if so
 **/
bool finishDraining(AdminState *state);

/**
 * Finish a drain operation with a status code.
 *
 * @param state   The AdminState to query
 * @param result  The result of the drain operation
 *
 * @return <code>true</code> if the state was draining; will notify the
 *         waiter if so
 **/
bool finishDrainingWithResult(AdminState *state, int result);

/**
 * Change the state to normal operation if the current state is quiescent.
 *
 * @param state  The AdminState to resume
 *
 * @return <code>true</code> if the state was resumed
 **/
bool resumeIfQuiescent(AdminState *state);

#endif // ADMIN_STATE_H
