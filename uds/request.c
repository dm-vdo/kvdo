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
 * $Id: //eng/uds-releases/gloria/src/uds/request.c#3 $
 */

#include "request.h"

#include "featureDefs.h"
#include "grid.h"
#include "indexSession.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "parameter.h"
#include "permassert.h"
#include "udsState.h"

/**********************************************************************/
int launchAllocatedClientRequest(Request *request)
{
  int result = getBaseContext(request->blockContext.id, &request->context);
  if (result != UDS_SUCCESS) {
    return sansUnrecoverable(result);
  }

  request->action           = (RequestAction) request->type;
  request->isControlMessage = false;
  request->unbatched        = false;

  request->router = selectGridRouter(request->context->indexSession->grid,
                                     &request->hash);

  enqueueRequest(request, STAGE_TRIAGE);
  return UDS_SUCCESS;
}

#if GRID
/**********************************************************************/
int launchAIPControlMessage(AIPContext     *serverContext,
                            void           *controlData,
                            RequestAction   controlAction,
                            RequestStage    initialStage,
                            IndexRouter    *router,
                            Request       **requestPtr)
{
  Request *request;
  int result = ALLOCATE(1, Request, __func__, &request);
  if (result != UDS_SUCCESS) {
    FREE(serverContext);
    return result;
  }

  request->serverContext    = serverContext;
  request->router           = router;
  request->isControlMessage = true;
  request->unbatched        = true;
  request->action           = controlAction;
  request->controlData      = controlData;

  // Enqueue and return immediately for asynchronous requests.
  if (requestPtr == NULL) {
    enqueueRequest(request, initialStage);
    return UDS_SUCCESS;
  }

  // Initialize the synchronous notification fields in the request.
  SynchronousCallback synchronous;
  result = initializeSynchronousRequest(&synchronous);
  if (result != UDS_SUCCESS) {
    FREE(request);
    FREE(serverContext);
    return result;
  }
  request->synchronous = &synchronous;

  enqueueRequest(request, initialStage);
  awaitSynchronousRequest(request->synchronous);
  request->synchronous = NULL;

  *requestPtr = request;
  return result;
}
#endif /* GRID */

/**********************************************************************/
int launchClientControlMessage(UdsContext     *context,
                               void           *controlData,
                               RequestAction   controlAction,
                               RequestStage    initialStage,
                               Request       **requestPtr)
{
  Request *request;
  int result = ALLOCATE(1, Request, __func__, &request);
  if (result != UDS_SUCCESS) {
    return result;
  }

  request->context          = context;
  request->isControlMessage = true;
  request->unbatched        = true;
  request->action           = controlAction;
  request->controlData      = controlData;

  // Enqueue and return immediately for asynchronous requests.
  if (requestPtr == NULL) {
    enqueueRequest(request, initialStage);
    return UDS_SUCCESS;
  }

  // Initialize the synchronous notification fields in the request.
  SynchronousCallback synchronous;
  result = initializeSynchronousRequest(&synchronous);
  if (result != UDS_SUCCESS) {
    FREE(request);
    return result;
  }
  request->synchronous = &synchronous;

  enqueueRequest(request, initialStage);
  awaitSynchronousRequest(request->synchronous);
  request->synchronous = NULL;

  *requestPtr = request;
  return result;
}

/**********************************************************************/
int launchZoneControlMessage(RequestAction  action,
                             ZoneMessage    message,
                             unsigned int   zone,
                             IndexRouter   *router)
{
  Request *request;
  int result = ALLOCATE(1, Request, __func__, &request);
  if (result != UDS_SUCCESS) {
    return result;
  }

  request->router           = router;
  request->isControlMessage = true;
  request->unbatched        = true;
  request->action           = action;
  request->zoneNumber       = zone;
  request->zoneMessage      = message;

  enqueueRequest(request, STAGE_INDEX);
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeRequest(Request *request)
{
  if (request == NULL) {
    return;
  }

  // Capture fields from the request we need after it is freed.
  UdsContext *context = request->context;

  FREE(request->serverContext);
  FREE(request);
  request = NULL;

  if (context == NULL) {
    return;
  }

  // Release the counted reference to the context that was acquired for the
  // request (and not released) in createRequest().
  releaseBaseContext(context);
}

/**********************************************************************/
static RequestQueue *getNextStageQueue(Request      *request,
                                       RequestStage  nextStage)
{
  if (nextStage == STAGE_CALLBACK) {
    return request->context->indexSession->callbackQueue;
  }

  // Local and remote index routers handle the rest of the pipeline
  // differently, so delegate the choice of queue to the router.
  return request->router->methods->selectQueue(request->router, request,
                                               nextStage);
}

/**********************************************************************/
static void handleRequestErrors(Request *request)
{
  // XXX Use the router's callback function to hand back the error
  // and clean up the request? (Possible thread issues doing that.)

  // Awaken any synchronous request waiting for us.
  if (request->synchronous != NULL) {
    awakenSynchronousRequest(request->synchronous);
  } else {
    freeRequest(request);
  }
}

/**********************************************************************/
void enqueueRequest(Request *request, RequestStage nextStage)
{
  RequestQueue *nextQueue = getNextStageQueue(request, nextStage);
  if (nextQueue == NULL) {
    handleRequestErrors(request);
    return;
  }

  requestQueueEnqueue(nextQueue, request);
}

/*
 * This function pointer allows unit test code to intercept the slow-lane
 * requeuing of a request.
 */
static RequestRestarter requestRestarter = NULL;

/**********************************************************************/
void restartRequest(Request *request)
{
  request->requeued = true;
  if (requestRestarter == NULL) {
    enqueueRequest(request, STAGE_INDEX);
  } else {
    requestRestarter(request);
  }
}

/**********************************************************************/
void setRequestRestarter(RequestRestarter restarter)
{
  requestRestarter = restarter;
}

/**********************************************************************/
void updateRequestContextStats(Request *request)
{
  // We don't need any synchronization since the context stats are only
  // accessed from the single callback thread.

  SessionStats *sessionStats = &request->context->indexSession->stats;

  sessionStats->requests++;
  bool found = (request->location != LOC_UNAVAILABLE);

  switch (request->action) {
  case REQUEST_INDEX:
    if (found) {
      sessionStats->postsFound++;

      if (request->location == LOC_IN_OPEN_CHAPTER) {
        sessionStats->postsFoundOpenChapter++;
      } else if (request->location == LOC_IN_DENSE) {
        sessionStats->postsFoundDense++;
      } else if (request->location == LOC_IN_SPARSE) {
        sessionStats->postsFoundSparse++;
      }
    } else {
      sessionStats->postsNotFound++;
    }
    break;

  case REQUEST_UPDATE:
    if (found) {
      sessionStats->updatesFound++;
    } else {
      sessionStats->updatesNotFound++;
    }
    break;

  case REQUEST_DELETE:
    if (found) {
      sessionStats->deletionsFound++;
    } else {
      sessionStats->deletionsNotFound++;
    }
    break;

  case REQUEST_QUERY:
    if (found) {
      sessionStats->queriesFound++;
    } else {
      sessionStats->queriesNotFound++;
    }
    break;

  default:
    request->status = ASSERT(false, "unknown next action in request: %d",
                             request->action);
  }
}

/**********************************************************************/
void enterCallbackStage(Request *request)
{
  if (!request->isControlMessage) {
    // This is a client index request, all of which are now asynchronous.
    ASSERT_LOG_ONLY(request->synchronous == NULL, "asynchronous request");

    if (request->status != UDS_SUCCESS) {
      request->status = handleError(request->context, request->status);
    }

    // Handle asynchronous client callbacks in the designated thread.
    enqueueRequest(request, STAGE_CALLBACK);
  } else if (request->synchronous != NULL) {
    /*
     * Synchronous control messages require that we mark the callback
     * structure complete, transferring request ownership back to the waiting
     * thread (and waking it).
     */
    awakenSynchronousRequest(request->synchronous);
  } else {
    /*
     * Asynchronous control messages are complete when they are executed.
     * There should be nothing they need to do on the callback thread. The
     * message has been completely processed, so just free it.
     */
    freeRequest(request);
  }
}
