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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#36 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

/**
 * The state codes.
 **/
extern const struct admin_state_code *VDO_ADMIN_STATE_NORMAL_OPERATION;
extern const struct admin_state_code *VDO_ADMIN_STATE_OPERATING;
extern const struct admin_state_code *VDO_ADMIN_STATE_FORMATTING;
extern const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADING;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_RECOVERY;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_REBUILD;
extern const struct admin_state_code *VDO_ADMIN_STATE_WAITING_FOR_RECOVERY;
extern const struct admin_state_code *VDO_ADMIN_STATE_NEW;
extern const struct admin_state_code *VDO_ADMIN_STATE_RECOVERING;
extern const struct admin_state_code *VDO_ADMIN_STATE_REBUILDING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVED;
extern const struct admin_state_code *VDO_ADMIN_STATE_SCRUBBING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED;
extern const struct admin_state_code *VDO_ADMIN_STATE_SUSPENDED_OPERATION;
extern const struct admin_state_code *VDO_ADMIN_STATE_RESUMING;

struct admin_state {
	/** The current administrative state */
	const struct admin_state_code *current_state;
	/**
	 * The next administrative state (when the current operation finishes)
	 */
	const struct admin_state_code *next_state;
	/** A completion waiting on a state change */
	struct vdo_completion *waiter;
	/** Whether an operation is being initiated */
	bool starting;
	/** Whether an operation has completed in the initiator */
	bool complete;
};

/**
 * A method to be called once an admin operation may be initiated.
 **/
typedef void vdo_admin_initiator(struct admin_state *state);

/**
 * Get the name of an admin_state_code for logging purposes.
 *
 * @param code  The admin_state_code
 *
 * @return The name of the state's code
 **/
const char * __must_check
get_vdo_admin_state_code_name(const struct admin_state_code *code);

/**
 * Get the name of an admin_state's code for logging purposes.
 *
 * @param state  The admin_state
 *
 * @return The name of the state's code
 **/
const char * __must_check
get_vdo_admin_state_name(const struct admin_state *state);

/**
 * Get the current admin state code.
 *
 * @param state  The admin_state to query
 *
 * @return The current state
 **/
static inline const struct admin_state_code * __must_check
get_vdo_admin_state_code(const struct admin_state *state)
{
	return READ_ONCE(state->current_state);
}

/**
 * Set the current admin state code. This function should be used primarily for
 * initialization and by adminState internals. Most uses should go through the
 * operation interfaces.
 *
 * @param state  The admin_state to modify
 * @param code   The code to set
 **/
static inline void
set_vdo_admin_state_code(struct admin_state *state,
			 const struct admin_state_code *code)
{
	WRITE_ONCE(state->current_state, code);
}

/**
 * Check whether an admin_state is in normal operation.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is normal
 **/
bool __must_check is_vdo_state_normal(const struct admin_state *state);

/**
 * Check whether an admin_state is suspending.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is suspending
 **/
static inline bool __must_check
is_vdo_state_suspending(const struct admin_state *state)
{
	return (get_vdo_admin_state_code(state) == VDO_ADMIN_STATE_SUSPENDING);
}

/**
 * Check whether an admin_state is saving.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saving
 **/
static inline bool __must_check
is_vdo_state_saving(const struct admin_state *state)
{
	return (get_vdo_admin_state_code(state) == VDO_ADMIN_STATE_SAVING);
}

/**
 * Check whether an admin_state is saved.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saved
 **/
static inline bool __must_check
is_vdo_state_saved(const struct admin_state *state)
{
	return (get_vdo_admin_state_code(state) == VDO_ADMIN_STATE_SAVED);
}

/**
 * Check whether an admin_state_code is a drain operation.
 *
 * @param code  The admin_state_code to check
 *
 * @return <code>true</code> if the code is for a drain operation
 **/
bool __must_check is_vdo_drain_operation(const struct admin_state_code *code);

/**
 * Check whether an admin_state is draining.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is draining
 **/
static inline bool __must_check
is_vdo_state_draining(const struct admin_state *state)
{
	return is_vdo_drain_operation(get_vdo_admin_state_code(state));
}

/**
 * Check whether an admin_state_code is a load operation.
 *
 * @param code  The admin_state_code to check
 *
 * @return <code>true</code> if the code is for a load operation
 **/
bool __must_check is_vdo_load_operation(const struct admin_state_code *code);

/**
 * Check whether an admin_state is loading.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is loading
 **/
static inline bool __must_check
is_vdo_state_loading(const struct admin_state *state)
{
	return is_vdo_load_operation(get_vdo_admin_state_code(state));
}

/**
 * Check whether an admin_state_code is a resume operation.
 *
 * @param code  The admin_state_code to check
 *
 * @return <code>true</code> if the code is for a resume operation
 **/
bool __must_check is_vdo_resume_operation(const struct admin_state_code *code);

/**
 * Check whether an admin_state is resumeing.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is resumeing
 **/
static inline bool __must_check
is_vdo_state_resuming(const struct admin_state *state)
{
	return is_vdo_resume_operation(get_vdo_admin_state_code(state));
}

/**
 * Check whether an admin_state is doing a clean load.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is a clean load
 **/
static inline bool __must_check
is_vdo_state_clean_load(const struct admin_state *state)
{
	const struct admin_state_code *code = get_vdo_admin_state_code(state);
	return ((code == VDO_ADMIN_STATE_FORMATTING) ||
		(code == VDO_ADMIN_STATE_LOADING));
}

/**
 * Check whether an admin_state_code is quiescing.
 *
 * @param code  The admin_state_code to check
 *
 * @return <code>true</code> is the state is quiescing
 **/
bool __must_check is_vdo_quiescing_code(const struct admin_state_code *code);

/**
 * Check whether an admin_state is quiescing.
 *
 * @param state  The admin_state to check
 *
 * @return <code>true</code> if the state is quiescing
 **/
static inline bool __must_check
is_vdo_state_quiescing(const struct admin_state *state)
{
	return is_vdo_quiescing_code(get_vdo_admin_state_code(state));
}

/**
 * Check where an admin_state_code is quiescent.
 *
 * @param code  The admin_state_code to check
 *
 * @return <code>true</code> is the state is quiescent
 **/
bool __must_check is_vdo_quiescent_code(const struct admin_state_code *code);

/**
 * Check whether an admin_state is quiescent.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> is the state is quiescent
 **/
static inline bool __must_check
is_vdo_state_quiescent(const struct admin_state *state)
{
	return is_vdo_quiescent_code(get_vdo_admin_state_code(state));
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
bool start_vdo_draining(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator);

/**
 * Finish a drain operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was draining; will notify the waiter
 *         if so
 **/
bool finish_vdo_draining(struct admin_state *state);

/**
 * Finish a drain operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the drain operation
 *
 * @return <code>true</code> if the state was draining; will notify the
 *         waiter if so
 **/
bool finish_vdo_draining_with_result(struct admin_state *state, int result);

/**
 * Check that an operation is a load.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to finish with an error if the operation is
 *                   not a load
 *
 * @return <code>true</code> if the specified operation is a load
 **/
bool __must_check assert_vdo_load_operation(const struct admin_state_code *operation,
					    struct vdo_completion *waiter);

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
bool start_vdo_loading(struct admin_state *state,
		       const struct admin_state_code *operation,
		       struct vdo_completion *waiter,
		       vdo_admin_initiator *initiator);

/**
 * Finish a load operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was loading; will notify the waiter
 *         if so
 **/
bool finish_vdo_loading(struct admin_state *state);

/**
 * Finish a load operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the load operation
 *
 * @return <code>true</code> if the state was loading; will notify the
 *         waiter if so
 **/
bool finish_vdo_loading_with_result(struct admin_state *state, int result);

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
bool start_vdo_resuming(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator);

/**
 * Finish a resume operation if one was in progress.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state was resuming; will notify the waiter
 *         if so
 **/
bool finish_vdo_resuming(struct admin_state *state);

/**
 * Finish a resume operation with a status code.
 *
 * @param state   The admin_state to query
 * @param result  The result of the resume operation
 *
 * @return <code>true</code> if the state was resuming; will notify the
 *         waiter if so
 **/
bool finish_vdo_resuming_with_result(struct admin_state *state, int result);

/**
 * Change the state to normal operation if the current state is quiescent.
 *
 * @param state  The admin_state to resume
 *
 * @return VDO_SUCCESS if the state resumed, VDO_INVALID_ADMIN_STATE otherwise
 **/
int resume_vdo_if_quiescent(struct admin_state *state);

/**
 * Attempt to start an operation.
 *
 * @param state      the admin_state
 * @param operation  the operation to start
 *
 * @return VDO_SUCCESS             if the operation was started
 *         VDO_INVALID_ADMIN_STATE if not
 **/
int start_vdo_operation(struct admin_state *state,
			const struct admin_state_code *operation);

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
bool start_vdo_operation_with_waiter(struct admin_state *state,
				     const struct admin_state_code *operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator);

/**
 * Finish the current operation. Will notify the operation waiter if there is
 * one. This method should be used for operations started with
 * start_vdo_operation(). For operations which were started with
 * start_vdo_draining(), use finish_vdo_draining() instead.
 *
 * @param state   The state whose operation is to be finished
 * @param result  The result of the operation
 *
 * @return <code>true</code> if there was an operation to finish
 **/
bool finish_vdo_operation(struct admin_state *state, int result);

/**
 * Set a result for the current operation.
 *
 * @param state  the admin_state
 * @param result the result to set; if there is no waiter, this is a no-op
 **/
static inline void set_vdo_operation_result(struct admin_state *state,
					    int result)
{
	if (state->waiter != NULL) {
		set_vdo_completion_result(state->waiter, result);
	}
}

#endif // ADMIN_STATE_H
