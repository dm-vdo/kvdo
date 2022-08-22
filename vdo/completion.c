// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "completion.h"

#include <linux/kernel.h>

#include "logger.h"
#include "permassert.h"

#include "kernel-types.h"
#include "status-codes.h"
#include "thread-config.h"
#include "vio.h"
#include "vdo.h"

static const char *VDO_COMPLETION_TYPE_NAMES[] = {
	/* Keep VDO_UNSET_COMPLETION_TYPE at the top. */
	"VDO_UNSET_COMPLETION_TYPE",

	/*
	 * Keep this block in sorted order. If you add or remove an
	 * entry, be sure to update the corresponding list in completion.h.
	 */
	"VDO_ACTION_COMPLETION",
	"VDO_ADMIN_COMPLETION",
	"VDO_BLOCK_ALLOCATOR_COMPLETION",
	"VDO_BLOCK_MAP_RECOVERY_COMPLETION",
	"VDO_DATA_VIO_POOL_COMPLETION",
	"VDO_FLUSH_COMPLETION",
	"VDO_FLUSH_NOTIFICATION_COMPLETION",
	"VDO_GENERATION_FLUSHED_COMPLETION",
	"VDO_HASH_ZONE_COMPLETION",
	"VDO_HASH_ZONES_COMPLETION",
	"VDO_LOCK_COUNTER_COMPLETION",
	"VDO_PAGE_COMPLETION",
	"VDO_PARTITION_COPY_COMPLETION",
	"VDO_READ_ONLY_MODE_COMPLETION",
	"VDO_READ_ONLY_REBUILD_COMPLETION",
	"VDO_RECOVERY_COMPLETION",
	"VDO_REFERENCE_COUNT_REBUILD_COMPLETION",
	"VDO_SLAB_SCRUBBER_COMPLETION",
	"VDO_SUB_TASK_COMPLETION",
	"VDO_SYNC_COMPLETION",
	"VIO_COMPLETION",

};

/**
 * vdo_initialize_completion() - Initialize a completion to a clean state,
 *                               for reused completions.
 * @completion: The completion to initialize.
 * @vdo: The VDO instance.
 * @type: The type of the completion.
 */
void vdo_initialize_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type)
{
	memset(completion, 0, sizeof(*completion));
	completion->vdo = vdo;
	completion->type = type;
	vdo_reset_completion(completion);
}

/**
 * vdo_reset_completion() - Reset a completion to a clean state, while
 *                          keeping the type, vdo and parent information.
 * @completion: The completion to reset.
 */
void vdo_reset_completion(struct vdo_completion *completion)
{
	completion->result = VDO_SUCCESS;
	completion->complete = false;
}

/**
 * assert_incomplete() - Assert that a completion is not complete.
 * @completion: The completion to check.
 */
static inline void assert_incomplete(struct vdo_completion *completion)
{
	ASSERT_LOG_ONLY(!completion->complete, "completion is not complete");
}

/**
 * vdo_set_completion_result() - Set the result of a completion.
 * @completion: The completion whose result is to be set.
 * @result: The result to set.
 *
 * Older errors will not be masked.
 */
void vdo_set_completion_result(struct vdo_completion *completion, int result)
{
	assert_incomplete(completion);
	if (completion->result == VDO_SUCCESS) {
		completion->result = result;
	}
}

/**
 * vdo_invoke_completion_callback_with_priority() - Invoke the callback of
 *                                                  a completion.
 * @completion: The completion whose callback is to be invoked.
 * @priority: The priority at which to enqueue the completion.
 *
 * If called on the correct thread (i.e. the one specified in the
 * completion's callback_thread_id field), the completion will be run
 * immediately. Otherwise, the completion will be enqueued on the
 * correct callback thread.
 */
void
vdo_invoke_completion_callback_with_priority(struct vdo_completion *completion,
					     enum vdo_completion_priority priority)
{
	thread_id_t callback_thread = completion->callback_thread_id;

	if (completion->requeue ||
	    (callback_thread != vdo_get_callback_thread_id())) {
		vdo_enqueue_completion_with_priority(completion, priority);
		return;
	}

	vdo_run_completion_callback(completion);
}

/**
 * vdo_continue_completion() - Continue processing a completion.
 * @completion: The completion to continue.
 * @result: The current result (will not mask older errors).
 *
 * Continue processing a completion by setting the current result and calling
 * vdo_invoke_completion_callback().
 */
void vdo_continue_completion(struct vdo_completion *completion, int result)
{
	vdo_set_completion_result(completion, result);
	vdo_invoke_completion_callback(completion);
}

/**
 * vdo_complete_completion() - Complete a completion.
 *
 * @completion: The completion to complete.
 */
void vdo_complete_completion(struct vdo_completion *completion)
{
	assert_incomplete(completion);
	completion->complete = true;
	if (completion->callback != NULL) {
		vdo_invoke_completion_callback(completion);
	}
}

/**
 * vdo_finish_completion_parent_callback() - A callback to finish the parent
 *                                           of a completion.
 * @completion: The completion which has finished and whose parent should
 *              be finished.
 */
void vdo_finish_completion_parent_callback(struct vdo_completion *completion)
{
	vdo_finish_completion((struct vdo_completion *) completion->parent,
			      completion->result);
}

/**
 * vdo_preserve_completion_error_and_continue() - Error handler.
 * @completion: The completion which failed.
 *
 * Error handler which preserves an error in the parent (if there is
 * one), and then resets the failing completion and calls its
 * non-error callback.
 */
void
vdo_preserve_completion_error_and_continue(struct vdo_completion *completion)
{
	if (completion->parent != NULL) {
		vdo_set_completion_result(completion->parent, completion->result);
	}

	vdo_reset_completion(completion);
	vdo_invoke_completion_callback(completion);
}

/**
 * get_completion_type_name() - Return the name of a completion type.
 * @completion_type: The completion type.
 *
 * Return: a pointer to a static string; if the completion_type is unknown
 *         this is to a static buffer that may be overwritten.
 */
static const char *
get_completion_type_name(enum vdo_completion_type completion_type)
{
	/*
	 * Try to catch failures to update the array when the enum values
	 * change.
	 */
	STATIC_ASSERT(ARRAY_SIZE(VDO_COMPLETION_TYPE_NAMES) ==
		      (VDO_MAX_COMPLETION_TYPE - VDO_UNSET_COMPLETION_TYPE));

	if (completion_type >= VDO_MAX_COMPLETION_TYPE) {
		static char numeric[100];

		snprintf(numeric,
			 99,
			 "%d (%#x)",
			 completion_type,
			 completion_type);
		return numeric;
	}

	return VDO_COMPLETION_TYPE_NAMES[completion_type];
}

/**
 * vdo_noop_completion_callback() - A callback which does nothing.
 * @completion: The completion being called back.
 *
 *  This callback is intended to be set as an error handler in the
 * case where an error should do nothing.
 */
void
vdo_noop_completion_callback(struct vdo_completion *completion __always_unused)
{
}

/**
 * vdo_assert_completion_type() - Assert that a completion is of the correct
 *                                type.
 * @actual: The actual completion type.
 * @expected: The expected completion type.
 *
 * Return: VDO_SUCCESS or VDO_PARAMETER_MISMATCH
 */
int vdo_assert_completion_type(enum vdo_completion_type actual,
			       enum vdo_completion_type expected)
{
	return ASSERT((expected == actual),
		      "completion type is %s instead of %s",
		      get_completion_type_name(actual),
		      get_completion_type_name(expected));
}

/**
 * vdo_enqueue_completion_with_priority() - Enqueue a completion.
 * @completion: The completion to be enqueued.
 * @priority: The priority at which the work should be done.
 *
 * A function to enqueue a vdo_completion to run on the thread
 * specified by its callback_thread_id field at the specified
 * priority.
 */
void vdo_enqueue_completion_with_priority(struct vdo_completion *completion,
					  enum vdo_completion_priority priority)
{
	struct vdo *vdo = completion->vdo;
	thread_id_t thread_id = completion->callback_thread_id;

	if (ASSERT(thread_id < vdo->thread_config->thread_count,
		   "thread_id %u (completion type %d) is less than thread count %u",
		   thread_id,
		   completion->type,
		   vdo->thread_config->thread_count) != UDS_SUCCESS) {
		BUG();
	}

	completion->requeue = false;
	completion->priority = priority;
	completion->my_queue = NULL;
	enqueue_work_queue(vdo->threads[thread_id].queue, completion);
}

