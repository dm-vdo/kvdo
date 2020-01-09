/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminCompletion.c#8 $
 */

#include "adminCompletion.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "atomic.h"
#include "completion.h"
#include "types.h"
#include "vdoInternal.h"

/**********************************************************************/
void assert_admin_operation_type(struct admin_completion *completion,
				 AdminOperationType expected)
{
	ASSERT_LOG_ONLY(completion->type == expected,
			"admin operation type is %u instead of %u",
			completion->type,
			expected);
}

/**********************************************************************/
struct admin_completion *
admin_completion_from_sub_task(struct vdo_completion *completion)
{
	STATIC_ASSERT(offsetof(struct admin_completion, completion) == 0);
	assertCompletionType(completion->type, SUB_TASK_COMPLETION);
	struct vdo_completion *parent = completion->parent;
	assertCompletionType(parent->type, ADMIN_COMPLETION);
	return (struct admin_completion *)parent;
}

/**********************************************************************/
void assert_admin_phase_thread(struct admin_completion *admin_completion,
			       const char *what,
			       const char *phase_names[])
{
	ThreadID expected = admin_completion->get_thread_id(admin_completion);
	ASSERT_LOG_ONLY((getCallbackThreadID() == expected),
			"%s on correct thread for %s",
			what,
			phase_names[admin_completion->phase]);
}

/**********************************************************************/
struct vdo *vdo_from_admin_sub_task(struct vdo_completion *completion,
				    AdminOperationType expected)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	assert_admin_operation_type(admin_completion, expected);
	return admin_completion->completion.parent;
}

/**********************************************************************/
int initialize_admin_completion(struct vdo *vdo,
				struct admin_completion *admin_completion)
{
	int result = initializeEnqueueableCompletion(&admin_completion->completion,
						     ADMIN_COMPLETION,
						     vdo->layer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result =
		initializeEnqueueableCompletion(&admin_completion->sub_task_completion,
		SUB_TASK_COMPLETION,
		vdo->layer);
	if (result != VDO_SUCCESS) {
		uninitialize_admin_completion(admin_completion);
		return result;
	}

	atomicStoreBool(&admin_completion->busy, false);

	return VDO_SUCCESS;
}

/**********************************************************************/
void uninitialize_admin_completion(struct admin_completion *admin_completion)
{
	destroyEnqueueable(&admin_completion->sub_task_completion);
	destroyEnqueueable(&admin_completion->completion);
}

/**********************************************************************/
struct vdo_completion *reset_admin_sub_task(struct vdo_completion *completion)
{
	struct admin_completion *admin_completion =
		admin_completion_from_sub_task(completion);
	resetCompletion(completion);
	completion->callbackThreadID =
		admin_completion->get_thread_id(admin_completion);
	return completion;
}

/**********************************************************************/
void prepare_admin_sub_task_on_thread(struct vdo *vdo,
				      VDOAction *callback,
				      VDOAction *error_handler,
				      ThreadID thread_id)
{
	prepareForRequeue(&vdo->adminCompletion.sub_task_completion,
			  callback,
			  error_handler,
			  thread_id,
			  &vdo->adminCompletion);
}

/**********************************************************************/
void prepare_admin_sub_task(struct vdo *vdo,
			    VDOAction *callback,
			    VDOAction *error_handler)
{
	struct admin_completion *admin_completion = &vdo->adminCompletion;
	prepare_admin_sub_task_on_thread(vdo,
					 callback,
					 error_handler,
					 admin_completion->completion.callbackThreadID);
}

/**
 * Callback for admin operations which will notify the layer that the operation
 * is complete.
 *
 * @param completion  The admin completion
 **/
static void admin_operation_callback(struct vdo_completion *completion)
{
	completion->layer->completeAdminOperation(completion->layer);
}

/**********************************************************************/
int perform_admin_operation(struct vdo *vdo,
			    AdminOperationType type,
			    ThreadIDGetterForPhase *thread_id_getter,
			    VDOAction *action,
			    VDOAction *error_handler)
{
	struct admin_completion *admin_completion = &vdo->adminCompletion;
	if (!compareAndSwapBool(&admin_completion->busy, false, true)) {
		return logErrorWithStringError(VDO_COMPONENT_BUSY,
					       "Can't start admin operation of type %u, "
					       "another operation is already in progress",
					       type);
	}

	prepareCompletion(&admin_completion->completion,
			  admin_operation_callback,
			  admin_operation_callback,
			  getAdminThread(getThreadConfig(vdo)),
			  vdo);
	admin_completion->type = type;
	admin_completion->get_thread_id = thread_id_getter;
	admin_completion->phase = 0;
	prepare_admin_sub_task(vdo, action, error_handler);

	PhysicalLayer *layer = vdo->layer;
	layer->enqueue(admin_completion->sub_task_completion.enqueueable);
	layer->waitForAdminOperation(layer);
	int result = admin_completion->completion.result;
	atomicStoreBool(&admin_completion->busy, false);
	return result;
}
