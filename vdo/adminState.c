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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.c#35 $
 */

#include "adminState.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "types.h"

/**
 * The list of state types.
 **/
enum admin_type {
	/** Normal operation, data_vios may be active */
	VDO_ADMIN_TYPE_NORMAL = 0,
	/**
	 * Format: an operation for formatting a new VDO.
	 **/
	VDO_ADMIN_TYPE_FORMAT,
	/**
	 * Preload: an operation which must be performed before loading.
	 **/
	VDO_ADMIN_TYPE_PRE_LOAD,
	/**
	 * Recover: a recovery operation.
	 **/
	VDO_ADMIN_TYPE_RECOVER,
	/**
	 * Rebuild: write data necessary for a full rebuild, drain outstanding
	 *          I/O, and return to normal operation.
	 **/
	VDO_ADMIN_TYPE_REBUILD,
	/**
	 * Save: write all dirty metadata thereby restoring the VDO to a clean
	 *       state, drain outstanding I/O, and become quiescent.
	 **/
	VDO_ADMIN_TYPE_SAVE,
	/**
	 * Scrub: load and/or save state necessary to scrub a slab.
	 **/
	VDO_ADMIN_TYPE_SCRUB,
	/**
	 * Suspend: write enough dirty metadata to perform resize transactions,
	 *          drain outstanding I/O, and become quiescent.
	 **/
	VDO_ADMIN_TYPE_SUSPEND,
	/**
	 * Resume: return to normal from a quiescent state
	 **/
	VDO_ADMIN_TYPE_RESUME,
};

/**
 * The bit position of flags used to categorize states.
 **/
enum admin_flag_bit {
	VDO_ADMIN_FLAG_BIT_START = 0,
	/** Flag indicating that I/O is draining */
	VDO_ADMIN_FLAG_BIT_DRAINING = VDO_ADMIN_FLAG_BIT_START,
	/** Flag indicating a load operation */
	VDO_ADMIN_FLAG_BIT_LOADING,
	/** Flag indicating that the next state will be a quiescent state */
	VDO_ADMIN_FLAG_BIT_QUIESCING,
	/** Flag indicating that the state is quiescent */
	VDO_ADMIN_FLAG_BIT_QUIESCENT,
	/**
	 * Flag indicating that an operation is in progress and so no other
	 * operation may be started.
	 **/
	VDO_ADMIN_FLAG_BIT_OPERATING,
};

/**
 * The flags themselves.
 **/
enum admin_flag {
	VDO_ADMIN_FLAG_DRAINING = (uint16_t) (1 << VDO_ADMIN_FLAG_BIT_DRAINING),
	VDO_ADMIN_FLAG_LOADING = (uint16_t) (1 << VDO_ADMIN_FLAG_BIT_LOADING),
	VDO_ADMIN_FLAG_QUIESCING = (uint16_t) (1 << VDO_ADMIN_FLAG_BIT_QUIESCING),
	VDO_ADMIN_FLAG_QUIESCENT = (uint16_t) (1 << VDO_ADMIN_FLAG_BIT_QUIESCENT),
	VDO_ADMIN_FLAG_OPERATING = (uint16_t) (1 << VDO_ADMIN_FLAG_BIT_OPERATING),
};

struct admin_state_code {
	const char *name;
	enum admin_type type;
	uint16_t flags;
};

/**
 * The state codes.
 **/
static const struct admin_state_code VDO_CODE_NORMAL_OPERATION = {
	.name = "VDO_ADMIN_STATE_NORMAL_OPERATION",
	.type = VDO_ADMIN_TYPE_NORMAL,
	.flags = 0,
};
const struct admin_state_code *VDO_ADMIN_STATE_NORMAL_OPERATION =
	&VDO_CODE_NORMAL_OPERATION;
static const struct admin_state_code VDO_CODE_OPERATING = {
	.name = "VDO_ADMIN_STATE_OPERATING",
	.type = VDO_ADMIN_TYPE_NORMAL,
	.flags = VDO_ADMIN_FLAG_OPERATING,
	};
const struct admin_state_code *VDO_ADMIN_STATE_OPERATING =
	&VDO_CODE_OPERATING;
static const struct admin_state_code VDO_CODE_FORMATTING = {
	.name = "VDO_ADMIN_STATE_FORMATTING",
	.type = VDO_ADMIN_TYPE_FORMAT,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_FORMATTING =
	&VDO_CODE_FORMATTING;
static const struct admin_state_code VDO_CODE_PRE_LOADING = {
	.name = "VDO_ADMIN_STATE_PRE_LOADING",
	.type = VDO_ADMIN_TYPE_PRE_LOAD,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADING =
	&VDO_CODE_PRE_LOADING;
static const struct admin_state_code VDO_CODE_LOADING = {
	.name = "VDO_ADMIN_STATE_LOADING",
	.type = VDO_ADMIN_TYPE_NORMAL,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING =
	&VDO_CODE_LOADING;
static const struct admin_state_code VDO_CODE_LOADING_FOR_RECOVERY = {
	.name = "VDO_ADMIN_STATE_LOADING_FOR_RECOVERY",
	.type = VDO_ADMIN_TYPE_RECOVER,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_RECOVERY =
	&VDO_CODE_LOADING_FOR_RECOVERY;
static const struct admin_state_code VDO_CODE_LOADING_FOR_REBUILD = {
	.name = "VDO_ADMIN_STATE_LOADING_FOR_REBUILD",
	.type = VDO_ADMIN_TYPE_REBUILD,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_REBUILD =
	&VDO_CODE_LOADING_FOR_REBUILD;
static const struct admin_state_code VDO_CODE_WAITING_FOR_RECOVERY = {
	.name = "VDO_ADMIN_STATE_WAITING_FOR_RECOVERY",
	.type = VDO_ADMIN_TYPE_RECOVER,
	.flags = (VDO_ADMIN_FLAG_OPERATING),
};
const struct admin_state_code *VDO_ADMIN_STATE_WAITING_FOR_RECOVERY =
	&VDO_CODE_WAITING_FOR_RECOVERY;
static const struct admin_state_code VDO_CODE_NEW = {
	.name = "VDO_ADMIN_STATE_NEW",
	.type = VDO_ADMIN_TYPE_NORMAL,
	.flags = (VDO_ADMIN_FLAG_QUIESCENT),
};
const struct admin_state_code *VDO_ADMIN_STATE_NEW =
	&VDO_CODE_NEW;
static const struct admin_state_code VDO_CODE_RECOVERING = {
	.name = "VDO_ADMIN_STATE_RECOVERING",
	.type = VDO_ADMIN_TYPE_RECOVER,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING),
};
const struct admin_state_code *VDO_ADMIN_STATE_RECOVERING =
	&VDO_CODE_RECOVERING;
static const struct admin_state_code VDO_CODE_REBUILDING = {
	.name = "VDO_ADMIN_STATE_REBUILDING",
	.type = VDO_ADMIN_TYPE_REBUILD,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING),
};
const struct admin_state_code *VDO_ADMIN_STATE_REBUILDING =
	&VDO_CODE_REBUILDING;
static const struct admin_state_code VDO_CODE_SAVING = {
	.name = "VDO_ADMIN_STATE_SAVING",
	.type = VDO_ADMIN_TYPE_SAVE,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING
		  | VDO_ADMIN_FLAG_QUIESCING),
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVING =
	&VDO_CODE_SAVING;
static const struct admin_state_code VDO_CODE_SAVED = {
	.name = "VDO_ADMIN_STATE_SAVED",
	.type = VDO_ADMIN_TYPE_SAVE,
	.flags = (VDO_ADMIN_FLAG_QUIESCENT),
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVED =
	&VDO_CODE_SAVED;
static const struct admin_state_code VDO_CODE_SCRUBBING = {
	.name = "VDO_ADMIN_STATE_SCRUBBING",
	.type = VDO_ADMIN_TYPE_SCRUB,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING
		  | VDO_ADMIN_FLAG_LOADING),
};
const struct admin_state_code *VDO_ADMIN_STATE_SCRUBBING =
	&VDO_CODE_SCRUBBING;
static const struct admin_state_code VDO_CODE_SAVE_FOR_SCRUBBING = {
	.name = "VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING",
	.type = VDO_ADMIN_TYPE_SCRUB,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING),
};
const struct admin_state_code *VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING =
	&VDO_CODE_SAVE_FOR_SCRUBBING;
static const struct admin_state_code VDO_CODE_SUSPENDING = {
	.name = "VDO_ADMIN_STATE_SUSPENDING",
	.type = VDO_ADMIN_TYPE_SUSPEND,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_DRAINING
		  | VDO_ADMIN_FLAG_QUIESCING),
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDING =
	&VDO_CODE_SUSPENDING;
static const struct admin_state_code VDO_CODE_SUSPENDED = {
	.name = "VDO_ADMIN_STATE_SUSPENDED",
	.type = VDO_ADMIN_TYPE_SUSPEND,
	.flags = VDO_ADMIN_FLAG_QUIESCENT,
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED =
	&VDO_CODE_SUSPENDED;
static const struct admin_state_code VDO_CODE_SUSPENDED_OPERATION = {
	.name = "VDO_ADMIN_STATE_SUSPENDED_OPERATION",
	.type = VDO_ADMIN_TYPE_SUSPEND,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_QUIESCENT),
};
const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED_OPERATION =
	&VDO_CODE_SUSPENDED_OPERATION;
static const struct admin_state_code VDO_CODE_RESUMING = {
	.name = "VDO_ADMIN_STATE_RESUMING",
	.type = VDO_ADMIN_TYPE_RESUME,
	.flags = (VDO_ADMIN_FLAG_OPERATING
		  | VDO_ADMIN_FLAG_QUIESCENT),
};
const struct admin_state_code *VDO_ADMIN_STATE_RESUMING =
	&VDO_CODE_RESUMING;

/**********************************************************************/
const char *get_vdo_admin_state_code_name(const struct admin_state_code *code)
{
	return code->name;
}

/**********************************************************************/
const char *get_vdo_admin_state_name(const struct admin_state *state)
{
	return get_vdo_admin_state_code_name(get_vdo_admin_state_code(state));
}

/**********************************************************************/
bool is_vdo_state_normal(const struct admin_state *state)
{
	return (get_vdo_admin_state_code(state)->type
		== VDO_ADMIN_TYPE_NORMAL);
}

/**
 * Check whether an admin_state_code has a given flag set.
 *
 * @param code  The code to check
 * @param flag  The desired flag
 **/
static bool __must_check code_has_flag(const struct admin_state_code *code,
				       enum admin_flag flag)
{
	return ((code->flags & flag) == flag);
}

/**
 * Check whether an admin_state_code is an operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is an operation
 **/
static inline bool __must_check
is_vdo_operation_state_code(const struct admin_state_code *code)
{
	return code_has_flag(code, VDO_ADMIN_FLAG_OPERATING);
}

/**********************************************************************/
bool is_vdo_drain_operation(const struct admin_state_code *code)
{
	return code_has_flag(code, VDO_ADMIN_FLAG_DRAINING);
}

/**********************************************************************/
bool is_vdo_load_operation(const struct admin_state_code *code)
{
	return code_has_flag(code, VDO_ADMIN_FLAG_LOADING);
}

/**********************************************************************/
bool is_vdo_resume_operation(const struct admin_state_code *code)
{
	return (code->type == VDO_ADMIN_TYPE_RESUME);
}

/**********************************************************************/
bool is_vdo_quiescing_code(const struct admin_state_code *code)
{
	return code_has_flag(code, VDO_ADMIN_FLAG_QUIESCING);
}

/**********************************************************************/
bool is_vdo_quiescent_code(const struct admin_state_code *code)
{
	return code_has_flag(code, VDO_ADMIN_FLAG_QUIESCENT);
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
	return is_vdo_operation_state_code(get_vdo_admin_state_code(state));
}

/**
 * Check whether an admin_state_code is a quiescent operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is a quiescent operation
 **/
static inline bool __must_check
is_vdo_quiescent_operation(const struct admin_state_code *code)
{
	return (is_vdo_quiescent_code(code)
		&& is_vdo_operation_state_code(code));
}

/**********************************************************************/
static const struct admin_state_code *
get_next_state(const struct admin_state_code *previous_state,
	       const struct admin_state_code *operation)
{
	if (operation == VDO_ADMIN_STATE_SAVING) {
		return VDO_ADMIN_STATE_SAVED;
	}

	if (operation == VDO_ADMIN_STATE_SUSPENDING) {
		return VDO_ADMIN_STATE_SUSPENDED;
	}

	if (operation == VDO_ADMIN_STATE_SUSPENDED_OPERATION) {
		return previous_state;
	}

	return VDO_ADMIN_STATE_NORMAL_OPERATION;
}

/**********************************************************************/
bool finish_vdo_operation(struct admin_state *state, int result)
{
	if (!is_vdo_state_operating(state)) {
		return false;
	}

	state->complete = state->starting;
	if (state->waiter != NULL) {
		set_vdo_completion_result(state->waiter, result);
	}

	if (!state->starting) {
		set_vdo_admin_state_code(state, state->next_state);
		if (state->waiter != NULL) {
			complete_vdo_completion(UDS_FORGET(state->waiter));
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
	if (is_vdo_state_operating(state) ||
	    (is_vdo_state_quiescent(state) != is_vdo_quiescent_operation(operation))) {
		result =
		  uds_log_error_strerror(VDO_INVALID_ADMIN_STATE,
					 "Can't start %s from %s",
					 get_vdo_admin_state_code_name(operation),
					 get_vdo_admin_state_name(state));
	} else if (state->waiter != NULL) {
		result =
		  uds_log_error_strerror(VDO_COMPONENT_BUSY,
					 "Can't start %s with extant waiter",
					 get_vdo_admin_state_code_name(operation));
	} else {
		state->waiter = waiter;
		state->next_state = get_next_state(state->current_state,
						   operation);
		set_vdo_admin_state_code(state, operation);
		if (initiator != NULL) {
			state->starting = true;
			initiator(state);
			state->starting = false;
			if (state->complete) {
				finish_vdo_operation(state, VDO_SUCCESS);
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
					get_vdo_admin_state_code_name(code),
					what);
	if (waiter != NULL) {
		finish_vdo_completion(waiter, result);
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
	return check_code(is_vdo_drain_operation(operation),
			  operation,
			  "drain operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_draining(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	return (assert_vdo_drain_operation(operation, waiter)
		&& start_operation(state, operation, waiter, initiator));
}

/**********************************************************************/
bool finish_vdo_draining(struct admin_state *state)
{
	return finish_vdo_draining_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_draining_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_draining(state)
		&& finish_vdo_operation(state, result));
}

/**********************************************************************/
bool assert_vdo_load_operation(const struct admin_state_code *operation,
			       struct vdo_completion *waiter)
{
	return check_code(is_vdo_load_operation(operation),
			  operation,
			  "load operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_loading(struct admin_state *state,
		       const struct admin_state_code *operation,
		       struct vdo_completion *waiter,
		       vdo_admin_initiator *initiator)
{
	return (assert_vdo_load_operation(operation, waiter) &&
		start_operation(state, operation, waiter, initiator));
}

/**********************************************************************/
bool finish_vdo_loading(struct admin_state *state)
{
	return finish_vdo_loading_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_loading_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_loading(state)
		&& finish_vdo_operation(state, result));
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
	return check_code(is_vdo_resume_operation(operation),
			  operation,
			  "resume operation",
			  waiter);
}

/**********************************************************************/
bool start_vdo_resuming(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator)
{
	return (assert_vdo_resume_operation(operation, waiter) &&
		start_operation(state, operation, waiter, initiator));
}

/**********************************************************************/
bool finish_vdo_resuming(struct admin_state *state)
{
	return finish_vdo_resuming_with_result(state, VDO_SUCCESS);
}

/**********************************************************************/
bool finish_vdo_resuming_with_result(struct admin_state *state, int result)
{
	return (is_vdo_state_resuming(state)
		&& finish_vdo_operation(state, result));
}

/**********************************************************************/
int resume_vdo_if_quiescent(struct admin_state *state)
{
	if (!is_vdo_state_quiescent(state)) {
		return VDO_INVALID_ADMIN_STATE;
	}

	set_vdo_admin_state_code(state, VDO_ADMIN_STATE_NORMAL_OPERATION);
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
	return check_code(is_vdo_operation_state_code(code),
			  code, "operation", waiter);
}

/**********************************************************************/
int start_vdo_operation(struct admin_state *state,
			const struct admin_state_code *operation)
{
	return (assert_operation(operation, NULL) ?
			begin_operation(state, operation, NULL, NULL) :
			VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
bool start_vdo_operation_with_waiter(struct admin_state *state,
				     const struct admin_state_code *operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator)
{
	return (assert_operation(operation, waiter) &&
		(begin_operation(state, operation, waiter, initiator) ==
		 VDO_SUCCESS));
}
