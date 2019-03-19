/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/indexRouter.h#3 $
 */

#ifndef INDEX_ROUTER_H
#define INDEX_ROUTER_H

#include "compiler.h"
#include "indexRouterStats.h"
#include "request.h"

/**
 * Callback after a query, update or remove request completes and fills in
 * select fields in the request: status for all requests, oldMetadata and
 * hashExists for query and update requests.
 *
 * @param request     request object.
 **/
typedef void (*IndexRouterCallback)(Request *request);

/**
 * Forward declaration of all the index router function hooks. The struct is
 * declared lower down in this file due to its length.
 **/
typedef struct indexRouterMethods IndexRouterMethods;

/**
 * The header structure contain fields common to the all the index router
 * implementations.
 **/
struct indexRouter {
  const IndexRouterMethods *methods;
  IndexRouterCallback       callback;
};

/**
 * Index router methods as a function table in IndexRouter (see common.h).
 **/
struct indexRouterMethods {
  /**
   * Optionally save the index router state to persistent storage, and always
   * destroy the index router and free its memory.
   *
   * @param router    The index router to save.
   * @param saveFlag  True to save the index router state.
   *
   * @return        UDS_SUCCESS if successful.
   **/
  int (*saveAndFree)(IndexRouter *router, bool saveFlag);

  /**
   * Select and return the request queue responsible for executing the next
   * index stage of a request, updating the request with any associated state
   * (such as the zone number for UDS requests on a local index).
   *
   * @param router      The index router.
   * @param request     The Request destined for the queue.
   * @param nextStage   The next request stage (STAGE_TRIAGE or STAGE_INDEX).
   *
   * @return the next index stage queue (the local triage queue, local zone
   *         queue, or remote RPC send queue)
   **/
  RequestQueue *(*selectQueue)(IndexRouter  *router,
                               Request      *request,
                               RequestStage  nextStage);

  /**
   * Executes the index operation for a UDS request and calls the callback upon
   * completion.
   *
   * @param router      The index router.
   * @param request     A pointer to the Request to process.
   **/
  void (*execute)(IndexRouter *router, Request *request);

  /**
   * Gather router usage counters.
   *
   * @param router    The index router.
   * @param counters  The statistics structure to fill.
   *
   * @return          UDS_SUCCESS or error code
   **/
  int (*getStatistics)(IndexRouter *router, IndexRouterStatCounters *counters);

  /**
   * Change the checkpoint frequency.
   *
   * @param router    The index router.
   * @param frequency The new checkpointing frequency.
   **/
  void (*setCheckpointFrequency)(IndexRouter *router, unsigned int frequency);
};

/**
 * Optionally save the index router state to persistent storage, and always
 * destroy the index router and free its memory.
 *
 * @param router    the index router to destroy (may be NULL)
 * @param saveFlag  True to save the index router state.
 *
 * @return        UDS_SUCCESS if successful.
 **/
static INLINE int saveAndFreeIndexRouter(IndexRouter *router, bool saveFlag)
{
  return router->methods->saveAndFree(router, saveFlag);
}

/**
 * Destroy an index router and free its memory.
 *
 * @param router  the index router to destroy (may be NULL)
 **/
static INLINE void freeIndexRouter(IndexRouter *router)
{
  if (router != NULL) {
    saveAndFreeIndexRouter(router, false);
  }
}

#endif /* INDEX_ROUTER_H */
