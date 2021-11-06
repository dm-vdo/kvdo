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

/*
 * A read_only_notifier is responsible for propogating the fact that the VDO
 * has encountered an unrecoverable error to all base threads. It also persists
 * the read-only state to the super block.
 *
 * The notifier also provides the ability to wait for any notifications to be
 * complete in order to not cause super block write races when shutting down
 * the VDO.
 */

#ifndef READ_ONLY_NOTIFIER_H
#define READ_ONLY_NOTIFIER_H

#include "completion.h"

/**
 * A function to notify a listener that the VDO has gone read-only.
 *
 * @param listener  The object to notify
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
typedef void vdo_read_only_notification(void *listener,
					struct vdo_completion *parent);

int __must_check
make_vdo_read_only_notifier(bool is_read_only,
			    const struct thread_config *thread_config,
			    struct vdo *vdo,
			    struct read_only_notifier **notifier_ptr);

void free_vdo_read_only_notifier(struct read_only_notifier *notifier);

void
vdo_wait_until_not_entering_read_only_mode(struct read_only_notifier *notifier,
					   struct vdo_completion *parent);

void vdo_allow_read_only_mode_entry(struct read_only_notifier *notifier,
				    struct vdo_completion *parent);

void vdo_enter_read_only_mode(struct read_only_notifier *notifier,
			      int error_code);

bool __must_check vdo_is_read_only(struct read_only_notifier *notifier);

bool __must_check
vdo_is_or_will_be_read_only(struct read_only_notifier *notifier);

int register_vdo_read_only_listener(struct read_only_notifier *notifier,
				    void *listener,
				    vdo_read_only_notification *notification,
				    thread_id_t thread_id);

#endif /* READ_ONLY_NOTIFIER_H */
