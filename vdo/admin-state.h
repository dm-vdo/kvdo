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

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

struct admin_state_code {
	const char *name;
	/** Normal operation, data_vios may be active */
	bool normal;
	/** I/O is draining, new requests should not start */
	bool draining;
	/** This is a startup time operation */
	bool loading;
	/** The next state will be quiescent */
	bool quiescing;
	/** The VDO is quiescent, there should be no I/O */
	bool quiescent;
	/**
	 * Whether an operation is in progress and so no other operation may be
	 * started
	 */
	bool operating;
};

/**
 * The state codes.
 **/
extern const struct admin_state_code *VDO_ADMIN_STATE_NORMAL_OPERATION;
extern const struct admin_state_code *VDO_ADMIN_STATE_OPERATING;
extern const struct admin_state_code *VDO_ADMIN_STATE_FORMATTING;
extern const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADING;
extern const struct admin_state_code *VDO_ADMIN_STATE_PRE_LOADED;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_RECOVERY;
extern const struct admin_state_code *VDO_ADMIN_STATE_LOADING_FOR_REBUILD;
extern const struct admin_state_code *VDO_ADMIN_STATE_WAITING_FOR_RECOVERY;
extern const struct admin_state_code *VDO_ADMIN_STATE_NEW;
extern const struct admin_state_code *VDO_ADMIN_STATE_INITIALIZED;
extern const struct admin_state_code *VDO_ADMIN_STATE_RECOVERING;
extern const struct admin_state_code *VDO_ADMIN_STATE_REBUILDING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVED;
extern const struct admin_state_code *VDO_ADMIN_STATE_SCRUBBING;
extern const struct admin_state_code *VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING;
extern const struct admin_state_code *VDO_ADMIN_STATE_STOPPING;
extern const struct admin_state_code *VDO_ADMIN_STATE_STOPPED;
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

const char * __must_check
vdo_get_admin_state_name(const struct admin_state *state);

/**
 * Get the current admin state code.
 *
 * @param state  The admin_state to query
 *
 * @return The current state
 **/
static inline const struct admin_state_code * __must_check
vdo_get_admin_state_code(const struct admin_state *state)
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
vdo_set_admin_state_code(struct admin_state *state,
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
static inline bool __must_check
vdo_is_state_normal(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->normal;
}

/**
 * Check whether an admin_state is suspending.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is suspending
 **/
static inline bool __must_check
vdo_is_state_suspending(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SUSPENDING);
}

/**
 * Check whether an admin_state is saving.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saving
 **/
static inline bool __must_check
vdo_is_state_saving(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SAVING);
}

/**
 * Check whether an admin_state is saved.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is saved
 **/
static inline bool __must_check
vdo_is_state_saved(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SAVED);
}

/**
 * Check whether an admin_state is draining.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is draining
 **/
static inline bool __must_check
vdo_is_state_draining(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->draining;
}

/**
 * Check whether an admin_state is loading.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is loading
 **/
static inline bool __must_check
vdo_is_state_loading(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->loading;
}

/**
 * Check whether an admin_state is resumeing.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is resumeing
 **/
static inline bool __must_check
vdo_is_state_resuming(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_RESUMING);
}

/**
 * Check whether an admin_state is doing a clean load.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> if the state is a clean load
 **/
static inline bool __must_check
vdo_is_state_clean_load(const struct admin_state *state)
{
	const struct admin_state_code *code = vdo_get_admin_state_code(state);

	return ((code == VDO_ADMIN_STATE_FORMATTING) ||
		(code == VDO_ADMIN_STATE_LOADING));
}

/**
 * Check whether an admin_state is quiescing.
 *
 * @param state  The admin_state to check
 *
 * @return <code>true</code> if the state is quiescing
 **/
static inline bool __must_check
vdo_is_state_quiescing(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->quiescing;
}

/**
 * Check whether an admin_state is quiescent.
 *
 * @param state  The admin_state to query
 *
 * @return <code>true</code> is the state is quiescent
 **/
static inline bool __must_check
vdo_is_state_quiescent(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->quiescent;
}

bool vdo_start_draining(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator * initiator);

bool vdo_finish_draining(struct admin_state *state);

bool vdo_finish_draining_with_result(struct admin_state *state, int result);

bool __must_check
vdo_assert_load_operation(const struct admin_state_code *operation,
			  struct vdo_completion *waiter);

bool vdo_start_loading(struct admin_state *state,
		       const struct admin_state_code *operation,
		       struct vdo_completion *waiter,
		       vdo_admin_initiator *initiator);

bool vdo_finish_loading(struct admin_state *state);

bool vdo_finish_loading_with_result(struct admin_state *state, int result);

bool vdo_start_resuming(struct admin_state *state,
			const struct admin_state_code *operation,
			struct vdo_completion *waiter,
			vdo_admin_initiator *initiator);

bool vdo_finish_resuming(struct admin_state *state);

bool vdo_finish_resuming_with_result(struct admin_state *state, int result);

int vdo_resume_if_quiescent(struct admin_state *state);

int vdo_start_operation(struct admin_state *state,
			const struct admin_state_code *operation);

bool vdo_start_operation_with_waiter(struct admin_state *state,
				     const struct admin_state_code *operation,
				     struct vdo_completion *waiter,
				     vdo_admin_initiator *initiator);

bool vdo_finish_operation(struct admin_state *state, int result);

/**
 * Set a result for the current operation.
 *
 * @param state  the admin_state
 * @param result the result to set; if there is no waiter, this is a no-op
 **/
static inline void vdo_set_operation_result(struct admin_state *state,
					    int result)
{
	if (state->waiter != NULL) {
		vdo_set_completion_result(state->waiter, result);
	}
}

#endif /* ADMIN_STATE_H */
