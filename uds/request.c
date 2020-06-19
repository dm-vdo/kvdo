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
 * $Id: //eng/uds-releases/krusty/src/uds/request.c#6 $
 */

#include "request.h"

#include "indexRouter.h"
#include "indexSession.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "requestQueue.h"

/**********************************************************************/
int uds_start_chunk_operation(struct uds_request *uds_request)
{
	if (uds_request->callback == NULL) {
		return UDS_CALLBACK_REQUIRED;
	}
	switch (uds_request->type) {
	case UDS_DELETE:
	case UDS_POST:
	case UDS_QUERY:
	case UDS_UPDATE:
		break;
	default:
		return UDS_INVALID_OPERATION_TYPE;
	}
	memset(uds_request->private, 0, sizeof(uds_request->private));
	Request *request = (Request *) uds_request;

	int result = getIndexSession(request->session);
	if (result != UDS_SUCCESS) {
		return sansUnrecoverable(result);
	}

	request->found = false;
	request->action = (enum request_action) request->type;
	request->is_control_message = false;
	request->unbatched = false;
	request->router = request->session->router;

	enqueue_request(request, STAGE_TRIAGE);
	return UDS_SUCCESS;
}

/**********************************************************************/
int launch_zone_control_message(enum request_action action,
				struct zone_message message,
				unsigned int zone,
				struct index_router *router)
{
	Request *request;
	int result = ALLOCATE(1, Request, __func__, &request);
	if (result != UDS_SUCCESS) {
		return result;
	}

	request->router = router;
	request->is_control_message = true;
	request->unbatched = true;
	request->action = action;
	request->zone_number = zone;
	request->zone_message = message;

	enqueue_request(request, STAGE_INDEX);
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_request(Request *request)
{
	if (request != NULL) {
		FREE(request);
	}
}

/**********************************************************************/
static RequestQueue *get_next_stage_queue(Request *request,
					  enum request_stage next_stage)
{
	if (next_stage == STAGE_CALLBACK) {
		return request->session->callbackQueue;
	}

	// Local and remote index routers handle the rest of the pipeline
	// differently, so delegate the choice of queue to the router.
	return select_index_router_queue(request->router, request, next_stage);
}

/**********************************************************************/
static void handle_request_errors(Request *request)
{
	// XXX Use the router's callback function to hand back the error
	// and clean up the request? (Possible thread issues doing that.)

	free_request(request);
}

/**********************************************************************/
void enqueue_request(Request *request, enum request_stage next_stage)
{
	RequestQueue *next_queue = get_next_stage_queue(request, next_stage);
	if (next_queue == NULL) {
		handle_request_errors(request);
		return;
	}

	requestQueueEnqueue(next_queue, request);
}

/*
 * This function pointer allows unit test code to intercept the slow-lane
 * requeuing of a request.
 */
static request_restarter_t request_restarter = NULL;

/**********************************************************************/
void restart_request(Request *request)
{
	request->requeued = true;
	if (request_restarter == NULL) {
		enqueue_request(request, STAGE_INDEX);
	} else {
		request_restarter(request);
	}
}

/**********************************************************************/
void set_request_restarter(request_restarter_t restarter)
{
	request_restarter = restarter;
}

/**********************************************************************/
static INLINE void increment_once(uint64_t *count_ptr)
{
	WRITE_ONCE(*count_ptr, READ_ONCE(*count_ptr) + 1);
}

/**********************************************************************/
void update_request_context_stats(Request *request)
{
	/*
	 * We don't need any synchronization since the context stats are only
	 *  modified from the single callback thread.
	 *
	 * We increment either 2 or 3 counters in this method.
	 *
	 * XXX We always increment the "requests" counter.  But there is no
	 * code that uses the value stored in this counter.
	 *
	 * We always increment exactly one of these counters (unless there is
	 * an error in the code, which never happens): postsFound postsNotFound
	 *     updatesFound    updatesNotFound
	 *     deletionsFound  deletionsNotFound
	 *     queriesFound    queriesNotFound
	 *
	 * XXX In the case of post request that were found in the index, we
	 * increment exactly one of these counters.  But there is no code that
	 * uses the value stored in these counters. inMemoryPostsFound
	 *          densePostsFound
	 *          sparsePostsFound
	 */

	SessionStats *session_stats = &request->session->stats;

	increment_once(&session_stats->requests);
	bool found = (request->location != LOC_UNAVAILABLE);

	switch (request->action) {
	case REQUEST_INDEX:
		if (found) {
			increment_once(&session_stats->postsFound);

			if (request->location == LOC_IN_OPEN_CHAPTER) {
				increment_once(&session_stats->postsFoundOpenChapter);
			} else if (request->location == LOC_IN_DENSE) {
				increment_once(&session_stats->postsFoundDense);
			} else if (request->location == LOC_IN_SPARSE) {
				increment_once(&session_stats->postsFoundSparse);
			}
		} else {
			increment_once(&session_stats->postsNotFound);
		}
		break;

	case REQUEST_UPDATE:
		if (found) {
			increment_once(&session_stats->updatesFound);
		} else {
			increment_once(&session_stats->updatesNotFound);
		}
		break;

	case REQUEST_DELETE:
		if (found) {
			increment_once(&session_stats->deletionsFound);
		} else {
			increment_once(&session_stats->deletionsNotFound);
		}
		break;

	case REQUEST_QUERY:
		if (found) {
			increment_once(&session_stats->queriesFound);
		} else {
			increment_once(&session_stats->queriesNotFound);
		}
		break;

	default:
		request->status = ASSERT(false,
					 "unknown next action in request: %d",
					 request->action);
	}
}

/**********************************************************************/
void enter_callback_stage(Request *request)
{
	if (!request->is_control_message) {
		if (isUnrecoverable(request->status)) {
			// Unrecoverable errors must disable the index session
			disableIndexSession(request->session);
			// The unrecoverable state is internal and must not
			// sent to the client.
			request->status = sansUnrecoverable(request->status);
		}

		// Handle asynchronous client callbacks in the designated
		// thread.
		enqueue_request(request, STAGE_CALLBACK);
	} else {
		/*
		 * Asynchronous control messages are complete when they are
		 * executed. There should be nothing they need to do on the
		 * callback thread. The message has been completely processed,
		 * so just free it.
		 */
		free_request(request);
	}
}
