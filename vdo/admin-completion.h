/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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

typedef thread_id_t
vdo_thread_id_getter_for_phase(struct admin_completion *admin_completion);

struct admin_completion {
	/*
	 * FIXME this field should be replaced by container_of() when
	 * enqueuables go away and this becomes a field of struct vdo.
	 */
	struct vdo *vdo;
	struct vdo_completion completion;
	struct vdo_completion sub_task_completion;
	atomic_t busy;
	enum admin_operation_type type;
	vdo_thread_id_getter_for_phase *get_thread_id;
	uint32_t phase;
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
