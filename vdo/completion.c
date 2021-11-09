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
	"VDO_EXTENT_COMPLETION",
	"VDO_FLUSH_COMPLETION",
	"VDO_FLUSH_NOTIFICATION_COMPLETION",
	"VDO_GENERATION_FLUSHED_COMPLETION",
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
 * Initialize a completion to a clean state, for reused completions.
 *
 * @param completion The completion to initialize
 * @param vdo        The VDO instance
 * @param type       The type of the completion
 **/
void initialize_vdo_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type)
{
	memset(completion, 0, sizeof(*completion));
	completion->vdo = vdo;
	completion->type = type;
	reset_vdo_completion(completion);
}

/**
 * Reset a completion to a clean state, while keeping
 * the type, vdo and parent information.
 *
 * @param completion the completion to reset
 **/
void reset_vdo_completion(struct vdo_completion *completion)
{
	completion->result = VDO_SUCCESS;
	completion->complete = false;
}

/**
 * Assert that a completion is not complete.
 *
 * @param completion The completion to check
 **/
static inline void assert_incomplete(struct vdo_completion *completion)
{
	ASSERT_LOG_ONLY(!completion->complete, "completion is not complete");
}

/**
 * Set the result of a completion. Older errors will not be masked.
 *
 * @param completion The completion whose result is to be set
 * @param result     The result to set
 **/
void set_vdo_completion_result(struct vdo_completion *completion, int result)
{
	assert_incomplete(completion);
	if (completion->result == VDO_SUCCESS) {
		completion->result = result;
	}
}

/**
 * Check whether a completion's callback must be enqueued, or if it can be run
 * on the current thread. Side effect: clears the requeue flag if it is set,
 * so the caller MUST requeue if this returns true.
 *
 * @param completion  The completion whose callback is to be invoked
 *
 * @return <code>false</code> if the callback must be run on this thread
 *         <code>true</code>  if the callback must be enqueued
 **/
static inline bool __must_check
requires_enqueue(struct vdo_completion *completion)
{
	thread_id_t callback_thread = completion->callback_thread_id;

	if (completion->requeue) {
		completion->requeue = false;
		return true;
	}

	return (callback_thread != vdo_get_callback_thread_id());
}

/**
 * Invoke the callback of a completion. If called on the correct thread (i.e.
 * the one specified in the completion's callback_thread_id field), the
 * completion will be run immediately. Otherwise, the completion will be
 * enqueued on the correct callback thread.
 *
 * @param completion  The completion whose callback is to be invoked
 * @param priority    The priority at which to enqueue the completion
 **/
void
vdo_invoke_completion_callback_with_priority(struct vdo_completion *completion,
					     enum vdo_work_item_priority priority)
{
	if (requires_enqueue(completion)) {
		vdo_enqueue_completion_with_priority(completion, priority);
		return;
	}

	vdo_run_completion_callback(completion);
}

/**
 * Continue processing a completion by setting the current result and calling
 * vdo_invoke_completion_callback().
 *
 * @param completion  The completion to continue
 * @param result      The current result (will not mask older errors)
 **/
void continue_vdo_completion(struct vdo_completion *completion, int result)
{
	set_vdo_completion_result(completion, result);
	vdo_invoke_completion_callback(completion);
}

/**
 * Complete a completion.
 *
 * @param completion  The completion to complete
 **/
void complete_vdo_completion(struct vdo_completion *completion)
{
	assert_incomplete(completion);
	completion->complete = true;
	if (completion->callback != NULL) {
		vdo_invoke_completion_callback(completion);
	}
}

/**
 * A callback to finish the parent of a completion.
 *
 * @param completion  The completion which has finished and whose parent should
 *                    be finished
 **/
void vdo_finish_completion_parent_callback(struct vdo_completion *completion)
{
	vdo_finish_completion((struct vdo_completion *) completion->parent,
			      completion->result);
}

/**
 * Error handler which preserves an error in the parent (if there is one),
 * and then resets the failing completion and calls its non-error callback.
 *
 * @param completion  The completion which failed
 **/
void
preserve_vdo_completion_error_and_continue(struct vdo_completion *completion)
{
	if (completion->parent != NULL) {
		set_vdo_completion_result(completion->parent, completion->result);
	}

	reset_vdo_completion(completion);
	vdo_invoke_completion_callback(completion);
}

/**
 * Return the name of a completion type.
 *
 * @param completion_type  the completion type
 *
 * @return a pointer to a static string; if the completion_type is unknown
 *         this is to a static buffer that may be overwritten.
 **/
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
 * Assert that a completion is of the correct type
 *
 * @param actual    The actual completion type
 * @param expected  The expected completion type
 *
 * @return          VDO_SUCCESS or VDO_PARAMETER_MISMATCH
 **/
int assert_vdo_completion_type(enum vdo_completion_type actual,
			       enum vdo_completion_type expected)
{
	return ASSERT((expected == actual),
		      "completion type is %s instead of %s",
		      get_completion_type_name(actual),
		      get_completion_type_name(expected));
}

static void vdo_enqueued_work(struct vdo_work_item *work_item)
{
	vdo_run_completion_callback(container_of(work_item,
				    struct vdo_completion,
				    work_item));
}

/**
 * A function to enqueue a vdo_completion to run on the thread specified by its
 * callback_thread_id field at the specified priority.
 *
 * @param completion  The completion to be enqueued
 * @param priority    The priority at which the work should be done
 **/
void vdo_enqueue_completion_with_priority(struct vdo_completion *completion,
					  enum vdo_work_item_priority priority)
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

	setup_work_item(&completion->work_item,
			vdo_enqueued_work,
			priority);
	enqueue_work_queue(vdo->threads[thread_id].queue,
			   &completion->work_item);
}

/* Preparing work items */

/**********************************************************************/
void setup_work_item(struct vdo_work_item *item,
		     vdo_work_function work,
		     enum vdo_work_item_priority priority)
{
	ASSERT_LOG_ONLY(item->my_queue == NULL,
			"setup_work_item not called on enqueued work item");
	item->work = work;
	item->stat_table_index = 0;
	item->priority = priority;
	item->my_queue = NULL;
}

