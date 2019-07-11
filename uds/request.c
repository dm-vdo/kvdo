/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/request.c#2 $
 */

#include "request.h"

#include "indexRouter.h"
#include "indexSession.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "parameter.h"
#include "permassert.h"
#include "requestQueue.h"
#include "udsState.h"

/**********************************************************************/
int udsStartChunkOperation(UdsRequest *udsRequest)
{
  if (udsRequest->callback == NULL) {
    return UDS_CALLBACK_REQUIRED;
  }
  switch (udsRequest->type) {
  case UDS_DELETE:
  case UDS_POST:
  case UDS_QUERY:
  case UDS_UPDATE:
    break;
  default:
    return UDS_INVALID_OPERATION_TYPE;
  }
  memset(udsRequest->private, 0, sizeof(udsRequest->private));
  Request *request = (Request *)udsRequest;

  int result = getIndexSession(request->session);
  if (result != UDS_SUCCESS) {
    return sansUnrecoverable(result);
  }

  request->found            = false;
  request->action           = (RequestAction) request->type;
  request->indexSession     = request->session;
  request->isControlMessage = false;
  request->unbatched        = false;
  request->router           = request->indexSession->router;

  enqueueRequest(request, STAGE_TRIAGE);
  return UDS_SUCCESS;
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
  if (request != NULL) {
    FREE(request);
  }
}

/**********************************************************************/
static RequestQueue *getNextStageQueue(Request      *request,
                                       RequestStage  nextStage)
{
  if (nextStage == STAGE_CALLBACK) {
    return request->indexSession->callbackQueue;
  }

  // Local and remote index routers handle the rest of the pipeline
  // differently, so delegate the choice of queue to the router.
  return selectIndexRouterQueue(request->router, request, nextStage);
}

/**********************************************************************/
static void handleRequestErrors(Request *request)
{
  // XXX Use the router's callback function to hand back the error
  // and clean up the request? (Possible thread issues doing that.)

  freeRequest(request);
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
static INLINE void increment_once(uint64_t *countPtr)
{
  WRITE_ONCE(*countPtr, READ_ONCE(*countPtr) + 1);
}

/**********************************************************************/
void updateRequestContextStats(Request *request)
{
  /*
   * We don't need any synchronization since the context stats are only
   *  modified from the single callback thread.
   *
   * We increment either 2 or 3 counters in this method.
   *
   * XXX We always increment the "requests" counter.  But there is no code
   *     that uses the value stored in this counter.
   *
   * We always increment exactly one of these counters (unless there is an
   * error in the code, which never happens):
   *     postsFound      postsNotFound
   *     updatesFound    updatesNotFound
   *     deletionsFound  deletionsNotFound
   *     queriesFound    queriesNotFound
   *
   * XXX In the case of post request that were found in the index, we increment
   *     exactly one of these counters.  But there is no code that uses the
   *     value stored in these counters.
   *          inMemoryPostsFound
   *          densePostsFound
   *          sparsePostsFound
   */

  SessionStats *sessionStats = &request->indexSession->stats;

  increment_once(&sessionStats->requests);
  bool found = (request->location != LOC_UNAVAILABLE);

  switch (request->action) {
  case REQUEST_INDEX:
    if (found) {
      increment_once(&sessionStats->postsFound);

      if (request->location == LOC_IN_OPEN_CHAPTER) {
        increment_once(&sessionStats->postsFoundOpenChapter);
      } else if (request->location == LOC_IN_DENSE) {
        increment_once(&sessionStats->postsFoundDense);
      } else if (request->location == LOC_IN_SPARSE) {
        increment_once(&sessionStats->postsFoundSparse);
      }
    } else {
      increment_once(&sessionStats->postsNotFound);
    }
    break;

  case REQUEST_UPDATE:
    if (found) {
      increment_once(&sessionStats->updatesFound);
    } else {
      increment_once(&sessionStats->updatesNotFound);
    }
    break;

  case REQUEST_DELETE:
    if (found) {
      increment_once(&sessionStats->deletionsFound);
    } else {
      increment_once(&sessionStats->deletionsNotFound);
    }
    break;

  case REQUEST_QUERY:
    if (found) {
      increment_once(&sessionStats->queriesFound);
    } else {
      increment_once(&sessionStats->queriesNotFound);
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
    if (isUnrecoverable(request->status)) {
      // Unrecoverable errors must disable the index session
      setIndexSessionState(request->indexSession, IS_DISABLED);
      // The unrecoverable state is internal and must not sent to the client.
      request->status = sansUnrecoverable(request->status);
    }

    // Handle asynchronous client callbacks in the designated thread.
    enqueueRequest(request, STAGE_CALLBACK);
  } else {
    /*
     * Asynchronous control messages are complete when they are executed.
     * There should be nothing they need to do on the callback thread. The
     * message has been completely processed, so just free it.
     */
    freeRequest(request);
  }
}
