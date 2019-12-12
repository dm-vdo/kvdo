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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#19 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

/**
 * The list of state types.
 **/
typedef enum {
  /** Normal operation, DataVIOs may be active */
  ADMIN_TYPE_NORMAL = 0,
  /**
   * Format: an operation for formatting a new VDO.
   **/
  ADMIN_TYPE_FORMAT,
  /**
   * Recover: a recovery operation.
   **/
  ADMIN_TYPE_RECOVER,
  /**
   * Rebuild: write data necessary for a full rebuild, drain outstanding I/O,
   *          and return to normal operation.
   **/
  ADMIN_TYPE_REBUILD,
  /**
   * Save: write all dirty metadata thereby restoring the VDO to a clean state,
   *       drain outstanding I/O, and become quiescent.
   **/
  ADMIN_TYPE_SAVE,
  /**
   * Scrub: load and/or save state necessary to scrub a slab.
   **/
  ADMIN_TYPE_SCRUB,
  /**
   * Suspend: write enough dirty metadata to perform resize transactions,
   *          drain outstanding I/O, and become quiescent.
   **/
  ADMIN_TYPE_SUSPEND,
  /**
   * Resume: return to normal from a quiescent state
   **/
  ADMIN_TYPE_RESUME,
  /** The mask for extracting the AdminType from and AdminStateCode */
  ADMIN_TYPE_MASK = 0xff,
} AdminType;


/**
 * The bit position of flags used to categorize states.
 **/
typedef enum {
  ADMIN_FLAG_BIT_START    = 8,
  /** Flag indicating that I/O is draining */
  ADMIN_FLAG_BIT_DRAINING = ADMIN_FLAG_BIT_START,
  /** Flag indicating a load operation */
  ADMIN_FLAG_BIT_LOADING,
  /** Flag indicating that the next state will be a quiescent state */
  ADMIN_FLAG_BIT_QUIESCING,
  /** Flag indicating that the state is quiescent */
  ADMIN_FLAG_BIT_QUIESCENT,
  /**
   * Flag indicating that an operation is in progress and so no other
   * operation may be started.
   **/
  ADMIN_FLAG_BIT_OPERATING,
} AdminFlagBit;

/**
 * The flags themselves.
 **/
typedef enum {
  ADMIN_FLAG_DRAINING  = (uint32_t) (1 << ADMIN_FLAG_BIT_DRAINING),
  ADMIN_FLAG_LOADING   = (uint32_t) (1 << ADMIN_FLAG_BIT_LOADING),
  ADMIN_FLAG_QUIESCING = (uint32_t) (1 << ADMIN_FLAG_BIT_QUIESCING),
  ADMIN_FLAG_QUIESCENT = (uint32_t) (1 << ADMIN_FLAG_BIT_QUIESCENT),
  ADMIN_FLAG_OPERATING = (uint32_t) (1 << ADMIN_FLAG_BIT_OPERATING),
} AdminFlag;

/**
 * The state codes.
 **/
typedef enum {
  ADMIN_STATE_NORMAL_OPERATION     = ADMIN_TYPE_NORMAL,
  ADMIN_STATE_OPERATING            = (ADMIN_TYPE_NORMAL
                                      | ADMIN_FLAG_OPERATING),
  ADMIN_STATE_FORMATTING           = (ADMIN_TYPE_FORMAT
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_LOADING),
  ADMIN_STATE_LOADING              = (ADMIN_TYPE_NORMAL
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_LOADING),
  ADMIN_STATE_LOADING_FOR_RECOVERY = (ADMIN_TYPE_RECOVER
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_LOADING),
  ADMIN_STATE_LOADING_FOR_REBUILD  = (ADMIN_TYPE_REBUILD
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_LOADING),
  ADMIN_STATE_WAITING_FOR_RECOVERY = (ADMIN_TYPE_RECOVER
                                      | ADMIN_FLAG_OPERATING),
  ADMIN_STATE_NEW                  = (ADMIN_TYPE_NORMAL
                                      | ADMIN_FLAG_QUIESCENT),
  ADMIN_STATE_RECOVERING           = (ADMIN_TYPE_RECOVER
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING),
  ADMIN_STATE_REBUILDING           = (ADMIN_TYPE_REBUILD
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING),
  ADMIN_STATE_SAVING               = (ADMIN_TYPE_SAVE
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING
                                      | ADMIN_FLAG_QUIESCING),
  ADMIN_STATE_SAVED                = (ADMIN_TYPE_SAVE
                                      | ADMIN_FLAG_QUIESCENT),
  ADMIN_STATE_SCRUBBING            = (ADMIN_TYPE_SCRUB
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING
                                      | ADMIN_FLAG_LOADING),
  ADMIN_STATE_SAVE_FOR_SCRUBBING   = (ADMIN_TYPE_SCRUB
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING),
  ADMIN_STATE_SUSPENDING           = (ADMIN_TYPE_SUSPEND
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_DRAINING
                                      | ADMIN_FLAG_QUIESCING),
  ADMIN_STATE_SUSPENDED            = (ADMIN_TYPE_SUSPEND
                                      | ADMIN_FLAG_QUIESCENT),
  ADMIN_STATE_SUSPENDED_OPERATION  = (ADMIN_TYPE_SUSPEND
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_QUIESCENT),
  ADMIN_STATE_RESUMING             = (ADMIN_TYPE_RESUME
                                      | ADMIN_FLAG_OPERATING
                                      | ADMIN_FLAG_QUIESCENT),
} AdminStateCode;

struct admin_state {
  /** The current administrative state */
  AdminStateCode         state;
  /** The next administrative state (when the current operation finishes */
  AdminStateCode         nextState;
  /** A completion waiting on a state change */
  struct vdo_completion *waiter;
  /** Whether an operation is being initiated */
  bool                   starting;
  /** Whether an operation has completed in the initiator */
  bool                   complete;
};

/**
 * A method to be called once an admin operation may be initiated.
 **/
typedef void AdminInitiator(struct admin_state *state);

/**
 * Get the name of an AdminStateCode for logging purposes.
 *
 * @param code  The AdminStateCode
 *
 * @return The name of the state's code
 **/
const char *getAdminStateCodeName(AdminStateCode code)
  __attribute__((warn_unused_result));

/**
 * Get the name of an admin_state's code for logging purposes.
 *
 * @param state  The admin_state
 *
 * @return The name of the state's code
 **/
const char *getAdminStateName(const struct admin_state *state)
  __attribute__((warn_unused_result));

/**
 * Check whether an admin_state is in normal operation.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is normal
 **/
__attribute__((warn_unused_result))
static inline bool isNormal(struct admin_state *state)
{
  return ((state->state & ADMIN_TYPE_MASK) == ADMIN_TYPE_NORMAL);
}

/**
 * Check whether an AdminStateCode is an operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is an operation
 **/
__attribute__((warn_unused_result))
static inline bool isOperation(AdminStateCode code)
{
  return ((code & ADMIN_FLAG_OPERATING) == ADMIN_FLAG_OPERATING);
}

/**
 * Check whether an admin_state is operating.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is operating
 **/
__attribute__((warn_unused_result))
static inline bool isOperating(struct admin_state *state)
{
  return isOperation(state->state);
}

/**
 * Check whether an admin_state is suspending.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is suspending
 **/
__attribute__((warn_unused_result))
static inline bool isSuspending(struct admin_state *state)
{
  return (state->state == ADMIN_STATE_SUSPENDING);
}

/**
 * Check whether an admin_state is suspended.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is suspended
 **/
__attribute__((warn_unused_result))
static inline bool isSuspended(struct admin_state *state)
{
  return (state->state == ADMIN_STATE_SUSPENDED);
}

/**
 * Check whether an admin_state is saving.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saving
 **/
__attribute__((warn_unused_result))
static inline bool isSaving(struct admin_state *state)
{
  return (state->state == ADMIN_STATE_SAVING);
}

/**
 * Check whether an admin_state is saved.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saved
 **/
__attribute__((warn_unused_result))
static inline bool isSaved(struct admin_state *state)
{
  return (state->state == ADMIN_STATE_SAVED);
}

/**
 * Check whether an AdminStateCode is a drain operation.
 *
 * @param code  The AdminStateCode to check
 *
 * @return <code>true</code> if the code is for a drain operation
 **/
__attribute__((warn_unused_result))
static inline bool isDrainOperation(AdminStateCode code)
{
  return ((code & ADMIN_FLAG_DRAINING) == ADMIN_FLAG_DRAINING);
}

/**
 * Check whether an admin_state is draining.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is draining
 **/
__attribute__((warn_unused_result))
static inline bool isDraining(struct admin_state *state)
{
  return isDrainOperation(state->state);
}

/**
 * Check whether an AdminStateCode is a load operation.
 *
 * @param code  The AdminStateCode to check
 *
 * @return <code>true</code> if the code is for a load operation
 **/
__attribute__((warn_unused_result))
static inline bool isLoadOperation(AdminStateCode code)
{
  return ((code & ADMIN_FLAG_LOADING) == ADMIN_FLAG_LOADING);
}

/**
 * Check whether an admin_state is loading.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is loading
 **/
__attribute__((warn_unused_result))
static inline bool isLoading(struct admin_state *state)
{
  return isLoadOperation(state->state);
}

/**
 * Check whether an AdminStateCode is a resume operation.
 *
 * @param code  The AdminStateCode to check
 *
 * @return <code>true</code> if the code is for a resume operation
 **/
__attribute__((warn_unused_result))
static inline bool isResumeOperation(AdminStateCode code)
{
  return ((code & ADMIN_TYPE_MASK) == ADMIN_TYPE_RESUME);
}

/**
 * Check whether an admin_state is resumeing.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is resumeing
 **/
__attribute__((warn_unused_result))
static inline bool isResuming(struct admin_state *state)
{
  return isResumeOperation(state->state);
}

/**
 * Check whether an admin_state is doing a clean load.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is a clean load
 **/
__attribute__((warn_unused_result))
static inline bool isCleanLoad(struct admin_state *state)
{
  return ((state->state == ADMIN_STATE_FORMATTING)
          || (state->state == ADMIN_STATE_LOADING));
}

/**
 * Check whether an AdminStateCode is quiescing.
 *
 * param code  The AdminStateCode to check
 *
 * @return <code>true</code> is the state is quiescing
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescingCode(AdminStateCode code)
{
  return ((code & ADMIN_FLAG_QUIESCING) == ADMIN_FLAG_QUIESCING);
}

/**
 * Check whether an admin_state is quiescing.
 *
 * @param state  The admin_state to check
 *
 * @return <code>true</code> if the state is quiescing
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescing(struct admin_state *state)
{
  return isQuiescingCode(state->state);
}

/**
 * Check where an AdminStateCode is quiescent.
 *
 * param code  The AdminStateCode to check
 *
 * @return <code>true</code> is the state is quiescent
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescentCode(AdminStateCode code)
{
  return ((code & ADMIN_FLAG_QUIESCENT) == ADMIN_FLAG_QUIESCENT);
}

/**
 * Check whether an admin_state is quiescent.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> is the state is quiescent
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescent(struct admin_state *state)
{
  return isQuiescentCode(state->state);
}

/**
 * Check whether an AdminStateCode is a quiescent operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is a quiescent operation
 **/
__attribute__((warn_unused_result))
static inline bool isQuiescentOperation(AdminStateCode code)
{
  return (isQuiescentCode(code) && isOperation(code));
}

/**
 * Check that an operation is a drain.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to finish with an error if the operation is
 *                   not a drain
 *
 * @return <code>true</code> if the specified operation is a drain
 **/
bool assertDrainOperation(AdminStateCode         operation,
                          struct vdo_completion *waiter)
  __attribute__((warn_unused_result));

/**
 * Initiate a drain operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of drain to initiate
 * @param waiter     The completion to notify when the drain is complete (may be
 *                   NULL)
 * @param initiator  The AdminInitiator to call if the operation may begin; may
 *                   be NULL
 *
 * @return <code>true</code> if the drain was initiated, if not the waiter
 *         will be notified
 **/
bool startDraining(struct admin_state    *state,
                   AdminStateCode         operation,
                   struct vdo_completion *waiter,
                   AdminInitiator        *initiator);

/**
 * Finish a drain operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was draining; will notify the waiter
 *         if so
 **/
bool finishDraining(struct admin_state *state);

/**
 * Finish a drain operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the drain operation
 *
 * @return <code>true</code> if the state was draining; will notify the
 *         waiter if so
 **/
bool finishDrainingWithResult(struct admin_state *state, int result);

/**
 * Check that an operation is a load.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to finish with an error if the operation is
 *                   not a load
 *
 * @return <code>true</code> if the specified operation is a load
 **/
bool assertLoadOperation(AdminStateCode         operation,
                         struct vdo_completion *waiter)
  __attribute__((warn_unused_result));

/**
 * Initiate a load operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of load to initiate
 * @param waiter     The completion to notify when the load is complete (may be
 *                   NULL)
 * @param initiator  The AdminInitiator to call if the operation may begin; may
 *                   be NULL
 *
 * @return <code>true</code> if the load was initiated, if not the waiter
 *         will be notified
 **/
bool startLoading(struct admin_state    *state,
                  AdminStateCode         operation,
                  struct vdo_completion *waiter,
                  AdminInitiator        *initiator);

/**
 * Finish a load operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was loading; will notify the waiter
 *         if so
 **/
bool finishLoading(struct admin_state *state);

/**
 * Finish a load operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the load operation
 *
 * @return <code>true</code> if the state was loading; will notify the
 *         waiter if so
 **/
bool finishLoadingWithResult(struct admin_state *state, int result);

/**
 * Check whether an AdminStateCode is a resume operation.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to notify if the operation is not a resume
 *                   operation; may be NULL
 *
 * @return <code>true</code> if the code is a resume operation
 **/
bool assertResumeOperation(AdminStateCode         operation,
                           struct vdo_completion *waiter);

/**
 * Initiate a resume operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of resume to start
 * @param waiter     The completion to notify when the resume is complete (may
 *                   be NULL)
 * @param initiator  The AdminInitiator to call if the operation may begin; may
 *                   be NULL
 *
 * @return <code>true</code> if the resume was initiated, if not the waiter
 *         will be notified
 **/
bool startResuming(struct admin_state    *state,
                   AdminStateCode         operation,
                   struct vdo_completion *waiter,
                   AdminInitiator        *initiator);

/**
 * Finish a resume operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was resuming; will notify the waiter
 *         if so
 **/
bool finishResuming(struct admin_state *state);

/**
 * Finish a resume operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the resume operation
 *
 * @return <code>true</code> if the state was resuming; will notify the
 *         waiter if so
 **/
bool finishResumingWithResult(struct admin_state *state, int result);

/**
 * Change the state to normal operation if the current state is quiescent.
 *
 * @param state  The admin_state to resume
 *
 * @return VDO_SUCCESS if the state resumed, VDO_INVALID_ADMIN_STATE otherwise
 **/
int resumeIfQuiescent(struct admin_state *state);

/**
 * Attempt to start an operation.
 *
 * @param state      the admin_state
 * @param operation  the operation to start
 *
 * @return VDO_SUCCESS             if the operation was started
 *         VDO_INVALID_ADMIN_STATE if not
 **/
int startOperation(struct admin_state *state, AdminStateCode operation);

/**
 * Attempt to start an operation.
 *
 * @param state      the admin_state
 * @param operation  the operation to start
 * @param waiter     the completion to notify when the operation completes or
 *                   fails to start; may be NULL
 * @param initiator  The AdminInitiator to call if the operation may begin; may
 *                   be NULL
 *
 * @return <code>true</code> if the operation was started
 **/
bool startOperationWithWaiter(struct admin_state    *state,
                              AdminStateCode         operation,
                              struct vdo_completion *waiter,
                              AdminInitiator        *initiator);

/**
 * Finish the current operation. Will notify the operation waiter if there is
 * one. This method should be used for operations started with
 * startOperation(). For operations which were started with startDraining(),
 * use finishDraining() instead.
 *
 * @param state  The state whose operation is to be finished
 *
 * @return <code>true</code> if there was an operation to finish
 **/
bool finishOperation(struct admin_state *state);

/**
 * Finish the current operation with a status code. Will notify the operation
 * waiter if there is one.
 *
 * @param state   The state whose operation is to be finished
 * @param result  The result of the operation
 **/
bool finishOperationWithResult(struct admin_state *state, int result);

/**
 * Set a result for the current operation.
 *
 * @param state  the admin_state
 * @param result the result to set; if there is no waiter, this is a no-op
 **/
static inline void setOperationResult(struct admin_state *state, int result)
{
  if (state->waiter != NULL) {
    setCompletionResult(state->waiter, result);
  }
}

#endif // ADMIN_STATE_H
