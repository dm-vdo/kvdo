/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadCondVarLinuxKernel.c#5 $
 */

#include "threads.h"
#include "timeUtils.h"
#include "uds-error.h"

/**********************************************************************/
int init_cond(struct cond_var *cv)
{
	cv->event_count = NULL;
	return make_event_count(&cv->event_count);
}

/**********************************************************************/
int signal_cond(struct cond_var *cv)
{
	event_count_broadcast(cv->event_count);
	return UDS_SUCCESS;
}

/**********************************************************************/
int broadcast_cond(struct cond_var *cv)
{
	event_count_broadcast(cv->event_count);
	return UDS_SUCCESS;
}

/**********************************************************************/
int wait_cond(struct cond_var *cv, struct mutex *mutex)
{
	event_token_t token = event_count_prepare(cv->event_count);
	unlock_mutex(mutex);
	event_count_wait(cv->event_count, token, NULL);
	lock_mutex(mutex);
	return UDS_SUCCESS;
}

/**********************************************************************/
int timed_wait_cond(struct cond_var *cv,
		    struct mutex *mutex,
		    rel_time_t timeout)
{
	event_token_t token = event_count_prepare(cv->event_count);
	unlock_mutex(mutex);
	bool happened = event_count_wait(cv->event_count, token, &timeout);
	lock_mutex(mutex);
	return happened ? UDS_SUCCESS : ETIMEDOUT;
}

/**********************************************************************/
int destroy_cond(struct cond_var *cv)
{
	free_event_count(cv->event_count);
	cv->event_count = NULL;
	return UDS_SUCCESS;
}
