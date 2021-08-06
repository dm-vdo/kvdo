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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/syncCompletion.c#1 $
 */

#include "syncCompletion.h"

#include <linux/delay.h>

#include "completion.h"

struct sync_completion {
	struct vdo_completion vdo_completion;
	struct completion completion;
	vdo_action *action;
};

/**
 * Convert a vdo_completion to a sync completion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a sync completion.
 **/
static inline struct sync_completion * __must_check
as_sync_completion(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type, VDO_SYNC_COMPLETION);
	return container_of(completion,
			    struct sync_completion,
			    vdo_completion);
}

/**
 * A vdo_action to signal the waiting thread that a synchronous action is
 * complete. This callback is registered in run_synchrnous_action().
 *
 * @param completion  The sync completion
 **/
static void complete_synchronous_action(struct vdo_completion *completion)
{
	complete(&(as_sync_completion(completion)->completion));
}

/**
 * A vdo_action to perform a synchronous action registered in
 * perform_synchronous_vdo_action().
 *
 * @param completion  The sync completion
 **/
static void run_synchronous_action(struct vdo_completion *completion)
{
	completion->callback = complete_synchronous_action;
	as_sync_completion(completion)->action(completion);
}

/**********************************************************************/
int perform_synchronous_vdo_action(struct vdo *vdo,
				   vdo_action *action,
				   thread_id_t thread_id,
				   void *parent)
{
	struct sync_completion sync;

	initialize_vdo_completion(&sync.vdo_completion, vdo, VDO_SYNC_COMPLETION);
	init_completion(&sync.completion);
	sync.action = action;
	launch_vdo_completion_callback_with_parent(&sync.vdo_completion,
						   run_synchronous_action,
						   thread_id,
						   parent);
	wait_for_completion(&sync.completion);
	return sync.vdo_completion.result;
}
