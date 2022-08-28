/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SYNC_COMPLETION_H
#define SYNC_COMPLETION_H

#include "completion.h"
#include "types.h"

int vdo_perform_synchronous_action(struct vdo *vdo,
				   vdo_action * action,
				   thread_id_t thread_id,
				   void *parent);

#endif /* SYNC_COMPLETION_H */
