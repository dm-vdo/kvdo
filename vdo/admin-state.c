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
 */

#include "admin-state.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"

/**
 * The state codes.
 **/
static const struct admin_state_code VDO_CODE_NORMAL_OPERATION = {
	.name = "VDO_ADMIN_STATE_NORMAL_OPERATION",
	.normal = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_NORMAL_OPERATION =
	&VDO_CODE_NORMAL_OPERATION;
static const struct admin_state_code VDO_CODE_OPERATING = {
	.name = "VDO_ADMIN_STATE_OPERATING",
	.normal = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_OPERATING =
	&VDO_CODE_OPERATING;
static const struct admin_state_code VDO_CODE_FORMATTING = {
	.name = "VDO_ADMIN_STATE_FORMATTING",
	.operating = true,
	.loading = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_FORMATTING =
	&VDO_CODE_FORMATTING;
static const struct admin_state_code VDO_CODE_PRE_LOADING = {
	.name = "VDO_ADMIN_STATE_PRE_LOADING",
	.operating = true,
	.loading = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADING =
	&VDO_CODE_PRE_LOADING;
static const struct admin_state_code VDO_CODE_PRE_LOADED = {
	.name = "VDO_ADMIN_STATE_PRE_LOADED",
};
const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADED =
	&VDO_CODE_PRE_LOADED;
static const struct admin_state_code VDO_CODE_LOADING = {
	.name = "VDO_ADMIN_STATE_LOADING",
	.normal = true,
	.operating = true,
	.loading = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING =
	&VDO_CODE_LOADING;
static const struct admin_state_code VDO_CODE_LOADING_FOR_RECOVERY = {
	.name = "VDO_ADMIN_STATE_LOADING_FOR_RECOVERY",
	.operating = true,
	.loading = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_RECOVERY =
	&VDO_CODE_LOADING_FOR_RECOVERY;
static const struct admin_state_code VDO_CODE_LOADING_FOR_REBUILD = {
	.name = "VDO_ADMIN_STATE_LOADING_FOR_REBUILD",
	.operating = true,
	.loading = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_REBUILD =
	&VDO_CODE_LOADING_FOR_REBUILD;
static const struct admin_state_code VDO_CODE_WAITING_FOR_RECOVERY = {
	.name = "VDO_ADMIN_STATE_WAITING_FOR_RECOVERY",
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_WAITING_FOR_RECOVERY =
	&VDO_CODE_WAITING_FOR_RECOVERY;
static const struct admin_state_code VDO_CODE_NEW = {
	.name = "VDO_ADMIN_STATE_NEW",
	.quiescent = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_NEW =
	&VDO_CODE_NEW;
static const struct admin_state_code VDO_CODE_INITIALIZED = {
	.name = "VDO_ADMIN_STATE_INITIALIZED",
};
const struct admin_state_code *VDO_ADMIN_STATE_INITIALIZED =
	&VDO_CODE_INITIALIZED;
static const struct admin_state_code VDO_CODE_RECOVERING = {
	.name = "VDO_ADMIN_STATE_RECOVERING",
	.draining = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_RECOVERING =
	&VDO_CODE_RECOVERING;
static const struct admin_state_code VDO_CODE_REBUILDING = {
	.name = "VDO_ADMIN_STATE_REBUILDING",
	.draining = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_REBUILDING =
	&VDO_CODE_REBUILDING;
static const struct admin_state_code VDO_CODE_SAVING = {
	.name = "VDO_ADMIN_STATE_SAVING",
	.draining = true,
	.quiescing = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVING =
	&VDO_CODE_SAVING;
static const struct admin_state_code VDO_CODE_SAVED = {
	.name = "VDO_ADMIN_STATE_SAVED",
	.quiescent = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVED =
	&VDO_CODE_SAVED;
static const struct admin_state_code VDO_CODE_SCRUBBING = {
	.name = "VDO_ADMIN_STATE_SCRUBBING",
	.draining = true,
	.loading = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SCRUBBING =
	&VDO_CODE_SCRUBBING;
static const struct admin_state_code VDO_CODE_SAVE_FOR_SCRUBBING = {
	.name = "VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING",
	.draining = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING =
	&VDO_CODE_SAVE_FOR_SCRUBBING;
static const struct admin_state_code VDO_CODE_STOPPING = {
	.name = "VDO_ADMIN_STATE_STOPPING",
	.draining = true,
	.quiescing = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_STOPPING =
	&VDO_CODE_STOPPING;
static const struct admin_state_code VDO_CODE_STOPPED = {
	.name = "VDO_ADMIN_STATE_STOPPED",
	.quiescent = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_STOPPED =
	&VDO_CODE_STOPPED;
static const struct admin_state_code VDO_CODE_SUSPENDING = {
	.name = "VDO_ADMIN_STATE_SUSPENDING",
	.draining = true,
	.quiescing = true,
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDING =
	&VDO_CODE_SUSPENDING;
static const struct admin_state_code VDO_CODE_SUSPENDED = {
	.name = "VDO_ADMIN_STATE_SUSPENDED",
	.quiescent = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED =
	&VDO_CODE_SUSPENDED;
static const struct admin_state_code VDO_CODE_SUSPENDED_OPERATION = {
	.name = "VDO_ADMIN_STATE_SUSPENDED_OPERATION",
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED_OPERATION =
	&VDO_CODE_SUSPENDED_OPERATION;
static const struct admin_state_code VDO_CODE_RESUMING = {
	.name = "VDO_ADMIN_STATE_RESUMING",
	.operating = true,
};
const struct admin_state_code *VDO_ADMIN_STATE_RESUMING =
	&VDO_CODE_RESUMING;

/**
 * Get the name of an admin_state_code for logging purposes.
 *
 * @param code  The admin_state_code
 *
 * @return The name of the state's code
 **/
static const char *
vdo_get_admin_state_code_name(const struct admin_state_code *code)
{
	return code->name;
}

/**
 * Get the name of an admin_state's code for logging purposes.
 *
 * @param state  The admin_state
 *
 * @return The name of the state's code
 **/
const char *vdo_get_admin_state_name(const struct admin_state *state)
{
	return vdo_get_admin_state_code_name(vdo_get_admin_state_code(state));
}

/**
 * Check whether an admin_state is operating.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is operating
 **/
static inline bool __must_check
is_vdo_state_operating(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->operating;
}

/**
 * Determine the state which should be set after a given operation completes
 * based on the operation and the current state.
 *
 * @param state      The admin_state
 * @param operation  The operation to be started
 *
 * @return The state to set when the operation completes or NULL if the
 *         operation can not be started in the current state
 **/
static const struct admin_state_code *
get_next_state(const struct admin_state *state,
	       const struct admin_state_code *operation)
{
	const struct admin_state_code *code
		= vdo_get_admin_state_code(state);

	if (code->operating) {
		return NULL;
	}

	if (operation == VDO_ADMIN_STATE_SAVING) {
		return (code == VDO_ADMIN_STATE_NORMAL_OPERATION
			? VDO_ADMIN_STATE_SAVED
			: NULL);
	}

	if (operation == VDO_ADMIN_STATE_SUSPENDING) {
		return (code == VDO_ADMIN_STATE_NORMAL_OPERATION
			? VDO_ADMIN_STATE_SUSPENDED
			: NULL);
	}

	if (operation == VDO_ADMIN_STATE_STOPPING) {
		return (code == VDO_ADMIN_STATE_NORMAL_OPERATION
			? VDO_ADMIN_STATE_STOPPED
			: NULL);
	}

	if (operation == VDO_ADMIN_STATE_PRE_LOADING) {
		return (code == VDO_ADMIN_STATE_INITIALIZED
			? VDO_ADMIN_STATE_PRE_LOADED
			: NULL);
	}

	if (operation == VDO_ADMIN_STATE_SUSPENDED_OPERATION) {
		return (((code == VDO_ADMIN_STATE_SUSPENDED)
			 || (code == VDO_ADMIN_STATE_SAVED))
			? code
			: NULL);
	}

	return VDO_ADMIN_STATE_NORMAL_OPERATION;
}

/**
 * Finish the current operation. Will notify the operation waiter if there is
 * one. This method should be used for operations started with
 * vdo_start_operation(). For operations which were started with
 * vdo_start_draining(), use vdo_finish_draining() instead.
 *
 * @param state   The state whose operation is to be finished
 * @param result  The result of the operation
 *
 * @return <code>true</code> if there was an operation to finish
 **/
bool vdo_finish_operation(struct admin_state *state, int result)
{
	if (!is_vdo_state_operating(state)) {
		return false;
	}

	state->complete = state->starting;
	if (state->waiter != NULL) {
		vdo_set_completion_result(state->waiter, result);
	}

	if (!state->starting) {
		vdo_set_admin_state_code(state, state->next_state);
		if (state->waiter != NULL) {
			vdo_complete_completion(UDS_FORGET(state->waiter));
		}
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
static int __must_check
begin_operation(struct admin_state *state,
		const struct admin_state_code *operation,
		struct vdo_completion *waiter,
		vdo_admin_initiator *initiator)
{
	int result;
	const struct admin_state_code *next_state = get_next_state(state,
								   operation);

	if (next_state == NULL) {
		result =
		  uds_log_error_strerror(VDO_INVALID_ADMIN_STATE,
					 "Can't start %s from %s",
					 vdo_get_admin_state_code_name(operation),
					 vdo_get_admin_state_name(state));
	} else if (state->waiter != NULL) {
		result =
		  uds_log_error_strerror(VDO_COMPONENT_BUSY,
					 "Can't start %s with extant waiter",
					 vdo_get_admin_state_code_name(operation));
	} else {
		state->waiter = waiter;
		state->next_state = next_state;
		vdo_set_admin_state_code(state, operation);
		if (initiator != NULL) {
			state->starting = true;
			initiator(state);
			state->starting = false;
			if (state->complete) {
				vdo_finish_operation(state, VDO_SUCCESS);
			}
		}

		return VDO_SUCCESS;
	}

	if (waiter != NULL) {
		vdo_finish_completion(waiter, result);
	}

	return result;
}

/**
 * Start an operation if it may be started given the current state.
 *
 * @param state      The admin_state
 * @param operation  The operation to begin
 * @param waiter     A completion to notify when the operation is complete
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return true if the operation was started
 **/
static inline bool __must_check
start_operation(struct admin_state *state,
		const struct admin_state_code *operation,
		struct vdo_completion *waiter,
		vdo_admin_initiator *initiator)
{
	return (begin_operation(state, operation, waiter, initiator)
		== VDO_SUCCESS);
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
		       const struct admin_state_code *code,
		       const char *what,
		       struct vdo_completion *waiter)
{
	int result;

	if (valid) {
		return true;
	}

	result = uds_log_error_strerror(VDO_INVALID_ADMIN_STATE,
					"%s is not a %s",
					vdo_get_admin_state_code_name(code),
					what);
	if (waiter != NULL) {
		vdo_finish_completion(waiter, result);
	}

	return false;
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
static bool __must_check
assert_vdo_drain_operation(const struct admin_state_code *operation,
			   struct vdo_completion *waiter)
{
	return check_code(operation->draining,
			  operation,
			  "drain operation",
			  waiter);
}

/**
 * Initiate a drain operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of drain to initiate
 * @param waiter     The completion to notify when the drain is complete (may
 *                   be NULL)
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return <code>true</code> if the drain was initiated, if not the waiter
 *         will be notified
 **/
bool vdo_start_draining(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	const struct admin_state_code *code = vdo_get_admin_state_code(state);

	if (!assert_vdo_drain_operation(operation, waiter)) {
		return false;
	}

	if (code->quiescent) {
		vdo_complete_completion(waiter);
		return false;
	}

	if (!code->normal) {
		uds_log_error_strerror(VDO_INVALID_ADMIN_STATE,
				       "can't start %s from %s",
				       operation->name,
				       code->name);
		vdo_finish_completion(waiter, VDO_INVALID_ADMIN_STATE);
		return false;
	}

	return start_operation(state, operation, waiter, initiator);
}

/**
 * Finish a drain operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was draining; will notify the waiter
 *         if so
 **/
bool vdo_finish_draining(struct admin_state *state)
{
	return vdo_finish_draining_with_result(state, VDO_SUCCESS);
}

/**
 * Finish a drain operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the drain operation
 *
 * @return <code>true</code> if the state was draining; will notify the
 *         waiter if so
 **/
bool vdo_finish_draining_with_result(struct admin_state *state, int result)
{
	return (vdo_is_state_draining(state)
		&& vdo_finish_operation(state, result));
}

/**
 * Check that an operation is a load.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to finish with an error if the operation is
 *                   not a load
 *
 * @return <code>true</code> if the specified operation is a load
 **/
bool vdo_assert_load_operation(const struct admin_state_code *operation,
			       struct vdo_completion *waiter)
{
	return check_code(operation->loading,
			  operation,
			  "load operation",
			  waiter);
}

/**
 * Initiate a load operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of load to initiate
 * @param waiter     The completion to notify when the load is complete (may be
 *                   NULL)
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return <code>true</code> if the load was initiated, if not the waiter
 *         will be notified
 **/
bool vdo_start_loading(struct admin_state *state,
		       const struct admin_state_code *operation,
		       struct vdo_completion *waiter,
		       vdo_admin_initiator *initiator)
{
	return (vdo_assert_load_operation(operation, waiter) &&
		start_operation(state, operation, waiter, initiator));
}

/**
 * Finish a load operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was loading; will notify the waiter
 *         if so
 **/
bool vdo_finish_loading(struct admin_state *state)
{
	return vdo_finish_loading_with_result(state, VDO_SUCCESS);
}

/**
 * Finish a load operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the load operation
 *
 * @return <code>true</code> if the state was loading; will notify the
 *         waiter if so
 **/
bool vdo_finish_loading_with_result(struct admin_state *state, int result)
{
	return (vdo_is_state_loading(state)
		&& vdo_finish_operation(state, result));
}

/**
 * Check whether an admin_state_code is a resume operation.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to notify if the operation is not a resume
 *                   operation; may be NULL
 *
 * @return <code>true</code> if the code is a resume operation
 **/
static bool __must_check
assert_vdo_resume_operation(const struct admin_state_code *operation,
			    struct vdo_completion *waiter)
{
	return check_code(operation == VDO_ADMIN_STATE_RESUMING,
			  operation,
			  "resume operation",
			  waiter);
}

/**
 * Initiate a resume operation if the current state permits it.
 *
 * @param state      The admin_state
 * @param operation  The type of resume to start
 * @param waiter     The completion to notify when the resume is complete (may
 *                   be NULL)
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return <code>true</code> if the resume was initiated, if not the waiter
 *         will be notified
 **/
bool vdo_start_resuming(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	return (assert_vdo_resume_operation(operation, waiter) &&
		start_operation(state, operation, waiter, initiator));
}

/**
 * Finish a resume operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was resuming; will notify the waiter
 *         if so
 **/
bool vdo_finish_resuming(struct admin_state *state)
{
	return vdo_finish_resuming_with_result(state, VDO_SUCCESS);
}

/**
 * Finish a resume operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the resume operation
 *
 * @return <code>true</code> if the state was resuming; will notify the
 *         waiter if so
 **/
bool vdo_finish_resuming_with_result(struct admin_state *state, int result)
{
	return (vdo_is_state_resuming(state)
		&& vdo_finish_operation(state, result));
}

/**
 * Change the state to normal operation if the current state is quiescent.
 *
 * @param state  The admin_state to resume
 *
 * @return VDO_SUCCESS if the state resumed, VDO_INVALID_ADMIN_STATE otherwise
 **/
int vdo_resume_if_quiescent(struct admin_state *state)
{
	if (!vdo_is_state_quiescent(state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	vdo_set_admin_state_code(state, VDO_ADMIN_STATE_NORMAL_OPERATION);
	return VDO_SUCCESS;
}

/**
 * Check whether an admin_state_code is an operation.
 *
 * @param code    The operation to check
 * @param waiter  The completion to notify if the code is not an operation; may
 *                be NULL
 *
 * @return <code>true</code> if the code is an operation
 **/
static bool assert_operation(const struct admin_state_code *code,
			     struct vdo_completion *waiter)
{
	return check_code(code->operating, code, "operation", waiter);
}

/**
 * Attempt to start an operation.
 *
 * @param state      the admin_state
 * @param operation  the operation to start
 *
 * @return VDO_SUCCESS             if the operation was started
 *         VDO_INVALID_ADMIN_STATE if not
 **/
int vdo_start_operation(struct admin_state *state,
			const struct admin_state_code *operation)
{
	return (assert_operation(operation, NULL) ?
			begin_operation(state, operation, NULL, NULL) :
			VDO_INVALID_ADMIN_STATE);
}

/**
 * Attempt to start an operation.
 *
 * @param state      the admin_state
 * @param operation  the operation to start
 * @param waiter     the completion to notify when the operation completes or
 *                   fails to start; may be NULL
 * @param initiator  The vdo_admin_initiator to call if the operation may
 *                   begin; may be NULL
 *
 * @return <code>true</code> if the operation was started
 **/
bool vdo_start_operation_with_waiter(struct admin_state *state,
				     const struct admin_state_code *operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator)
{
	return (assert_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}

