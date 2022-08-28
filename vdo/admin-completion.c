// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "admin-completion.h"

#include <linux/atomic.h>
#include <linux/delay.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "completion.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

void vdo_assert_admin_operation_type(struct admin_completion *completion,
				     enum admin_operation_type expected)
{
	ASSERT_LOG_ONLY(completion->type == expected,
			"admin operation type is %u instead of %u",
			completion->type,
			expected);
}

struct admin_completion *
vdo_admin_completion_from_sub_task(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;

	vdo_assert_completion_type(completion->type, VDO_SUB_TASK_COMPLETION);
	vdo_assert_completion_type(parent->type, VDO_ADMIN_COMPLETION);
	return container_of(parent, struct admin_completion, completion);
}

/*
 * Assert that we are operating on the correct thread for the current phase.
 */
void vdo_assert_admin_phase_thread(struct admin_completion *admin_completion,
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

struct vdo *vdo_from_admin_sub_task(struct vdo_completion *completion,
				    enum admin_operation_type expected)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	vdo_assert_admin_operation_type(admin_completion, expected);
	return admin_completion->vdo;
}

void vdo_initialize_admin_completion(struct vdo *vdo,
				     struct admin_completion *admin_completion)
{
	admin_completion->vdo = vdo;
	vdo_initialize_completion(&admin_completion->completion, vdo,
				  VDO_ADMIN_COMPLETION);
	vdo_initialize_completion(&admin_completion->sub_task_completion, vdo,
				  VDO_SUB_TASK_COMPLETION);
	init_completion(&admin_completion->callback_sync);
	atomic_set(&admin_completion->busy, 0);
}

struct vdo_completion *
vdo_reset_admin_sub_task(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		vdo_admin_completion_from_sub_task(completion);
	vdo_reset_completion(completion);
	completion->callback_thread_id =
		admin_completion->get_thread_id(admin_completion);
	completion->requeue = true;
	return completion;
}

static void vdo_prepare_admin_sub_task_on_thread(struct vdo *vdo,
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

/*
 * Prepare the sub-task completion to run on the same thread as its enclosing
 * completion.
 */
void vdo_prepare_admin_sub_task(struct vdo *vdo,
				vdo_action *callback,
				vdo_action *error_handler)
{
	struct admin_completion *admin_completion = &vdo->admin_completion;

	vdo_prepare_admin_sub_task_on_thread(vdo,
					     callback, error_handler,
					     admin_completion->completion.callback_thread_id);
}

static void admin_operation_callback(struct vdo_completion *vdo_completion)
{
	struct admin_completion *completion;

	vdo_assert_completion_type(vdo_completion->type, VDO_ADMIN_COMPLETION);
	completion = container_of(vdo_completion,
				  struct admin_completion,
				  completion);
	complete(&completion->callback_sync);
}

/*
 * Perform an administrative operation (load, suspend, grow logical, or grow
 * physical). This method should not be called from base threads unless it is
 * certain the calling thread won't be needed to perform the operation. It may
 * (and should) be called from non-base threads.
 *
 * FIXME: does this base thread note apply anymore?
 */
int
vdo_perform_admin_operation(struct vdo *vdo,
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
	vdo_prepare_admin_sub_task(vdo, action, error_handler);
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
