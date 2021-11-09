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

#include "admin-completion.h"

#include <linux/atomic.h>
#include <linux/delay.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

/**
 * Check that an admin_completion's type is as expected.
 *
 * @param completion  The admin_completion to check
 * @param expected    The expected type
 **/
void assert_vdo_admin_operation_type(struct admin_completion *completion,
				     enum admin_operation_type expected)
{
	ASSERT_LOG_ONLY(completion->type == expected,
			"admin operation type is %u instead of %u",
			completion->type,
			expected);
}

/**
 * Convert the sub-task completion of an admin_completion to an
 * admin_completion.
 *
 * @param completion  the admin_completion's sub-task completion
 *
 * @return The sub-task completion as its enclosing admin_completion
 **/
struct admin_completion *
vdo_admin_completion_from_sub_task(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;

	assert_vdo_completion_type(completion->type, VDO_SUB_TASK_COMPLETION);
	assert_vdo_completion_type(parent->type, VDO_ADMIN_COMPLETION);
	return container_of(parent, struct admin_completion, completion);
}

/**
 * Assert that we are operating on the correct thread for the current phase.
 *
 * @param admin_completion  The admin_completion to check
 * @param what              The method doing the phase check
 * @param phase_names       The names of the phases of the current operation
 **/
void assert_vdo_admin_phase_thread(struct admin_completion *admin_completion,
				   const char *what,
				   const char *phase_names[])
{
	thread_id_t expected =
		admin_completion->get_thread_id(admin_completion);
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == expected),
			"%s on correct thread for %s",
			what,
			phase_names[admin_completion->phase]);
}

/**
 * Get the vdo from the sub-task completion of its admin_completion.
 *
 * @param completion  the sub-task completion
 * @param expected    the expected operation type of the admin_completion
 *
 * @return The vdo
 **/
struct vdo *vdo_from_admin_sub_task(struct vdo_completion *completion,
				    enum admin_operation_type expected)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	assert_vdo_admin_operation_type(admin_completion, expected);
	return admin_completion->vdo;
}

/**
 * Initialize an admin completion.
 *
 * @param vdo                The vdo which owns the completion
 * @param admin_completion   The admin_completion to initialize
 **/
void initialize_vdo_admin_completion(struct vdo *vdo,
				     struct admin_completion *admin_completion)
{
	admin_completion->vdo = vdo;
	initialize_vdo_completion(&admin_completion->completion, vdo,
				  VDO_ADMIN_COMPLETION);
	initialize_vdo_completion(&admin_completion->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);
	init_completion(&admin_completion->callback_sync);
	atomic_set(&admin_completion->busy, 0);
}

/**
 * Reset an admin_completion's sub-task completion.
 *
 * @param completion  The admin_completion's sub-task completion
 *
 * @return The sub-task completion for the convenience of callers
 **/
struct vdo_completion *
reset_vdo_admin_sub_task(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	reset_vdo_completion(completion);
	completion->callback_thread_id =
		admin_completion->get_thread_id(admin_completion);
	completion->requeue = true;
	return completion;
}

/**
 * Prepare the sub-task completion of a vdo's admin_completion
 *
 * @param vdo            The vdo
 * @param callback       The callback for the sub-task
 * @param error_handler  The error handler for the sub-task
 * @param thread_id      The ID of the thread on which to run the callback
 **/
static void prepare_vdo_admin_sub_task_on_thread(struct vdo *vdo,
						 vdo_action *callback,
						 vdo_action *error_handler,
						 thread_id_t thread_id)
{
	vdo_prepare_completion_for_requeue(&vdo->admin_completion.sub_task_completion,
					   callback,
					   error_handler,
					   thread_id,
					   &vdo->admin_completion.completion);
}

/**
 * Prepare the sub-task completion of a vdo's admin_completion to run on the
 * same thread as the admin_completion's main completion.
 *
 * @param vdo            The vdo
 * @param callback       The callback for the sub-task
 * @param error_handler  The error handler for the sub-task
 **/
void prepare_vdo_admin_sub_task(struct vdo *vdo,
				vdo_action *callback,
				vdo_action *error_handler)
{
	struct admin_completion *admin_completion = &vdo->admin_completion;

	prepare_vdo_admin_sub_task_on_thread(vdo,
					     callback, error_handler,
					     admin_completion->completion.callback_thread_id);
}

/**
 * Callback for admin operations which will notify the layer that the operation
 * is complete.
 *
 * @param vdo_completion  The vdo_completion within the admin completion
 **/
static void admin_operation_callback(struct vdo_completion *vdo_completion)
{
	struct admin_completion *completion;

	assert_vdo_completion_type(vdo_completion->type, VDO_ADMIN_COMPLETION);
	completion = container_of(vdo_completion,
				  struct admin_completion,
				  completion);
	complete(&completion->callback_sync);
}

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
int
perform_vdo_admin_operation(struct vdo *vdo,
			    enum admin_operation_type type,
			    vdo_thread_id_getter_for_phase *thread_id_getter,
			    vdo_action *action,
			    vdo_action *error_handler)
{
	int result;
	struct admin_completion *admin_completion = &vdo->admin_completion;

	if (atomic_cmpxchg(&admin_completion->busy, 0, 1) != 0) {
		return uds_log_error_strerror(VDO_COMPONENT_BUSY,
					      "Can't start admin operation of type %u, another operation is already in progress",
					      type);
	}

	vdo_prepare_completion(&admin_completion->completion,
			       admin_operation_callback,
			       admin_operation_callback,
			       vdo->thread_config->admin_thread,
			       NULL);
	admin_completion->type = type;
	admin_completion->get_thread_id = thread_id_getter;
	admin_completion->phase = 0;
	prepare_vdo_admin_sub_task(vdo, action, error_handler);
	reinit_completion(&admin_completion->callback_sync);
	vdo_enqueue_completion(&admin_completion->sub_task_completion);

	/*
	 * Using the "interruptible" interface means that Linux will not log a 
	 * message when we wait for more than 120 seconds. 
	 */
	while (wait_for_completion_interruptible(&admin_completion->callback_sync) != 0) {
		/*
		 * However, if we get a signal in a user-mode process, we could 
		 * spin... 
		 */
		fsleep(1000);
	}

	result = admin_completion->completion.result;
	smp_wmb();
	atomic_set(&admin_completion->busy, 0);
	return result;
}
