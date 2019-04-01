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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#5 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

typedef enum {
  ADMIN_STATE_NORMAL_OPERATION = 0,
  ADMIN_STATE_FLUSHING,
  ADMIN_STATE_SAVING,
  ADMIN_STATE_SAVED,
  ADMIN_STATE_SUSPENDING,
  ADMIN_STATE_SUSPENDED,
} AdminStateCode;

typedef struct {
  /** The current administrative state */
  AdminStateCode  state;
  /** A completion waiting on a state change */
  VDOCompletion  *waiter;
} AdminState;

/**
 * Get the name of an AdminStateCode for logging purposes.
 *
 * @param state  The AdminState
 **/
const char *getAdminStateName(const AdminState *state)
  __attribute__((warn_unused_result));

/**
 * Check whether an AdminState is flushing.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is flushing
 **/
__attribute__((warn_unused_result))
static inline bool isFlushing(AdminState *state)
{
  return (state->state == ADMIN_STATE_FLUSHING);
}

/**
 * Check whether an AdminState is saving.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is saving
 **/
__attribute__((warn_unused_result))
static inline bool isSaving(AdminState *state)
{
  return (state->state == ADMIN_STATE_SAVING);
}

/**
 * Check whether an AdminState is saved.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is saved
 **/
__attribute__((warn_unused_result))
static inline bool isSaved(AdminState *state)
{
  return (state->state == ADMIN_STATE_SAVED);
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
 * Check whether an AdminState is suspended.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is suspended
 **/
__attribute__((warn_unused_result))
static inline bool isSuspended(AdminState *state)
{
  return (state->state == ADMIN_STATE_SUSPENDED);
}

/**
 * Chech whether an AdminState is draining.
 *
 * @param state  The AdminState to query
 *
 * @return <code>true</code> if the state is draining
 **/
__attribute__((warn_unused_result))
static inline bool isDraining(AdminState *state)
{
  return (isFlushing(state) || isSaving(state) || isSuspending(state));
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
  return (isSaved(state) || isSuspended(state));
}

/**
 * Initiate a drain operation if the current state permits it.
 *
 * @param state      The AdminState
 * @param operation  The type of drain to initiate
 * @param waiter     The completion to notify when the drain is complete
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
