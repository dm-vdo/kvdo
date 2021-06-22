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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#31 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

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

	/** The mask for extracting the admin_type from an admin_state_code */
	VDO_ADMIN_TYPE_MASK = 0xff,
};

/**
 * The bit position of flags used to categorize states.
 **/
enum admin_flag_bit {
	VDO_ADMIN_FLAG_BIT_START = 8,
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
	VDO_ADMIN_FLAG_DRAINING = (uint32_t) (1 << VDO_ADMIN_FLAG_BIT_DRAINING),
	VDO_ADMIN_FLAG_LOADING = (uint32_t) (1 << VDO_ADMIN_FLAG_BIT_LOADING),
	VDO_ADMIN_FLAG_QUIESCING = (uint32_t) (1 << VDO_ADMIN_FLAG_BIT_QUIESCING),
	VDO_ADMIN_FLAG_QUIESCENT = (uint32_t) (1 << VDO_ADMIN_FLAG_BIT_QUIESCENT),
	VDO_ADMIN_FLAG_OPERATING = (uint32_t) (1 << VDO_ADMIN_FLAG_BIT_OPERATING),
};

/**
 * The state codes.
 **/
enum admin_state_code {
	VDO_ADMIN_STATE_NORMAL_OPERATION = VDO_ADMIN_TYPE_NORMAL,
	VDO_ADMIN_STATE_OPERATING =
		(VDO_ADMIN_TYPE_NORMAL | VDO_ADMIN_FLAG_OPERATING),
	VDO_ADMIN_STATE_FORMATTING =
		(VDO_ADMIN_TYPE_FORMAT | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_LOADING),
	VDO_ADMIN_STATE_LOADING =
		(VDO_ADMIN_TYPE_NORMAL | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_LOADING),
	VDO_ADMIN_STATE_LOADING_FOR_RECOVERY =
		(VDO_ADMIN_TYPE_RECOVER | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_LOADING),
	VDO_ADMIN_STATE_LOADING_FOR_REBUILD =
		(VDO_ADMIN_TYPE_REBUILD | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_LOADING),
	VDO_ADMIN_STATE_WAITING_FOR_RECOVERY =
		(VDO_ADMIN_TYPE_RECOVER | VDO_ADMIN_FLAG_OPERATING),
	VDO_ADMIN_STATE_NEW =
		(VDO_ADMIN_TYPE_NORMAL | VDO_ADMIN_FLAG_QUIESCENT),
	VDO_ADMIN_STATE_RECOVERING =
		(VDO_ADMIN_TYPE_RECOVER | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING),
	VDO_ADMIN_STATE_REBUILDING =
		(VDO_ADMIN_TYPE_REBUILD | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING),
	VDO_ADMIN_STATE_SAVING =
		(VDO_ADMIN_TYPE_SAVE | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING | VDO_ADMIN_FLAG_QUIESCING),
	VDO_ADMIN_STATE_SAVED =
		(VDO_ADMIN_TYPE_SAVE | VDO_ADMIN_FLAG_QUIESCENT),
	VDO_ADMIN_STATE_SCRUBBING =
		(VDO_ADMIN_TYPE_SCRUB | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING | VDO_ADMIN_FLAG_LOADING),
	VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING =
		(VDO_ADMIN_TYPE_SCRUB | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING),
	VDO_ADMIN_STATE_SUSPENDING =
		(VDO_ADMIN_TYPE_SUSPEND | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_DRAINING | VDO_ADMIN_FLAG_QUIESCING),
	VDO_ADMIN_STATE_SUSPENDED =
		(VDO_ADMIN_TYPE_SUSPEND | VDO_ADMIN_FLAG_QUIESCENT),
	VDO_ADMIN_STATE_SUSPENDED_OPERATION =
		(VDO_ADMIN_TYPE_SUSPEND | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_QUIESCENT),
	VDO_ADMIN_STATE_RESUMING =
		(VDO_ADMIN_TYPE_RESUME | VDO_ADMIN_FLAG_OPERATING |
		 VDO_ADMIN_FLAG_QUIESCENT),
};

struct admin_state {
	/** The current administrative state */
	enum admin_state_code current_state;
	/**
	 * The next administrative state (when the current operation finishes)
	 */
	enum admin_state_code next_state;
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
 * Get the name of an enum admin_state_code for logging purposes.
 *
 * @param code  The enum admin_state_code
 *
 * @return The name of the state's code
 **/
const char * __must_check
get_vdo_admin_state_code_name(enum admin_state_code code);

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
static inline enum admin_state_code __must_check
get_vdo_admin_state_code(const struct admin_state *state)
{
	return READ_ONCE(state->current_state);
}

/**
 * Check whether an admin_state is in normal operation.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is normal
 **/
static inline bool __must_check
is_vdo_state_normal(const struct admin_state *state)
{
	return ((get_vdo_admin_state_code(state) & VDO_ADMIN_TYPE_MASK)
		== VDO_ADMIN_TYPE_NORMAL);
}

/**
 * Check whether an enum admin_state_code is an operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is an operation
 **/
static inline bool __must_check
is_vdo_operation_state_code(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_FLAG_OPERATING) == VDO_ADMIN_FLAG_OPERATING);
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
 * Check whether an admin_state is suspended.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is suspended
 **/
static inline bool __must_check
is_vdo_state_suspended(const struct admin_state *state)
{
	return (get_vdo_admin_state_code(state) == VDO_ADMIN_STATE_SUSPENDED);
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
 * Check whether an enum admin_state_code is a drain operation.
 *
 * @param code  The enum admin_state_code to check
 *
 * @return <code>true</code> if the code is for a drain operation
 **/
static inline bool __must_check
is_vdo_drain_operation(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_FLAG_DRAINING) == VDO_ADMIN_FLAG_DRAINING);
}

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
 * Check whether an enum admin_state_code is a load operation.
 *
 * @param code  The enum admin_state_code to check
 *
 * @return <code>true</code> if the code is for a load operation
 **/
static inline bool __must_check
is_vdo_load_operation(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_FLAG_LOADING) == VDO_ADMIN_FLAG_LOADING);
}

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
 * Check whether an enum admin_state_code is a resume operation.
 *
 * @param code  The enum admin_state_code to check
 *
 * @return <code>true</code> if the code is for a resume operation
 **/
static inline bool __must_check
is_vdo_resume_operation(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_TYPE_MASK) == VDO_ADMIN_TYPE_RESUME);
}

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
	enum admin_state_code code = get_vdo_admin_state_code(state);
	return ((code == VDO_ADMIN_STATE_FORMATTING) ||
		(code == VDO_ADMIN_STATE_LOADING));
}

/**
 * Check whether an enum admin_state_code is quiescing.
 *
 * param code  The enum admin_state_code to check
 *
 * @return <code>true</code> is the state is quiescing
 **/
static inline bool __must_check
is_vdo_quiescing_code(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_FLAG_QUIESCING) == VDO_ADMIN_FLAG_QUIESCING);
}

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
 * Check where an enum admin_state_code is quiescent.
 *
 * param code  The enum admin_state_code to check
 *
 * @return <code>true</code> is the state is quiescent
 **/
static inline bool __must_check
is_vdo_quiescent_code(enum admin_state_code code)
{
	return ((code & VDO_ADMIN_FLAG_QUIESCENT) == VDO_ADMIN_FLAG_QUIESCENT);
}

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
 * Check whether an enum admin_state_code is a quiescent operation.
 *
 * @param code  The code to check
 *
 * @return <code>true</code> if the code is a quiescent operation
 **/
static inline bool __must_check
is_vdo_quiescent_operation(enum admin_state_code code)
{
	return (is_vdo_quiescent_code(code) && is_vdo_operation_state_code(code));
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
bool __must_check assert_vdo_drain_operation(enum admin_state_code operation,
					     struct vdo_completion *waiter);

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
			enum admin_state_code operation,
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
bool __must_check assert_vdo_load_operation(enum admin_state_code operation,
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
		       enum admin_state_code operation,
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
 * Check whether an enum admin_state_code is a resume operation.
 *
 * @param operation  The operation to check
 * @param waiter     The completion to notify if the operation is not a resume
 *                   operation; may be NULL
 *
 * @return <code>true</code> if the code is a resume operation
 **/
bool assert_vdo_resume_operation(enum admin_state_code operation,
				 struct vdo_completion *waiter);

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
			enum admin_state_code operation,
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
			enum admin_state_code operation);

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
				     enum admin_state_code operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator);

/**
 * Finish the current operation. Will notify the operation waiter if there is
 * one. This method should be used for operations started with
 * start_vdo_operation(). For operations which were started with
 * start_vdo_draining(), use finish_vdo_draining() instead.
 *
 * @param state  The state whose operation is to be finished
 *
 * @return <code>true</code> if there was an operation to finish
 **/
bool finish_vdo_operation(struct admin_state *state);

/**
 * Finish the current operation with a status code. Will notify the operation
 * waiter if there is one.
 *
 * @param state   The state whose operation is to be finished
 * @param result  The result of the operation
 **/
bool finish_vdo_operation_with_result(struct admin_state *state, int result);

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
