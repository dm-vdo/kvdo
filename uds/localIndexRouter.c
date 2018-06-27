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
 * $Id: //eng/uds-releases/gloria/src/uds/localIndexRouter.c#5 $
 */

#include "localIndexRouter.h"

#include "compiler.h"
#include "indexCheckpoint.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "requestQueue.h"
#include "zone.h"

static int saveAndFreeLocalIndexRouter(IndexRouter *header, bool saveFlag);
static RequestQueue *selectIndexRouterQueue(IndexRouter  *header,
                                            Request      *request,
                                            RequestStage  nextStage);
static void executeIndexRouterRequest(IndexRouter *header, Request *request);
static int getRouterStatistics(IndexRouter *header,
                               IndexRouterStatCounters *counters);
static void setCheckpointFrequency(IndexRouter *header, unsigned int frequency);

static const IndexRouterMethods methods = {
  .saveAndFree            = saveAndFreeLocalIndexRouter,
  .selectQueue            = selectIndexRouterQueue,
  .execute                = executeIndexRouterRequest,
  .getStatistics          = getRouterStatistics,
  .setCheckpointFrequency = setCheckpointFrequency,
};

/*
 * Convert a LocalIndexRouter pointer to the public IndexRouter pointer.
 */
static INLINE IndexRouter *asIndexRouter(LocalIndexRouter *router)
{
  return &router->header;
}

/**
 * This is the request processing function invoked by the zone's RequestQueue
 * worker thread.
 *
 * @param request  the request to be indexed or executed by the zone worker
 **/
static void executeZoneRequest(Request *request)
{
  request->router->methods->execute(request->router, request);
}

/**
 * Construct and enqueue asynchronous control messages to add the chapter
 * index for a given virtual chapter to the sparse chapter index cache.
 *
 * @param router          the router containing the relevant queues
 * @param index           the index with the relevant cache and chapter
 * @param virtualChapter  the virtual chapter number of the chapter to cache
 **/
static void enqueueBarrierMessages(LocalIndexRouter *router,
                                   Index            *index,
                                   uint64_t          virtualChapter)
{
  ZoneMessage barrier = {
    .index = index,
    .data = {
      .barrier = {
        .virtualChapter = virtualChapter,
      }
    }
  };
  for (unsigned int zone = 0; zone < router->zoneCount; zone++) {
    int result = launchZoneControlMessage(REQUEST_SPARSE_CACHE_BARRIER,
                                          barrier, zone,
                                          asIndexRouter(router));
    ASSERT_LOG_ONLY((result == UDS_SUCCESS), "barrier message allocation");
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
static void triageRequest(Request *request)
{
  LocalIndexRouter *router = asLocalIndexRouter(request->router);
  Index *index = router->index;

  // Check if the name is a hook in the index pointing at a sparse chapter.
  uint64_t sparseVirtualChapter = triageIndexRequest(index, request);
  if (sparseVirtualChapter != UINT64_MAX) {
    // Generate and place a barrier request on every zone queue.
    enqueueBarrierMessages(router, index, sparseVirtualChapter);
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
static int initializeLocalIndexQueues(LocalIndexRouter *router,
                                      const Geometry   *geometry)
{
  for (unsigned int i = 0; i < router->zoneCount; i++) {
    int result = makeRequestQueue("indexW", &executeZoneRequest,
                              &router->zoneQueues[i]);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  // The triage queue is only needed for sparse multi-zone indexes.
  if ((router->zoneCount > 1) && isSparse(geometry)) {
    int result = makeRequestQueue("triageW", &triageRequest,
                                  &router->triageQueue);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
static INLINE RequestQueue *getZoneQueue(LocalIndexRouter *router,
                                         unsigned int zoneNumber)
{
  return router->zoneQueues[zoneNumber];
}

/**********************************************************************/
int makeLocalIndexRouter(IndexLayout          *layout,
                         const Configuration  *config,
                         LoadType              loadType,
                         IndexRouterCallback   callback,
                         IndexRouter         **newRouter)
{
  unsigned int zoneCount = getZoneCount();
  LocalIndexRouter *router;
  int result = ALLOCATE_EXTENDED(LocalIndexRouter, zoneCount, RequestQueue *,
                                 "index router", &router);
  if (result != UDS_SUCCESS) {
    return result;
  }

  router->header.methods  = &methods;
  router->header.callback = callback;
  router->zoneCount       = zoneCount;

  result = initializeLocalIndexQueues(router, config->geometry);
  if (result != UDS_SUCCESS) {
    freeIndexRouter(asIndexRouter(router));
    return result;
  }

  result = makeIndex(layout, config, router->zoneCount, loadType,
                     &router->index);
  if (result != UDS_SUCCESS) {
    freeIndexRouter(asIndexRouter(router));
    return logErrorWithStringError(result, "failed to create index");
  }

  *newRouter = asIndexRouter(router);
  return UDS_SUCCESS;
}

/**********************************************************************/
static int saveAndFreeLocalIndexRouter(IndexRouter *header, bool saveFlag)
{
  LocalIndexRouter *router = asLocalIndexRouter(header);
  if (router == NULL) {
    return UDS_SUCCESS;
  }
  requestQueueFinish(router->triageQueue);
  for (unsigned int i = 0; i < router->zoneCount; i++) {
    requestQueueFinish(router->zoneQueues[i]);
  }

  int result = UDS_SUCCESS;
  if (saveFlag) {
    result = saveIndex(router->index);
  }

  freeIndex(router->index);
  FREE(router);
  return result;
}

/**********************************************************************/
static RequestQueue *selectIndexRouterQueue(IndexRouter  *header,
                                            Request      *request,
                                            RequestStage  nextStage)
{
  LocalIndexRouter *router = asLocalIndexRouter(header);

  if (request->isControlMessage) {
    return getZoneQueue(router, request->zoneNumber);
  }

  if (nextStage == STAGE_TRIAGE) {
    // The triage queue is only needed for multi-zone sparse indexes and won't
    // be allocated by the router if not needed, so simply check for NULL.
    if (router->triageQueue != NULL) {
      return router->triageQueue;
    }
    // Dense index or single zone, so route it directly to the zone queue.
  } else if (nextStage != STAGE_INDEX) {
    ASSERT_LOG_ONLY(false, "invalid index stage: %d", nextStage);
    return NULL;
  }

  Index *index = router->index;
  request->zoneNumber = getMasterIndexZone(index->masterIndex, &request->hash);
  return getZoneQueue(router, request->zoneNumber);
}

/**********************************************************************/
static void executeIndexRouterRequest(IndexRouter *header, Request *request)
{
  LocalIndexRouter *router = asLocalIndexRouter(header);

  if (request->isControlMessage) {
    int result = dispatchIndexZoneControlRequest(request);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "error executing control message: %d",
                              request->action);
    }
    request->status = result;
    enterCallbackStage(request);
    return;
  }

  if (request->requeued &&
      (request->status != UDS_SUCCESS) &&
      (request->status != UDS_QUEUED)) {
    request->status = makeUnrecoverable(request->status);
    router->header.callback(request);
    return;
  }

  Index *index = router->index;
  int result = dispatchIndexRequest(index, request);
  if (result == UDS_QUEUED) {
    // Take the request off the pipeline.
    return;
  }

  request->status = result;
  router->header.callback(request);
}

/**********************************************************************/
static int getRouterStatistics(IndexRouter             *header,
                               IndexRouterStatCounters *counters)
{
  LocalIndexRouter *router = asLocalIndexRouter(header);
  getIndexStats(router->index, counters);
  return UDS_SUCCESS;
}

/**********************************************************************/
static void setCheckpointFrequency(IndexRouter *header, unsigned int frequency)
{
  LocalIndexRouter *router = asLocalIndexRouter(header);
  setIndexCheckpointFrequency(router->index->checkpoint, frequency);
}
