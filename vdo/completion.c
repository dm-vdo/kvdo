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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/completion.c#26 $
 */

#include "completion.h"

#include "logger.h"
#include "statusCodes.h"

static const char *VDO_COMPLETION_TYPE_NAMES[] = {
	// Keep UNSET_COMPLETION_TYPE at the top.
	"UNSET_COMPLETION_TYPE",

	// Keep this block in sorted order. If you add or remove an
	// entry, be sure to update the corresponding list in completion.h.
	"ACTION_COMPLETION",
	"ADMIN_COMPLETION",
	"BLOCK_ALLOCATOR_COMPLETION",
	"BLOCK_MAP_RECOVERY_COMPLETION",
	"FLUSH_NOTIFICATION_COMPLETION",
	"GENERATION_FLUSHED_COMPLETION",
	"LOCK_COUNTER_COMPLETION",
	"PARTITION_COPY_COMPLETION",
	"READ_ONLY_MODE_COMPLETION",
	"READ_ONLY_REBUILD_COMPLETION",
	"RECOVERY_COMPLETION",
	"REFERENCE_COUNT_REBUILD_COMPLETION",
	"SLAB_SCRUBBER_COMPLETION",
	"SUB_TASK_COMPLETION",
	"SYNC_COMPLETION",
	"VDO_EXTENT_COMPLETION",
	"VDO_PAGE_COMPLETION",
	"VIO_COMPLETION",

};

/**********************************************************************/
void initialize_completion(struct vdo_completion *completion,
			   enum vdo_completion_type type,
			   PhysicalLayer *layer)
{
	memset(completion, 0, sizeof(*completion));
	completion->layer = layer;
	completion->type = type;
	reset_completion(completion);
}

/**********************************************************************/
void reset_completion(struct vdo_completion *completion)
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

/**********************************************************************/
void set_completion_result(struct vdo_completion *completion, int result)
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

	return (callback_thread != get_callback_thread_id());
}

/**********************************************************************/
void invoke_callback(struct vdo_completion *completion)
{
	if (requires_enqueue(completion)) {
		enqueue_completion(completion);
		return;
	}

	run_callback(completion);
}

/**********************************************************************/
void continue_completion(struct vdo_completion *completion, int result)
{
	set_completion_result(completion, result);
	invoke_callback(completion);
}

/**********************************************************************/
void complete_completion(struct vdo_completion *completion)
{
	assert_incomplete(completion);
	completion->complete = true;
	if (completion->callback != NULL) {
		invoke_callback(completion);
	}
}

/**********************************************************************/
void release_completion(struct vdo_completion **completion_ptr)
{
	struct vdo_completion *completion = *completion_ptr;
	if (completion == NULL) {
		return;
	}

	*completion_ptr = NULL;
	complete_completion(completion);
}

/**********************************************************************/
void release_completion_with_result(struct vdo_completion **completion_ptr,
				    int result)
{
	if (*completion_ptr == NULL) {
		return;
	}

	set_completion_result(*completion_ptr, result);
	release_completion(completion_ptr);
}

/**********************************************************************/
void finish_parent_callback(struct vdo_completion *completion)
{
	finish_completion((struct vdo_completion *) completion->parent,
			  completion->result);
}

/**********************************************************************/
void preserve_error_and_continue(struct vdo_completion *completion)
{
	if (completion->parent != NULL) {
		set_completion_result(completion->parent, completion->result);
	}

	reset_completion(completion);
	invoke_callback(completion);
}

/**********************************************************************/
const char *get_completion_type_name(enum vdo_completion_type completion_type)
{
	// Try to catch failures to update the array when the enum values
	// change.
	STATIC_ASSERT(COUNT_OF(VDO_COMPLETION_TYPE_NAMES) ==
		      (MAX_COMPLETION_TYPE - UNSET_COMPLETION_TYPE));

	if (completion_type >= MAX_COMPLETION_TYPE) {
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

/**********************************************************************/
int assert_completion_type(enum vdo_completion_type actual,
			   enum vdo_completion_type expected)
{
	return ASSERT((expected == actual),
		      "completion type is %s instead of %s",
		      get_completion_type_name(actual),
		      get_completion_type_name(expected));
}
