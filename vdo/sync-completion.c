// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "sync-completion.h"

#include <linux/delay.h>

#include "completion.h"

struct sync_completion {
	struct vdo_completion vdo_completion;
	struct completion completion;
	vdo_action *action;
};

/**
 * as_sync_completion() - Convert a vdo_completion to a sync completion.
 * @completion: The completion to convert.
 *
 * Return: The completion as a sync completion.
 */
static inline struct sync_completion * __must_check
as_sync_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_SYNC_COMPLETION);
	return container_of(completion,
			    struct sync_completion,
			    vdo_completion);
}

/**
 * complete_synchronous_action() - A vdo_action to signal the waiting thread
 *                                 that a synchronous action is complete.
 * @completion: The sync completion.
 *
 * This callback is registered in run_synchrnous_action().
 */
static void complete_synchronous_action(struct vdo_completion *completion)
{
	complete(&(as_sync_completion(completion)->completion));
}

/**
 * run_synchronous_action() - A vdo_action to perform a synchronous action
 *                            registered in vdo_perform_synchronous_action().
 * @completion: The sync completion.
 */
static void run_synchronous_action(struct vdo_completion *completion)
{
	completion->callback = complete_synchronous_action;
	as_sync_completion(completion)->action(completion);
}

/**
 * vdo_perform_synchronous_action() - Launch an action on a VDO thread and
 *                                    wait for it to complete.
 * @vdo: The vdo.
 * @action: The callback to launch.
 * @thread_id: The thread on which to run the action.
 * @parent: The parent of the sync completion (may be NULL).
 */
int vdo_perform_synchronous_action(struct vdo *vdo,
				   vdo_action *action,
				   thread_id_t thread_id,
				   void *parent)
{
	struct sync_completion sync;

	vdo_initialize_completion(&sync.vdo_completion, vdo, VDO_SYNC_COMPLETION);
	init_completion(&sync.completion);
	sync.action = action;
	vdo_launch_completion_callback_with_parent(&sync.vdo_completion,
						   run_synchronous_action,
						   thread_id,
						   parent);
	wait_for_completion(&sync.completion);
	return sync.vdo_completion.result;
}
