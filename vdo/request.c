// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "request.h"

#include "index.h"
#include "index-session.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "request-queue.h"

int uds_start_chunk_operation(struct uds_request *request)
{
	size_t internal_size;
	int result;

	if (request->callback == NULL) {
		uds_log_error("missing required callback");
		return -EINVAL;
	}
	switch (request->type) {
	case UDS_DELETE:
	case UDS_POST:
	case UDS_QUERY:
	case UDS_QUERY_NO_UPDATE:
	case UDS_UPDATE:
		break;
	default:
		uds_log_error("received invalid callback type");
		return -EINVAL;
	}

	/* Reset all internal fields before processing. */
	internal_size = sizeof(struct uds_request)
		- offsetof(struct uds_request, zone_number);
        // XXX should be using struct_group for this instead
	memset((char *) request + sizeof(*request) - internal_size,
	       0, internal_size);

	result = get_index_session(request->session);
	if (result != UDS_SUCCESS) {
		return result;
	}

	request->found = false;
	request->unbatched = false;
	request->index = request->session->index;

	enqueue_request(request, STAGE_TRIAGE);
	return UDS_SUCCESS;
}

int launch_zone_message(struct uds_zone_message message,
			unsigned int zone,
			struct uds_index *index)
{
	struct uds_request *request;
	int result = UDS_ALLOCATE(1, struct uds_request, __func__, &request);

	if (result != UDS_SUCCESS) {
		return result;
	}

	request->index = index;
	request->unbatched = true;
	request->zone_number = zone;
	request->zone_message = message;

	enqueue_request(request, STAGE_MESSAGE);
	return UDS_SUCCESS;
}

static struct uds_request_queue *
get_next_stage_queue(struct uds_request *request,
		     enum request_stage next_stage)
{
	if (next_stage == STAGE_CALLBACK) {
		return request->session->callback_queue;
	}

	return select_index_queue(request->index, request, next_stage);
}

void enqueue_request(struct uds_request *request,
		     enum request_stage next_stage)
{
	struct uds_request_queue *next_queue =
		get_next_stage_queue(request, next_stage);
	if (next_queue == NULL) {
		return;
	}

	uds_request_queue_enqueue(next_queue, request);
}

/*
 * This function pointer allows unit test code to intercept the slow-lane
 * requeuing of a request.
 */
static request_restarter_t request_restarter = NULL;

void restart_request(struct uds_request *request)
{
	request->requeued = true;
	if (request_restarter == NULL) {
		enqueue_request(request, STAGE_INDEX);
	} else {
		request_restarter(request);
	}
}

void set_request_restarter(request_restarter_t restarter)
{
	request_restarter = restarter;
}

static INLINE void increment_once(uint64_t *count_ptr)
{
	WRITE_ONCE(*count_ptr, READ_ONCE(*count_ptr) + 1);
}

void update_request_context_stats(struct uds_request *request)
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

	struct session_stats *session_stats = &request->session->stats;

	increment_once(&session_stats->requests);

	switch (request->type) {
	case UDS_POST:
		if (request->found) {
			increment_once(&session_stats->posts_found);

			if (request->location == UDS_LOCATION_IN_OPEN_CHAPTER) {
				increment_once(&session_stats->posts_found_open_chapter);
			} else if (request->location == UDS_LOCATION_IN_DENSE) {
				increment_once(&session_stats->posts_found_dense);
			} else if (request->location == UDS_LOCATION_IN_SPARSE) {
				increment_once(&session_stats->posts_found_sparse);
			}
		} else {
			increment_once(&session_stats->posts_not_found);
		}
		break;

	case UDS_UPDATE:
		if (request->found) {
			increment_once(&session_stats->updates_found);
		} else {
			increment_once(&session_stats->updates_not_found);
		}
		break;

	case UDS_DELETE:
		if (request->found) {
			increment_once(&session_stats->deletions_found);
		} else {
			increment_once(&session_stats->deletions_not_found);
		}
		break;

	case UDS_QUERY:
	case UDS_QUERY_NO_UPDATE:
		if (request->found) {
			increment_once(&session_stats->queries_found);
		} else {
			increment_once(&session_stats->queries_not_found);
		}
		break;

	default:
		request->status = ASSERT(false,
					 "unknown request type: %d",
					 request->type);
	}
}

void enter_callback_stage(struct uds_request *request)
{
	if (request->status != UDS_SUCCESS) {
		/* All request errors are considered unrecoverable */
		disable_index_session(request->session);
	}

	request->status = uds_map_to_system_error(request->status);
	/* Handle asynchronous client callbacks in the designated thread. */
	enqueue_request(request, STAGE_CALLBACK);
}
