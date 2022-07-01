// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "errors.h"
#include "time-utils.h"
#include "uds-threads.h"

int uds_init_cond(struct cond_var *cv)
{
	cv->event_count = NULL;
	return make_event_count(&cv->event_count);
}

int uds_signal_cond(struct cond_var *cv)
{
	event_count_broadcast(cv->event_count);
	return UDS_SUCCESS;
}

int uds_broadcast_cond(struct cond_var *cv)
{
	event_count_broadcast(cv->event_count);
	return UDS_SUCCESS;
}

int uds_wait_cond(struct cond_var *cv, struct mutex *mutex)
{
	event_token_t token = event_count_prepare(cv->event_count);

	uds_unlock_mutex(mutex);
	event_count_wait(cv->event_count, token, NULL);
	uds_lock_mutex(mutex);
	return UDS_SUCCESS;
}

int uds_timed_wait_cond(struct cond_var *cv,
			struct mutex *mutex,
			ktime_t timeout)
{
	bool happened;
	event_token_t token = event_count_prepare(cv->event_count);

	uds_unlock_mutex(mutex);
	happened = event_count_wait(cv->event_count, token, &timeout);
	uds_lock_mutex(mutex);
	return happened ? UDS_SUCCESS : ETIMEDOUT;
}

int uds_destroy_cond(struct cond_var *cv)
{
	free_event_count(cv->event_count);
	cv->event_count = NULL;
	return UDS_SUCCESS;
}
