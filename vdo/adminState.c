/*
 * Copyright Red Hat
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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/adminState.c#1 $
 */

#include "adminState.h"

#include "logger.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"

/**********************************************************************/
const char *get_vdo_admin_state_code_name(enum admin_state_code code)
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

	case ADMIN_STATE_NEW:
		return "ADMIN_STATE_NEW";

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
const char *get_vdo_admin_state_name(const struct admin_state *state)
{
	return get_vdo_admin_state_code_name(state->current_state);
}

/**********************************************************************/
static enum admin_state_code
get_next_state(enum admin_state_code previous_state,
	       enum admin_state_code operation)
{
	if (is_vdo_quiescing_code(operation)) {
		return ((operation & ADMIN_TYPE_MASK) | ADMIN_FLAG_QUIESCENT);
	}

	if (operation == ADMIN_STATE_SUSPENDED_OPERATION) {
		return previous_state;
	}

	return ADMIN_STATE_NORMAL_OPERATION;
}

/**
 * Finish an operation if one is in progress. If there is a waiter, it will be
 * notified.
 *
 * @param state   The admin_state
 * @param result  The result of the operation
 *
 * @return <code>true</code> if an operation was in progress and has been
 *         finished.
 **/
static bool end_operation(struct admin_state *state, int result)
{
	if (!is_vdo_state_operating(state)) {
		return false;
	}

	if (state->starting) {
		state->complete = true;
		if (state->waiter != NULL) {
			set_vdo_completion_result(state->waiter, result);
		}
	} else {
		state->complete = false;
		WRITE_ONCE(state->current_state, state->next_state);
		release_vdo_completion_with_result(&state->waiter, result);
	}

	return true;
}

/**
 * Begin an operation if it may be started given the current state.
 *
 * @param state      The admin_state
 * @param operation  The operation to begin
 * @param waiter     A completion to notify when the operation is complete; may
 *                   be NULL
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check begin_operation(struct admin_state *state,
					enum admin_state_code operation,
					struct vdo_completion *waiter,
					vdo_admin_initiator *initiator)
{
	int result;
	if (is_vdo_state_operating(state) ||
	    (is_vdo_state_quiescent(state) != is_vdo_quiescent_operation(operation))) {
		result =
		  log_error_strerror(VDO_INVALID_ADMIN_STATE,
				     "Can't start %s from %s",
				     get_vdo_admin_state_code_name(operation),
				     get_vdo_admin_state_name(state));
	} else if (state->waiter != NULL) {
		result =
		  log_error_strerror(VDO_COMPONENT_BUSY,
				     "Can't start %s with extant waiter",
				     get_vdo_admin_state_code_name(operation));
	} else {
		state->waiter = waiter;
		state->next_state =
			get_next_state(state->current_state, operation);
		WRITE_ONCE(state->current_state, operation);
		if (initiator != NULL) {
			state->starting = true;
			initiator(state);
			state->starting = false;
			if (state->complete) {
				end_operation(state, VDO_SUCCESS);
			}
		}

		return VDO_SUCCESS;
	}

	if (waiter != NULL) {
		finish_vdo_completion(waiter, result);
	}

	return result;
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
static bool check_code(bool valid,
		       enum admin_state_code code,
		       const char *what,
		       struct vdo_completion *waiter)
{
	int result;

	if (valid) {
		return true;
	}

	result = log_error_strerror(VDO_INVALID_ADMIN_STATE,
				    "%s is not a %s",
				    get_vdo_admin_state_code_name(code),
				    what);
	if (waiter != NULL) {
		finish_vdo_completion(waiter, result);
	}

	return false;
}

/**********************************************************************/
bool assert_vdo_drain_operation(enum admin_state_code operation,
				struct vdo_completion *waiter)
{
	return check_code(is_vdo_drain_operation(operation),
			  operation,
			  "drain operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_draining(struct admin_state *state,
			enum admin_state_code operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	return (assert_vdo_drain_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}

/**********************************************************************/
bool finish_vdo_draining(struct admin_state *state)
{
	return finish_vdo_draining_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_draining_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_draining(state) && end_operation(state, result));
}

/**********************************************************************/
bool assert_vdo_load_operation(enum admin_state_code operation,
			       struct vdo_completion *waiter)
{
	return check_code(is_vdo_load_operation(operation),
			  operation,
			  "load operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_loading(struct admin_state *state,
		       enum admin_state_code operation,
		       struct vdo_completion *waiter,
		       vdo_admin_initiator *initiator)
{
	return (assert_vdo_load_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}

/**********************************************************************/
bool finish_vdo_loading(struct admin_state *state)
{
	return finish_vdo_loading_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_loading_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_loading(state) && end_operation(state, result));
}

/**********************************************************************/
bool assert_vdo_resume_operation(enum admin_state_code operation,
				 struct vdo_completion *waiter)
{
	return check_code(is_vdo_resume_operation(operation),
			  operation,
			  "resume operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_resuming(struct admin_state *state,
			enum admin_state_code operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	return (assert_vdo_resume_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}

/**********************************************************************/
bool finish_vdo_resuming(struct admin_state *state)
{
	return finish_vdo_resuming_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_resuming_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_resuming(state) && end_operation(state, result));
}

/**********************************************************************/
int resume_vdo_if_quiescent(struct admin_state *state)
{
	if (!is_vdo_state_quiescent(state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	WRITE_ONCE(state->current_state, ADMIN_STATE_NORMAL_OPERATION);
	return VDO_SUCCESS;
}

/**
 * Check whether an enum admin_state_code is an operation.
 *
 * @param code    The operation to check
 * @param waiter  The completion to notify if the code is not an operation; may
 *                be NULL
 *
 * @return <code>true</code> if the code is an operation
 **/
static bool assert_operation(enum admin_state_code code,
			     struct vdo_completion *waiter)
{
	return check_code(is_vdo_operation_state_code(code),
			  code, "operation", waiter);
}

/**********************************************************************/
int start_vdo_operation(struct admin_state *state,
			enum admin_state_code operation)
{
	return (assert_operation(operation, NULL) ?
			begin_operation(state, operation, NULL, NULL) :
			VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
bool start_vdo_operation_with_waiter(struct admin_state *state,
				     enum admin_state_code operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator)
{
	return (assert_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}

/**********************************************************************/
bool finish_vdo_operation(struct admin_state *state)
{
	return finish_vdo_operation_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_operation_with_result(struct admin_state *state, int result)
{
	return end_operation(state, result);
}
