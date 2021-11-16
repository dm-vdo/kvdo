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

#ifndef ADMIN_COMPLETION_H
#define ADMIN_COMPLETION_H

#include <linux/atomic.h>
#include <linux/delay.h>

#include "uds-threads.h"

#include "completion.h"
#include "types.h"

enum admin_operation_type {
	VDO_ADMIN_OPERATION_UNKNOWN,
	VDO_ADMIN_OPERATION_GROW_LOGICAL,
	VDO_ADMIN_OPERATION_GROW_PHYSICAL,
	VDO_ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
	VDO_ADMIN_OPERATION_LOAD,
	VDO_ADMIN_OPERATION_PRE_LOAD,
	VDO_ADMIN_OPERATION_RESUME,
	VDO_ADMIN_OPERATION_SUSPEND,
};

struct admin_completion;

/**
 * A function which gets the ID of the thread on which the current phase of an
 * admin operation should be run.
 *
 * @param admin_completion The admin_completion
 *
 * @return The ID of the thread on which the current phase should be performed
 **/
typedef thread_id_t
vdo_thread_id_getter_for_phase(struct admin_completion *admin_completion);

struct admin_completion {
	/*
	 * XXX should be replaced by container_of() when enqueuables go away 
	 * and this becomes a field of struct vdo. 
	 */
	struct vdo *vdo;
	/** The completion */
	struct vdo_completion completion;
	/** The sub-task completion */
	struct vdo_completion sub_task_completion;
	/** Whether this completion is in use */
	atomic_t busy;
	/** The operation type */
	enum admin_operation_type type;
	/** Method to get the thread id for the current phase */
	vdo_thread_id_getter_for_phase *get_thread_id;
	/** The current phase of the operation */
	uint32_t phase;
	/** The struct completion for waiting on the operation */
	struct completion callback_sync;
};

void vdo_assert_admin_operation_type(struct admin_completion *completion,
				     enum admin_operation_type expected);

struct admin_completion * __must_check
vdo_admin_completion_from_sub_task(struct vdo_completion *completion);

void vdo_assert_admin_phase_thread(struct admin_completion *admin_completion,
				   const char *what,
				   const char *phase_names[]);

struct vdo * __must_check
vdo_from_admin_sub_task(struct vdo_completion *completion,
			enum admin_operation_type expected);

void vdo_initialize_admin_completion(struct vdo *vdo,
				     struct admin_completion *admin_completion);

struct vdo_completion *vdo_reset_admin_sub_task(struct vdo_completion *completion);

void vdo_prepare_admin_sub_task(struct vdo *vdo,
				vdo_action *callback,
				vdo_action *error_handler);

int __must_check
vdo_perform_admin_operation(struct vdo *vdo,
			    enum admin_operation_type type,
			    vdo_thread_id_getter_for_phase *thread_id_getter,
			    vdo_action *action,
			    vdo_action *error_handler);

#endif /* ADMIN_COMPLETION_H */
