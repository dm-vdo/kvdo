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

#include "read-only-notifier.h"

#include <linux/atomic.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "completion.h"
#include "thread-config.h"
#include "vdo.h"

/**
 * A read_only_notifier has a single completion which is used to perform
 * read-only notifications, however, vdo_enter_read_only_mode() may be called
 * from any thread. A pair of atomic fields are used to control the read-only
 * mode entry process. The first field holds the read-only error. The second is
 * the state field, which may hold any of the four special values enumerated
 * here.
 *
 * When vdo_enter_read_only_mode() is called on some base thread, a
 * compare-and-swap is done on read_only_error, setting it to the supplied
 * error if the value was VDO_SUCCESS. If this fails, some other thread has
 * already initiated read-only entry or scheduled a pending entry, so the call
 * exits. Otherwise, a compare-and-swap is done on the state, setting it to
 * NOTIFYING if the value was MAY_NOTIFY. If this succeeds, the caller
 * initiates the notification. If this failed due to notifications being
 * disallowed, the notifier will be in the MAY_NOT_NOTIFY state but
 * read_only_error will not be VDO_SUCCESS. This configuration will indicate to
 * vdo_allow_read_only_mode_entry() that there is a pending notification to
 * perform.
 **/
enum {
	/** Notifications are allowed but not in progress */
	MAY_NOTIFY,
	/** A notification is in progress */
	NOTIFYING,
	/** Notifications are not allowed */
	MAY_NOT_NOTIFY,
	/** A notification has completed */
	NOTIFIED,
};

/**
 * An object to be notified when the VDO enters read-only mode
 **/
struct read_only_listener {
	/** The listener */
	void *listener;
	/** The method to call to notify the listener */
	vdo_read_only_notification *notify;
	/** A pointer to the next listener */
	struct read_only_listener *next;
};

/**
 * Data associated with each base code thread.
 **/
struct thread_data {
	/**
	 * Each thread maintains its own notion of whether the VDO is read-only
	 * so that the read-only state can be checked from any base thread
	 * without worrying about synchronization or thread safety. This does
	 * mean that knowledge of the VDO going read-only does not occur
	 * simultaneously across the VDO's threads, but that does not seem to
	 * cause any problems.
	 */
	bool is_read_only;
	/**
	 * A list of objects waiting to be notified on this thread that the VDO
	 * has entered read-only mode.
	 **/
	struct read_only_listener *listeners;
};

struct read_only_notifier {
	/** The completion for entering read-only mode */
	struct vdo_completion completion;
	/** A completion waiting for notifications to be drained or enabled */
	struct vdo_completion *waiter;
	/** The code of the error which put the VDO into read-only mode */
	atomic_t read_only_error;
	/** The current state of the notifier (values described above) */
	atomic_t state;
	/** The thread config of the VDO */
	const struct thread_config *thread_config;
	/** The array of per-thread data */
	struct thread_data thread_data[];
};

/**
 * Convert a generic vdo_completion to a read_only_notifier.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a read_only_notifier
 **/
static inline struct read_only_notifier *
as_notifier(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_READ_ONLY_MODE_COMPLETION);
	return container_of(completion, struct read_only_notifier, completion);
}

/**
 * Create a read-only notifer.
 *
 * @param [in]  is_read_only     Whether the VDO is already read-only
 * @param [in]  thread_config    The thread configuration of the VDO
 * @param [in]  vdo              The VDO
 * @param [out] notifier_ptr     A pointer to receive the new notifier
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_make_read_only_notifier(bool is_read_only,
				const struct thread_config *thread_config,
				struct vdo *vdo,
				struct read_only_notifier **notifier_ptr)
{
	struct read_only_notifier *notifier;
	thread_count_t id;
	int result = UDS_ALLOCATE_EXTENDED(struct read_only_notifier,
					   thread_config->base_thread_count,
					   struct thread_data,
					   __func__,
					   &notifier);
	if (result != VDO_SUCCESS) {
		return result;
	}

	notifier->thread_config = thread_config;
	if (is_read_only) {
		atomic_set(&notifier->read_only_error, VDO_READ_ONLY);
		atomic_set(&notifier->state, NOTIFIED);
	} else {
		atomic_set(&notifier->state, MAY_NOT_NOTIFY);
	}

	vdo_initialize_completion(&notifier->completion, vdo,
				  VDO_READ_ONLY_MODE_COMPLETION);

	for (id = 0; id < thread_config->base_thread_count; id++) {
		notifier->thread_data[id].is_read_only = is_read_only;
	}

	*notifier_ptr = notifier;
	return VDO_SUCCESS;
}

/**
 * Free the list of read-only listeners associated with a thread.
 *
 * @param thread_data  The thread holding the list to free
 **/
static void free_listeners(struct thread_data *thread_data)
{
	struct read_only_listener *listener, *next;

	for (listener = UDS_FORGET(thread_data->listeners);
	     listener != NULL;
	     listener = next) {
		next = UDS_FORGET(listener->next);
		UDS_FREE(listener);
	}
}

/**
 * Free a read_only_notifier.
 *
 * @param notifier  The notifier to free
 **/
void vdo_free_read_only_notifier(struct read_only_notifier *notifier)
{
	thread_count_t id;

	if (notifier == NULL) {
		return;
	}

	for (id = 0; id < notifier->thread_config->base_thread_count; id++) {
		free_listeners(&notifier->thread_data[id]);
	}

	UDS_FREE(notifier);
}

/**
 * Check that a function was called on the admin thread.
 *
 * @param notifier  The notifier
 * @param caller    The name of the function (for logging)
 **/
static void
assert_notifier_on_admin_thread(struct read_only_notifier *notifier,
				const char *caller)
{
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((notifier->thread_config->admin_thread == thread_id),
			"%s called on admin thread",
			caller);
}

/**
 * Wait until no read-only notifications are in progress and prevent any
 * subsequent notifications. Notifications may be re-enabled by calling
 * vdo_allow_read_only_mode_entry().
 *
 * @param notifier  The read-only notifier on which to wait
 * @param parent    The completion to notify when no threads are entering
 *                  read-only mode
 **/
void vdo_wait_until_not_entering_read_only_mode(struct read_only_notifier *notifier,
						struct vdo_completion *parent)
{
	int state;

	if (notifier == NULL) {
		vdo_finish_completion(parent, VDO_SUCCESS);
		return;
	}

	assert_notifier_on_admin_thread(notifier, __func__);
	if (notifier->waiter != NULL) {
		vdo_finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	/*
	 * Extra barriers because this was original developed using 
	 * a CAS operation that implicitly had them. 
	 */
	smp_mb__before_atomic();
	state = atomic_cmpxchg(&notifier->state,
			       MAY_NOTIFY, MAY_NOT_NOTIFY);
	smp_mb__after_atomic();

	if ((state == MAY_NOT_NOTIFY) || (state == NOTIFIED)) {
		/* Notifications are already done or disallowed. */
		vdo_complete_completion(parent);
		return;
	}

	if (state == MAY_NOTIFY) {
		/*
		 * A notification was not in progress, and now they are 
		 * disallowed. 
		 */
		vdo_complete_completion(parent);
		return;
	}

	/*
	 * A notification is in progress, so wait for it to finish. There is no
	 * race here since the notification can't finish while the admin thread
	 * is in this method.
	 */
	notifier->waiter = parent;
}

/**
 * Complete the process of entering read only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void finish_entering_read_only_mode(struct vdo_completion *completion)
{
	struct read_only_notifier *notifier = as_notifier(completion);
	struct vdo_completion *waiter = notifier->waiter;

	assert_notifier_on_admin_thread(notifier, __func__);
	smp_wmb();
	atomic_set(&notifier->state, NOTIFIED);

	if (waiter != NULL) {
		notifier->waiter = NULL;
		vdo_finish_completion(waiter, completion->result);
	}
}

/**
 * Inform each thread that the VDO is in read-only mode.
 *
 * @param completion  The read-only mode completion
 **/
static void make_thread_read_only(struct vdo_completion *completion)
{
	thread_id_t thread_id = completion->callback_thread_id;
	struct read_only_notifier *notifier = as_notifier(completion);
	struct read_only_listener *listener = completion->parent;

	if (listener == NULL) {
		/* This is the first call on this thread */
		struct thread_data *thread_data =
			&notifier->thread_data[thread_id];
		thread_data->is_read_only = true;
		listener = thread_data->listeners;
		if (thread_id == 0) {
			/*
			 * Note: This message must be recognizable by 
			 * Permabit::UserMachine. 
			 */
			uds_log_error_strerror(atomic_read(&notifier->read_only_error),
					       "Unrecoverable error, entering read-only mode");
		}
	} else {
		/* We've just finished notifying a listener */
		listener = listener->next;
	}

	if (listener != NULL) {
		/* We have a listener to notify */
		vdo_prepare_completion(completion,
				       make_thread_read_only,
				       make_thread_read_only,
				       thread_id,
				       listener);
		listener->notify(listener->listener, completion);
		return;
	}

	/* We're done with this thread */
	if (++thread_id >= notifier->thread_config->base_thread_count) {
		/* There are no more threads */
		vdo_prepare_completion(completion,
				       finish_entering_read_only_mode,
				       finish_entering_read_only_mode,
				       notifier->thread_config->admin_thread,
				       NULL);
	} else {
		vdo_prepare_completion(completion,
				       make_thread_read_only,
				       make_thread_read_only,
				       thread_id,
				       NULL);
	}

	vdo_invoke_completion_callback(completion);
}

/**
 * Allow the notifier to put the VDO into read-only mode, reversing the effects
 * of vdo_wait_until_not_entering_read_only_mode(). If some thread tried to put
 * the VDO into read-only mode while notifications were disallowed, it will be
 * done when this method is called. If that happens, the parent will not be
 * notified until the VDO has actually entered read-only mode and attempted to
 * save the super block.
 *
 * <p>This method may only be called from the admin thread.
 *
 * @param notifier  The notifier
 * @param parent    The object to notify once the operation is complete
 **/
void vdo_allow_read_only_mode_entry(struct read_only_notifier *notifier,
				    struct vdo_completion *parent)
{
	int state;

	assert_notifier_on_admin_thread(notifier, __func__);
	if (notifier->waiter != NULL) {
		vdo_finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	/*
	 * Extra barriers because this was original developed using 
	 * a CAS operation that implicitly had them. 
	 */
	smp_mb__before_atomic();
	state = atomic_cmpxchg(&notifier->state,
			       MAY_NOT_NOTIFY, MAY_NOTIFY);
	smp_mb__after_atomic();

	if (state != MAY_NOT_NOTIFY) {
		/* Notifications were already allowed or complete. */
		vdo_complete_completion(parent);
		return;
	}

	if (atomic_read(&notifier->read_only_error) == VDO_SUCCESS) {
		smp_rmb();
		/* We're done */
		vdo_complete_completion(parent);
		return;
	}

	/* There may have been a pending notification */

	/*
	 * Extra barriers because this was original developed using 
	 * a CAS operation that implicitly had them. 
	 */
	smp_mb__before_atomic();
	state = atomic_cmpxchg(&notifier->state, MAY_NOTIFY, NOTIFYING);
	smp_mb__after_atomic();

	if (state != MAY_NOTIFY) {
		/*
		 * There wasn't a pending notification; the error check raced
		 * with a thread calling vdo_enter_read_only_mode() after we
		 * set the state to MAY_NOTIFY. It has already started the
		 * notification.
		 */
		vdo_complete_completion(parent);
		return;
	}

	/* Do the pending notification. */
	notifier->waiter = parent;
	make_thread_read_only(&notifier->completion);
}

/**
 * Put a VDO into read-only mode and save the read-only state in the super
 * block. This method is a no-op if the VDO is already read-only.
 *
 * @param notifier         The read-only notifier of the VDO
 * @param error_code       The error which caused the VDO to enter read-only
 *                         mode
 **/
void vdo_enter_read_only_mode(struct read_only_notifier *notifier,
			      int error_code)
{
	int state;
	thread_id_t thread_id = vdo_get_callback_thread_id();
	struct thread_data *thread_data;

	if (thread_id != VDO_INVALID_THREAD_ID) {
		thread_data = &notifier->thread_data[thread_id];
		if (thread_data->is_read_only) {
			/* This thread has already gone read-only. */
			return;
		}

		/* Record for this thread that the VDO is read-only. */
		thread_data->is_read_only = true;
	}

	/*
	 * Extra barriers because this was original developed using a CAS 
	 * operation that implicitly had them. 
	 */
	smp_mb__before_atomic();
	state = atomic_cmpxchg(&notifier->read_only_error,
			       VDO_SUCCESS,
			       error_code);
	smp_mb__after_atomic();

	if (state != VDO_SUCCESS) {
		/* The notifier is already aware of a read-only error */
		return;
	}

	state = atomic_cmpxchg(&notifier->state, MAY_NOTIFY, NOTIFYING);
	/*
	 * Extra barrier because this was original developed using a CAS 
	 * operation that implicitly had them. 
	 */
	smp_mb__after_atomic();

	if (state != MAY_NOTIFY) {
		return;
	}

	/* Initiate a notification starting on the lowest numbered thread. */
	vdo_launch_completion_callback(&notifier->completion,
				       make_thread_read_only, 0);
}

/**
 * Check whether the VDO is read-only. This method may be called from any
 * thread, as opposed to examining the VDO's state field which is only safe
 * to check from the admin thread.
 *
 * @param notifier        The read-only notifier of the VDO
 *
 * @return <code>true</code> if the VDO is read-only
 **/
bool vdo_is_read_only(struct read_only_notifier *notifier)
{
	return notifier->thread_data[vdo_get_callback_thread_id()].is_read_only;
}

/**
 * Check whether the VDO is or will be read-only (i.e. some thread has started
 * the process of entering read-only mode, but not all threads have been
 * notified yet). This method should only be called in cases where the expense
 * of reading atomic state is not a problem. It was introduced in order to allow
 * suppresion of spurious error messages resulting from VIO cleanup racing with
 * read-only notification.
 *
 * @param notifier  The read-only notifier of the VDO
 *
 * @return <code>true</code> if the VDO has started (and possibly finished)
 *         the process of entering read-only mode
 **/
bool vdo_is_or_will_be_read_only(struct read_only_notifier *notifier)
{
	return (atomic_read(&notifier->read_only_error) != VDO_SUCCESS);
}

/**
 * Register a listener to be notified when the VDO goes read-only.
 *
 * @param notifier       The notifier to register with
 * @param listener       The object to notify
 * @param notification   The function to call to send the notification
 * @param thread_id      The id of the thread on which to send the notification
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_register_read_only_listener(struct read_only_notifier *notifier,
				    void *listener,
				    vdo_read_only_notification *notification,
				    thread_id_t thread_id)
{
	struct thread_data *thread_data = &notifier->thread_data[thread_id];
	struct read_only_listener *read_only_listener;
	int result = UDS_ALLOCATE(1,
				  struct read_only_listener,
				  __func__,
				  &read_only_listener);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*read_only_listener = (struct read_only_listener) {
		     .listener = listener,
		     .notify = notification,
		     .next = thread_data->listeners,
	};

	thread_data->listeners = read_only_listener;
	return VDO_SUCCESS;
}
