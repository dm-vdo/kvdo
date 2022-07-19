/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "completion.h"
#include "types.h"

struct admin_state_code {
	const char *name;
	/* Normal operation, data_vios may be active */
	bool normal;
	/* I/O is draining, new requests should not start */
	bool draining;
	/* This is a startup time operation */
	bool loading;
	/* The next state will be quiescent */
	bool quiescing;
	/* The VDO is quiescent, there should be no I/O */
	bool quiescent;
	/*
	 * Whether an operation is in progress and so no other operation may be
	 * started
	 */
	bool operating;
};

/*
 * The state codes.
 */
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
	/* The current administrative state */
	const struct admin_state_code *current_state;
	/*
	 * The next administrative state (when the current operation finishes)
	 */
	const struct admin_state_code *next_state;
	/* A completion waiting on a state change */
	struct vdo_completion *waiter;
	/* Whether an operation is being initiated */
	bool starting;
	/* Whether an operation has completed in the initiator */
	bool complete;
};

/**
 * typedef vdo_admin_initiator - A method to be called once an admin operation
 *                               may be initiated.
 */
typedef void vdo_admin_initiator(struct admin_state *state);

/**
 * vdo_get_admin_state_code() - Get the current admin state code.
 * @state: The admin_state to query.
 *
 * Return: The current state.
 */
static inline const struct admin_state_code * __must_check
vdo_get_admin_state_code(const struct admin_state *state)
{
	return READ_ONCE(state->current_state);
}

/**
 * vdo_set_admin_state_code() - Set the current admin state code.
 * @state: The admin_state to modify.
 * @code: The code to set.
 *
 * This function should be used primarily for initialization and by adminState
 * internals. Most uses should go through the operation interfaces.
 */
static inline void
vdo_set_admin_state_code(struct admin_state *state,
			 const struct admin_state_code *code)
{
	WRITE_ONCE(state->current_state, code);
}

/**
 * vdo_is_state_normal() - Check whether an admin_state is in normal
 *                         operation.
 * @state: The admin_state to query.
 *
 * Return: true if the state is normal.
 */
static inline bool __must_check
vdo_is_state_normal(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->normal;
}

/**
 * vdo_is_state_suspending() - Check whether an admin_state is suspending.
 * @state: The admin_state to query.
 *
 * Return: true if the state is suspending.
 */
static inline bool __must_check
vdo_is_state_suspending(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SUSPENDING);
}

/**
 * vdo_is_state_saving() - Check whether an admin_state is saving.
 * @state: The admin_state to query.
 *
 * Return: true if the state is saving.
 */
static inline bool __must_check
vdo_is_state_saving(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SAVING);
}

/**
 * vdo_is_state_saved() - Check whether an admin_state is saved.
 * @state: The admin_state to query.
 *
 * Return: true if the state is saved.
 */
static inline bool __must_check
vdo_is_state_saved(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_SAVED);
}

/**
 * vdo_is_state_draining() - Check whether an admin_state is draining.
 * @state: The admin_state to query.
 *
 * Return: true if the state is draining.
 */
static inline bool __must_check
vdo_is_state_draining(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->draining;
}

/**
 * vdo_is_state_loading() - Check whether an admin_state is loading.
 * @state: The admin_state to query.
 *
 * Return: true if the state is loading.
 */
static inline bool __must_check
vdo_is_state_loading(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->loading;
}

/**
 * vdo_is_state_resuming() - Check whether an admin_state is resuming.
 * @state: The admin_state to query.
 *
 * Return: true if the state is resuming.
 */
static inline bool __must_check
vdo_is_state_resuming(const struct admin_state *state)
{
	return (vdo_get_admin_state_code(state) == VDO_ADMIN_STATE_RESUMING);
}

/**
 * vdo_is_state_clean_load() - Check whether an admin_state is doing a clean
 *                             load.
 * @state: The admin_state to query.
 *
 * Return: true if the state is a clean load.
 */
static inline bool __must_check
vdo_is_state_clean_load(const struct admin_state *state)
{
	const struct admin_state_code *code = vdo_get_admin_state_code(state);

	return ((code == VDO_ADMIN_STATE_FORMATTING) ||
		(code == VDO_ADMIN_STATE_LOADING));
}

/**
 * vdo_is_state_quiescing() - Check whether an admin_state is quiescing.
 * @state: The admin_state to check.
 *
 * Return: true if the state is quiescing.
 */
static inline bool __must_check
vdo_is_state_quiescing(const struct admin_state *state)
{
	return vdo_get_admin_state_code(state)->quiescing;
}

/**
 * vdo_is_state_quiescent() - Check whether an admin_state is quiescent.
 * @state: The admin_state to query.
 *
 * Return: true is the state is quiescent.
 */
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
 * vdo_set_operation_result() - Set a result for the current operation.
 * @state: the admin_state.
 * @result: the result to set; if there is no waiter, this is a no-op.
 */
static inline void vdo_set_operation_result(struct admin_state *state,
					    int result)
{
	if (state->waiter != NULL) {
		vdo_set_completion_result(state->waiter, result);
	}
}

#endif /* ADMIN_STATE_H */
