/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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
 * typedef vdo_read_only_notification - A function to notify a listener that
 *                                      the VDO has gone read-only.
 * @listener: The object to notify.
 * @parent: The completion to notify in order to acknowledge the notification.
 */
typedef void vdo_read_only_notification(void *listener,
					struct vdo_completion *parent);

int __must_check
vdo_make_read_only_notifier(bool is_read_only,
			    const struct thread_config *thread_config,
			    struct vdo *vdo,
			    struct read_only_notifier **notifier_ptr);

void vdo_free_read_only_notifier(struct read_only_notifier *notifier);

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

int vdo_register_read_only_listener(struct read_only_notifier *notifier,
				    void *listener,
				    vdo_read_only_notification *notification,
				    thread_id_t thread_id);

#endif /* READ_ONLY_NOTIFIER_H */
