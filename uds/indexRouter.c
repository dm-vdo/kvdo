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
 * $Id: //eng/uds-releases/krusty/src/uds/indexRouter.c#10 $
 */

#include "indexRouter.h"

#include "compiler.h"
#include "indexCheckpoint.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "requestQueue.h"
#include "zone.h"

/**
 * This is the request processing function invoked by the zone's RequestQueue
 * worker thread.
 *
 * @param request  the request to be indexed or executed by the zone worker
 **/
static void execute_zone_request(Request *request)
{
	execute_index_router_request(request->router, request);
}

/**
 * Construct and enqueue asynchronous control messages to add the chapter
 * index for a given virtual chapter to the sparse chapter index cache.
 *
 * @param router          the router containing the relevant queues
 * @param index           the index with the relevant cache and chapter
 * @param virtual_chapter  the virtual chapter number of the chapter to cache
 **/
static void enqueue_barrier_messages(struct index_router *router,
				     struct index *index,
				     uint64_t virtual_chapter)
{
	ZoneMessage barrier = { .index = index,
				.data = { .barrier = {
						  .virtualChapter =
							  virtual_chapter,
					  } } };
	unsigned int zone;
	for (zone = 0; zone < router->zone_count; zone++) {
		int result =
			launchZoneControlMessage(REQUEST_SPARSE_CACHE_BARRIER,
						 barrier, zone, router);
		ASSERT_LOG_ONLY((result == UDS_SUCCESS),
				"barrier message allocation");
	}
}

/**
 * This is the request processing function for the triage stage queue. Each
 * request is resolved in the master index, determining if it is a hook or
 * not, and if a hook, what virtual chapter (if any) it might be found in. If
 * a virtual chapter is found, this enqueues a sparse chapter cache barrier in
 * every zone before enqueueing the request in its zone.
 *
 * @param request  the request to triage
 **/
static void triage_request(Request *request)
{
	struct index_router *router = request->router;
	struct index *index = router->index;

	// Check if the name is a hook in the index pointing at a sparse
	// chapter.
	uint64_t sparse_virtual_chapter = triage_index_request(index, request);
	if (sparse_virtual_chapter != UINT64_MAX) {
		// Generate and place a barrier request on every zone queue.
		enqueue_barrier_messages(router, index,
					 sparse_virtual_chapter);
	}

	enqueueRequest(request, STAGE_INDEX);
}

/**
 * Initialize the zone queues and the triage queue.
 *
 * @param router    the router containing the queues
 * @param geometry  the geometry governing the indexes
 *
 * @return  UDS_SUCCESS or error code
 **/
static int initialize_local_index_queues(struct index_router *router,
					 const struct geometry *geometry)
{
	unsigned int i;
	for (i = 0; i < router->zone_count; i++) {
		int result = makeRequestQueue("indexW",
					      &execute_zone_request,
					      &router->zone_queues[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	// The triage queue is only needed for sparse multi-zone indexes.
	if ((router->zone_count > 1) && is_sparse(geometry)) {
		int result = makeRequestQueue("triageW", &triage_request,
					      &router->triage_queue);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
static INLINE RequestQueue *get_zone_queue(struct index_router *router,
					   unsigned int zone_number)
{
	return router->zone_queues[zone_number];
}

/**********************************************************************/
int make_index_router(struct index_layout *layout,
		      const struct configuration *config,
		      const struct uds_parameters *user_params,
		      LoadType load_type,
		      IndexLoadContext *load_context,
		      index_router_callback_t callback,
		      struct index_router **router_ptr)
{
	unsigned int zone_count = getZoneCount(user_params);
	struct index_router *router;
	int result = ALLOCATE_EXTENDED(struct index_router,
				       zone_count,
				       RequestQueue *,
				       "index router",
				       &router);
	if (result != UDS_SUCCESS) {
		return result;
	}

	router->callback = callback;
	router->zone_count = zone_count;

	result = initialize_local_index_queues(router, config->geometry);
	if (result != UDS_SUCCESS) {
		free_index_router(router);
		return result;
	}

	result = make_index(layout,
			    config,
			    user_params,
			    router->zone_count,
			    load_type,
			    load_context,
			    &router->index);
	if (result != UDS_SUCCESS) {
		free_index_router(router);
		return logErrorWithStringError(result,
					       "failed to create index");
	}

	router->need_to_save = (router->index->loaded_type != LOAD_LOAD);
	*router_ptr = router;
	return UDS_SUCCESS;
}

/**********************************************************************/
int save_index_router(struct index_router *router)
{
	if (!router->need_to_save) {
		return UDS_SUCCESS;
	}
	int result = save_index(router->index);
	router->need_to_save = (result != UDS_SUCCESS);
	return result;
}

/**********************************************************************/
void free_index_router(struct index_router *router)
{
	if (router == NULL) {
		return;
	}
	requestQueueFinish(router->triage_queue);
	unsigned int i;
	for (i = 0; i < router->zone_count; i++) {
		requestQueueFinish(router->zone_queues[i]);
	}
	free_index(router->index);
	FREE(router);
}

/**********************************************************************/
RequestQueue *select_index_router_queue(struct index_router *router,
					Request *request,
					RequestStage next_stage)
{
	if (request->isControlMessage) {
		return get_zone_queue(router, request->zoneNumber);
	}

	if (next_stage == STAGE_TRIAGE) {
		// The triage queue is only needed for multi-zone sparse
		// indexes and won't be allocated by the router if not needed,
		// so simply check for NULL.
		if (router->triage_queue != NULL) {
			return router->triage_queue;
		}
		// Dense index or single zone, so route it directly to the zone
		// queue.
	} else if (next_stage != STAGE_INDEX) {
		ASSERT_LOG_ONLY(false, "invalid index stage: %d", next_stage);
		return NULL;
	}

	struct index *index = router->index;
	request->zoneNumber =
		getMasterIndexZone(index->master_index, &request->chunkName);
	return get_zone_queue(router, request->zoneNumber);
}

/**********************************************************************/
void execute_index_router_request(struct index_router *router,
				  Request *request)
{
	if (request->isControlMessage) {
		int result = dispatchIndexZoneControlRequest(request);
		if (result != UDS_SUCCESS) {
			logErrorWithStringError(result,
						"error executing control message: %d",
						request->action);
		}
		request->status = result;
		enterCallbackStage(request);
		return;
	}

	router->need_to_save = true;
	if (request->requeued && !isSuccessful(request->status)) {
		request->status = makeUnrecoverable(request->status);
		router->callback(request);
		return;
	}

	struct index *index = router->index;
	int result = dispatch_index_request(index, request);
	if (result == UDS_QUEUED) {
		// Take the request off the pipeline.
		return;
	}

	request->status = result;
	router->callback(request);
}
