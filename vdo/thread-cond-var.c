// SPDX-License-Identifier: GPL-2.0-only
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
