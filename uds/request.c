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
 * $Id: //eng/uds-releases/flanders/src/uds/request.c#6 $
 */

#include "request.h"

#include "featureDefs.h"
#include "grid.h"
#include "indexSession.h"
#include "isCallbackThreadDefs.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "parameter.h"
#include "permassert.h"
#include "requestLimit.h"
#include "udsState.h"

/**
 * Synchronizer for synchronous callbacks. Really just a SynchronizedBoolean.
 **/
struct synchronousCallback {
  Mutex   mutex;
  CondVar condition;
  bool    complete;
};

// ************** Start of request turnaround histogram code **************
#if HISTOGRAMS
#include "histogram.h"

Histogram *turnaroundTimeHistogram = NULL;
const char *turnaroundTimeHistogramName = NULL;

static void finishTurnaroundHistogram(void)
{
  plotHistogram(turnaroundTimeHistogramName, turnaroundTimeHistogram);
  freeHistogram(&turnaroundTimeHistogram);
}

void doTurnaroundHistogram(const char *name)
{
  int result = udsSetParameter(UDS_TIME_REQUEST_TURNAROUND, UDS_PARAM_TRUE);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot set %s to true",
                            UDS_TIME_REQUEST_TURNAROUND);
  }

  turnaroundTimeHistogramName = name;
  turnaroundTimeHistogram = makeLogarithmicHistogram("Turnaround Time", 7);
  atexit(finishTurnaroundHistogram);
}
#endif /* HISTOGRAMS */
// ************** End of request turnaround histogram code **************

/**
 * Perform a synchronous callback by marking the request/callback
 * as complete and waking any thread waiting for completion.
 **/
static void awakenSynchronousRequest(SynchronousCallback *synchronous)
{
  // Awaken any users of this synchronous request.
  lockMutex(&synchronous->mutex);
  synchronous->complete = true;
  // This MUST be inside the mutex to ensure the callback structure
  // is not destroyed before this thread has called broadcast.
  broadcastCond(&synchronous->condition);
  unlockMutex(&synchronous->mutex);
}

/**
 * Initialize a synchronous callback.
 **/
static int initializeSynchronousRequest(SynchronousCallback *synchronous)
{
  int result = initCond(&synchronous->condition);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initMutex(&synchronous->mutex);
  if (result != UDS_SUCCESS) {
    return result;
  }
  synchronous->complete = false;
  return UDS_SUCCESS;
}

/**
 * Wait for a synchronous callback by waiting until the request/callback has
 * been marked as complete, then destroy the contents of the callback.
 **/
static void awaitSynchronousRequest(SynchronousCallback *synchronous)
{
  lockMutex(&synchronous->mutex);
  while (!synchronous->complete) {
    waitCond(&synchronous->condition, &synchronous->mutex);
  }
  unlockMutex(&synchronous->mutex);

  // We're done, so destroy the contents of the callback structure.
  destroyCond(&synchronous->condition);
  destroyMutex(&synchronous->mutex);
}

/**********************************************************************/
int launchAllocatedClientRequest(Request *request)
{
  int result = getBaseContext(request->blockContext.id, &request->context);
  if (result != UDS_SUCCESS) {
    return sansUnrecoverable(result);
  }

  // Start the clock on the call, not the enqueueing.
  AbsTime initTime = ABSTIME_EPOCH;
  if (request->context->timeRequestTurnaround) {
    initTime = currentTime(CT_MONOTONIC);
  }

  request->action           = (RequestAction) request->type;
  request->fromCallback     = true;
  request->initTime         = initTime;
  request->isControlMessage = false;
  request->unbatched        = false;

  request->router = selectGridRouter(request->context->indexSession->grid,
                                     &request->hash);

  enqueueRequest(request, STAGE_TRIAGE);
  return UDS_SUCCESS;
}

/**********************************************************************/
int createRequest(unsigned int contextId, Request **requestPtr)
{
  if (requestPtr == NULL) {
    logWarningWithStringError(UDS_INVALID_ARGUMENT,
                              "cannot create uds request with NULL pointer");
    return UDS_INVALID_ARGUMENT;
  }

  UdsContext *context;
  int result = getBaseContext(contextId, &context);
  if (result != UDS_SUCCESS) {
    return sansUnrecoverable(result);
  }

  // Start the clock on the call, not the enqueueing.
  AbsTime initTime = ABSTIME_EPOCH;
  if (context->timeRequestTurnaround) {
    initTime = currentTime(CT_MONOTONIC);
  }

  // Limit the number of outstanding requests, but don't count the requests
  // issued by callbacks against the limit.
  bool onCallbackThread = isCallbackThread();
  if (!onCallbackThread) {
    borrowRequestPermit(context->requestLimit);
  }

  // Don't batch non-callback client requests if the request limit is one (for
  // clients simulating synchronous operation, like the replayer).
  bool unbatched = (getRequestPermitLimit(context->requestLimit) == 1);

  Request *request;
  result = ALLOCATE(1, Request, "request", &request);
  if (result != UDS_SUCCESS) {
    return handleErrorAndReleaseBaseContext(context, result);
  }

  // NOTE: this passes the session reference on to the request
  request->context          = context;
  request->fromCallback     = onCallbackThread;
  request->initTime         = initTime;
  request->update           = false;
  request->isControlMessage = false;
  request->unbatched        = unbatched;
  *requestPtr = request;
  return UDS_SUCCESS;
}

/**********************************************************************/
int launchClientRequest(unsigned int        contextId,
                        UdsCallbackType     callbackType,
                        bool                update,
                        const UdsChunkName *chunkName,
                        UdsCookie           cookie,
                        void               *metadata)
{
  if (chunkName == NULL) {
    return UDS_CHUNK_NAME_REQUIRED;
  }

  Request *request;
  int result = createRequest(contextId, &request);
  if (result != UDS_SUCCESS) {
    return result;
  }

  request->update     = update;
  request->cookie     = cookie;
  request->type       = callbackType;
  request->action     = (RequestAction) callbackType;

  // The chunk name is known so we can select a router to handle the request.
  request->hash   = *chunkName;
  request->router = selectGridRouter(request->context->indexSession->grid,
                                     &request->hash);

  if (metadata != NULL) {
    memcpy(request->newMetadata.data, metadata,
           request->context->metadataSize);
  }

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
  bool holdsPermit = (!request->fromCallback && !request->isControlMessage);

  FREE(request->serverContext);
  FREE(request);
  request = NULL;

  if (context == NULL) {
    return;
  }

  // If this request was counted against the request limit, now that the
  // request has been freed, it is time to return the request permit.
  if (holdsPermit) {
    returnRequestPermits(context->requestLimit, 1);
  }

  // Release the counted reference to the context that was acquired for the
  // request (and not released) in createRequest().
  releaseBaseContext(context);
}

/**
 * Calculate the turnaround time (in microseconds) for a request.
 *
 * @param request     The request being finished
 **/
static int64_t getTurnaroundTime(Request *request)
{
  if (!request->context->timeRequestTurnaround) {
    return 0;
  }

  AbsTime finishTime = currentTime(CT_MONOTONIC);

  /*
   * Note: In the current Linux kernels we're using, time can run
   * backwards!  (Presumably only when switching between cores or
   * CPUs.)  It happens even if you use
   * clock_gettime(CLOCK_MONOTONIC).  So we can't really sanity-check
   * very well, and we use signed values.
   *
   * Just use the value, and hope such errors cancel out somewhat over
   * time.  Of course, since our switches between processors aren't
   * necessarily random, that's a bit naive.
   *
   * TODO: We might do better mapping in /dev/hpet and reading HPET
   * timer values, but that's more work and less portable, and may
   * require supporting falling back to gettimeofday or clock_gettime.
   * Still, it may be worth exploring if we want better accuracy.
   */
  return relTimeToMicroseconds(timeDifference(finishTime, request->initTime));
}

/**********************************************************************/
static RequestQueue *getNextStageQueue(Request      *request,
                                       RequestStage  nextStage)
{
  if (nextStage == STAGE_CALLBACK) {
    return request->context->callbackQueue;
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
  int64_t turnaround = getTurnaroundTime(request);

  // We don't need any synchronization since the context stats are only
  // accessed from the single callback thread.

  UdsContext *context = request->context;
  StatCounters *counters = &context->stats.counters;

  /*
   * Unless we get a really slow processing rate and a really large
   * request pipeline size *and* a deployment operating at saturation
   * for many years, we shouldn't need to worry about this overflowing.
   */
  counters->requestTurnaroundTime += turnaround;
  if (turnaround > counters->maximumTurnaroundTime) {
    counters->maximumTurnaroundTime = turnaround;
  }
#if HISTOGRAMS
  if (turnaroundTimeHistogram != NULL) {
    enterHistogramSample(turnaroundTimeHistogram, turnaround);
  }
#endif /* HISTOGRAMS */

  counters->requests++;
  bool found = (request->location != LOC_UNAVAILABLE);

  switch (request->action) {
  case REQUEST_INDEX:
    if (found) {
      counters->postsFound++;

      if (request->location == LOC_IN_OPEN_CHAPTER) {
        counters->postsFoundOpenChapter++;
      } else if (request->location == LOC_IN_DENSE) {
        counters->postsFoundDense++;
      } else if (request->location == LOC_IN_SPARSE) {
        counters->postsFoundSparse++;
      }
    } else {
      counters->postsNotFound++;
    }
    break;

  case REQUEST_UPDATE:
    if (found) {
      counters->updatesFound++;
    } else {
      counters->updatesNotFound++;
    }
    break;

  case REQUEST_DELETE:
    if (found) {
      counters->deletionsFound++;
    } else {
      counters->deletionsNotFound++;
    }
    break;

  case REQUEST_QUERY:
    if (found) {
      counters->queriesFound++;
    } else {
      counters->queriesNotFound++;
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
