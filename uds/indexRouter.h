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
 * $Id: //eng/uds-releases/krusty/src/uds/indexRouter.h#9 $
 */

#ifndef INDEX_ROUTER_H
#define INDEX_ROUTER_H

#include "compiler.h"
#include "index.h"
#include "indexSession.h"
#include "request.h"

/**
 * Callback after a query, update or remove request completes and fills in
 * select fields in the request: status for all requests, oldMetadata and
 * hashExists for query and update requests.
 *
 * @param request     request object.
 **/
typedef void (*IndexRouterCallback)(Request *request);

struct indexRouter {
  IndexRouterCallback  callback;
  unsigned int         zoneCount;
  bool                 needToSave;
  struct index        *index;
  RequestQueue        *triageQueue;
  RequestQueue        *zoneQueues[];
};

/**
 * Construct and initialize an IndexRouter instance.
 *
 * @param layout       the index_layout that describes the stored index
 * @param config       the configuration to use
 * @param userParams   the index session parameters.  If NULL, the default
 *                     session parameters will be used.
 * @param loadType     selects whether to create, load, or rebuild the index
 * @param loadContext  the index load context to use
 * @param callback     the function to invoke when a request completes or fails
 * @param routerPtr    a pointer in which to store the new router
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check makeIndexRouter(struct index_layout *layout,
				 const struct configuration *config,
				 const struct uds_parameters *userParams,
				 LoadType loadType,
				 IndexLoadContext *loadContext,
				 IndexRouterCallback callback,
				 IndexRouter **routerPtr);

/**
 * Executes the index operation for a UDS request and calls the callback upon
 * completion.
 *
 * @param router      The index router.
 * @param request     A pointer to the Request to process.
 **/
void executeIndexRouterRequest(IndexRouter *router, Request *request);

/**
 * Save the index router state to persistent storage.
 *
 * It is the responsibility of the caller to ensure that there are no other
 * uses of the index during a call to this method.  It is necessary that there
 * be no index requests from any block context nor any other attempt to save
 * the index until after a call to saveIndexRouter returns.
 *
 * @param router  the index router to save
 *
 * @return UDS_SUCCESS if successful.
 **/
int __must_check saveIndexRouter(IndexRouter *router);

/**
 * Destroy the index router and free its memory.
 *
 * @param router  the index router to destroy (may be NULL)
 **/
void freeIndexRouter(IndexRouter *router);

/**
 * Select and return the request queue responsible for executing the next
 * index stage of a request, updating the request with any associated state
 * (such as the zone number for UDS requests on a local index).
 *
 * @param router     The index router.
 * @param request    The Request destined for the queue.
 * @param nextStage  The next request stage (STAGE_TRIAGE or STAGE_INDEX).
 *
 * @return the next index stage queue (the local triage queue, local zone
 *         queue, or remote RPC send queue)
 **/
RequestQueue *selectIndexRouterQueue(IndexRouter  *router,
                                     Request      *request,
                                     RequestStage  nextStage);

/**
 * Wait for the index router to finish all operations that access a local
 * storage device.
 *
 * @param router    The index router.
 **/
static INLINE void waitForIdleIndexRouter(IndexRouter *router)
{
  wait_for_idle_chapter_writer(router->index->chapter_writer);
}

#endif /* INDEX_ROUTER_H */
