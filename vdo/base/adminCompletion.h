/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.h#10 $
 */

#ifndef ADMIN_COMPLETION_H
#define ADMIN_COMPLETION_H

#include "atomic.h"
#include "completion.h"
#include "types.h"

typedef enum adminOperationType {
	ADMIN_OPERATION_UNKNOWN = 0,
	ADMIN_OPERATION_GROW_LOGICAL,
	ADMIN_OPERATION_GROW_PHYSICAL,
	ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
	ADMIN_OPERATION_LOAD,
	ADMIN_OPERATION_RESUME,
	ADMIN_OPERATION_SAVE,
	ADMIN_OPERATION_SUSPEND,
} AdminOperationType;

struct admin_completion;

/**
 * A function which gets the ID of the thread on which the current phase of an
 * admin operation should be run.
 *
 * @param admin_completion The admin_completion
 *
 * @return The ID of the thread on which the current phase should be performed
 **/
typedef ThreadID
ThreadIDGetterForPhase(struct admin_completion *admin_completion);

struct admin_completion {
	// XXX should be replaced by container_of() when enqueuables go away
	// and this becomes a field of struct vdo.
	struct vdo *vdo;
	/** The completion */
	struct vdo_completion completion;
	/** The sub-task completion */
	struct vdo_completion sub_task_completion;
	/** Whether this completion is in use */
	AtomicBool busy;
	/** The operation type */
	AdminOperationType type;
	/** Method to get the ThreadID for the current phase */
	ThreadIDGetterForPhase *get_thread_id;
	/** The current phase of the operation */
	uint32_t phase;
};

/**
 * Check that an admin_completion's type is as expected.
 *
 * @param completion  The admin_completion to check
 * @param expected    The expected type
 **/
void assert_admin_operation_type(struct admin_completion *completion,
				 AdminOperationType expected);

/**
 * Convert the sub-task completion of an admin_completion to an
 * admin_completion.
 *
 * @param completion  the admin_completion's sub-task completion
 *
 * @return The sub-task completion as its enclosing admin_completion
 **/
struct admin_completion *
admin_completion_from_sub_task(struct vdo_completion *completion)
	__attribute__((warn_unused_result));

/**
 * Assert that we are operating on the correct thread for the current phase.
 *
 * @param admin_completion  The admin_completion to check
 * @param what              The method doing the phase check
 * @param phase_names       The names of the phases of the current operation
 **/
void assert_admin_phase_thread(struct admin_completion *admin_completion,
			       const char *what,
			       const char *phase_names[]);

/**
 * Get the vdo from the sub-task completion of its admin_completion.
 *
 * @param completion  the sub-task completion
 * @param expected    the expected operation type of the admin_completion
 *
 * @return The vdo
 **/
struct vdo *vdo_from_admin_sub_task(struct vdo_completion *completion,
				    AdminOperationType expected)
	__attribute__((warn_unused_result));

/**
 * Initialize an admin completion.
 *
 * @param vdo                The vdo which owns the completion
 * @param admin_completion   The admin_completion to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
int initialize_admin_completion(struct vdo *vdo,
				struct admin_completion *admin_completion)
	__attribute__((warn_unused_result));

/**
 * Clean up an admin completion's resources.
 *
 * @param admin_completion  The admin_completion to uninitialize
 **/
void uninitialize_admin_completion(struct admin_completion *admin_completion);

/**
 * Reset an admin_completion's sub-task completion.
 *
 * @param completion  The admin_completion's sub-task completion
 *
 * @return The sub-task completion for the convenience of callers
 **/
struct vdo_completion *reset_admin_sub_task(struct vdo_completion *completion);

/**
 * Prepare the sub-task completion of a vdo's admin_completion
 *
 * @param vdo            The vdo
 * @param callback       The callback for the sub-task
 * @param error_handler  The error handler for the sub-task
 * @param thread_id      The ID of the thread on which to run the callback
 **/
void prepare_admin_sub_task_on_thread(struct vdo *vdo,
				      vdo_action *callback,
				      vdo_action *error_handler,
				      ThreadID thread_id);

/**
 * Prepare the sub-task completion of a vdo's admin_completion to run on the
 * same thread as the admin_completion's main completion.
 *
 * @param vdo            The vdo
 * @param callback       The callback for the sub-task
 * @param error_handler  The error handler for the sub-task
 **/
void prepare_admin_sub_task(struct vdo *vdo,
			    vdo_action *callback,
			    vdo_action *error_handler);

/**
 * Perform an administrative operation (load, suspend, grow logical, or grow
 * physical). This method should not be called from base threads unless it is
 * certain the calling thread won't be needed to perform the operation. It may
 * (and should) be called from non-base threads.
 *
 * @param vdo               The vdo on which to perform the operation
 * @param type              The type of operation to perform
 * @param thread_id_getter  A function for getting the ID of the thread on
 *                          which a given phase should be run
 * @param action            The action which starts the operation
 * @param error_handler     The error handler for the operation
 *
 * @return The result of the operation
 **/
int perform_admin_operation(struct vdo *vdo,
			    AdminOperationType type,
			    ThreadIDGetterForPhase *thread_id_getter,
			    vdo_action *action,
			    vdo_action *error_handler)
	__attribute__((warn_unused_result));

#endif /* ADMIN_COMPLETION_H */
