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
 * $Id: //eng/uds-releases/gloria/src/uds/context.c#4 $
 */

#include "context.h"

#include "errors.h"
#include "featureDefs.h"
#include "grid.h"
#include "hashUtils.h"
#include "indexSession.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "parameter.h"
#include "permassert.h"
#include "threads.h"
#include "timeUtils.h"
#include "udsState.h"

/**********************************************************************/
int openContext(UdsIndexSession session, unsigned int *contextID)
{
  udsInitialize();

  lockGlobalStateMutex();
  int result = checkLibraryRunning();
  if (result != UDS_SUCCESS) {
    unlockGlobalStateMutex();
    return result;
  }

  // Hold a session group reference until the context is added, or fails.
  SessionGroup *contextGroup = getContextGroup();
  result = acquireSessionGroup(contextGroup);
  if (result != UDS_SUCCESS) {
    unlockGlobalStateMutex();
    return result;
  }

  // This index session reference is kept by the context object
  // and only released for an error return.
  IndexSession *indexSession;
  result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    releaseSessionGroup(contextGroup);
    unlockGlobalStateMutex();
    return result;
  }

  UdsContext *context = NULL;
  result = makeBaseContext(indexSession, &context);
  if (result != UDS_SUCCESS) {
    releaseSessionGroup(contextGroup);
    releaseIndexSession(indexSession);
    unlockGlobalStateMutex();
    return result;
  }
  // The non-null context now owns the indexSession reference.

  // Publish the new context in the context SessionGroup.
  result = initializeSession(contextGroup, &context->session,
                             (SessionContents) context, contextID);
  if (result != UDS_SUCCESS) {
    freeContext(context);
    releaseSessionGroup(contextGroup);
    unlockGlobalStateMutex();
    return result;
  }
  context->id = *contextID;
  logDebug("Opened context (%u)", *contextID);
  releaseBaseContext(context);
  releaseSessionGroup(contextGroup);
  unlockGlobalStateMutex();
  return UDS_SUCCESS;
}

/**********************************************************************/
static int checkContext(UdsContext *context)
{
  switch (context->contextState) {
    case UDS_CS_READY:
      return checkIndexSession(context->indexSession);
    case UDS_CS_DISABLED:
      return UDS_DISABLED;
    default:
      return UDS_NOCONTEXT;
  }
}

/**********************************************************************/
int getBaseContext(unsigned int contextId, UdsContext **contextPtr)
{
  Session *session;
  int result = getSession(getContextGroup(), contextId, &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  UdsContext *context = (UdsContext *) getSessionContents(session);
  result = checkContext(context);
  if (result != UDS_SUCCESS) {
    releaseSession(session);
    return result;
  }

  *contextPtr = context;
  return result;
}

/**********************************************************************/
void releaseBaseContext(UdsContext *context)
{
  releaseSession(&context->session);
}

/**********************************************************************/
int handleError(UdsContext *context, int errorCode)
{
  if (isUnrecoverable(errorCode)) {
    if (context != NULL) {
      context->contextState = UDS_CS_DISABLED;
      if (context->indexSession != NULL) {
        setIndexSessionState(context->indexSession, IS_DISABLED);
      }
    }
  }

  // Make sure the client never sees our internal code or attributes
  return sansUnrecoverable(errorCode);
}

/**********************************************************************/
int handleErrorAndReleaseBaseContext(UdsContext *context, int errorCode)
{
  int result = handleError(context, errorCode);
  releaseBaseContext(context);
  return sansUnrecoverable(result);
}

/**********************************************************************/
void freeContext(UdsContext *context)
{
  if (context == NULL) {
    return;
  }
  if (context->indexSession != NULL) {
    releaseIndexSession(context->indexSession);
  }
  FREE(context);
}

/**********************************************************************/
int closeContext(unsigned int contextId)
{
  SessionGroup *contextGroup = getContextGroup();
  int result = acquireSessionGroup(contextGroup);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Session *session;
  result = getSession(contextGroup, contextId, &session);
  if (result != UDS_SUCCESS) {
    releaseSessionGroup(contextGroup);
    return result;
  }

  UdsContext *context = (UdsContext *) getSessionContents(session);

  finishSession(contextGroup, &context->session);
  logDebug("Closed context (%u)", contextId);

  freeContext(context);
  releaseSessionGroup(contextGroup);
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeBaseContext(IndexSession *indexSession, UdsContext **contextPtr)
{
  UdsContext *context;
  int result = ALLOCATE(1, UdsContext, "empty context", &context);
  if (result != UDS_SUCCESS) {
    return result;
  }

  context->indexSession = indexSession;
  context->contextState = UDS_CS_READY;
  *contextPtr = context;
  return UDS_SUCCESS;
}

/**********************************************************************/
void flushBaseContext(UdsContext *context)
{
  waitForIdleSession(&context->session);
}

/**********************************************************************/
int flushContext(unsigned int contextId)
{
  UdsContext *context;
  int result = getBaseContext(contextId, &context);
  if (result != UDS_SUCCESS) {
    return result;
  }

  flushBaseContext(context);
  releaseBaseContext(context);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getContextIndexStats(unsigned int contextId, UdsIndexStats *stats)
{
  if (stats == NULL) {
    return logErrorWithStringError(UDS_INDEX_STATS_PTR_REQUIRED,
                                   "received a NULL index stats pointer");
  }

  UdsContext *context;
  int result = getBaseContext(contextId, &context);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "getBaseContext() failed.");
  }

  IndexRouterStatCounters routerStats;
  result = getGridStatistics(context->indexSession->grid, &routerStats);
  if (result != UDS_SUCCESS) {
    return handleErrorAndReleaseBaseContext(context, result);
  }

  stats->entriesIndexed   = routerStats.entriesIndexed;
  stats->memoryUsed       = routerStats.memoryUsed;
  stats->diskUsed         = routerStats.diskUsed;
  stats->collisions       = routerStats.collisions;
  stats->entriesDiscarded = routerStats.entriesDiscarded;
  stats->checkpoints      = routerStats.checkpoints;

  return handleErrorAndReleaseBaseContext(context, result);
}

/**********************************************************************/
int getContextStats(unsigned int contextId, UdsContextStats *stats)
{
  if (stats == NULL) {
    return
      logWarningWithStringError(UDS_CONTEXT_STATS_PTR_REQUIRED,
                                "received a NULL context stats pointer");
  }

  UdsContext *context;
  int         result = getBaseContext(contextId, &context);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result, "getBaseContext() failed");
  }

  // Send a synchronous control message to the callback thread to safely
  // gather the context statistics.
  Request *request;
  result = launchClientControlMessage(context, stats,
                                      REQUEST_COLLECT_CONTEXT_STATS,
                                      STAGE_CALLBACK, &request);
  if (result != UDS_SUCCESS) {
    return handleErrorAndReleaseBaseContext(context, result);
  }

  // The request directly updated stats, so there's no need to copy anything
  // else out of the request.
  result = request->status;
  freeRequest(request);
  return handleError(context, result);
}

/**********************************************************************/
static void collectStats(const UdsContext *context, UdsContextStats *stats)
{
  const SessionStats *sessionStats = &context->indexSession->stats;

  stats->currentTime = asTimeT(currentTime(CT_REALTIME));

  stats->postsFound         = sessionStats->postsFound;
  stats->inMemoryPostsFound = sessionStats->postsFoundOpenChapter;
  stats->densePostsFound    = sessionStats->postsFoundDense;
  stats->sparsePostsFound   = sessionStats->postsFoundSparse;
  stats->postsNotFound      = sessionStats->postsNotFound;
  stats->updatesFound       = sessionStats->updatesFound;
  stats->updatesNotFound    = sessionStats->updatesNotFound;
  stats->deletionsFound     = sessionStats->deletionsFound;
  stats->deletionsNotFound  = sessionStats->deletionsNotFound;
  stats->queriesFound       = sessionStats->queriesFound;
  stats->queriesNotFound    = sessionStats->queriesNotFound;
  stats->requests           = sessionStats->requests;
}

/**********************************************************************/
int dispatchContextControlRequest(Request *request)
{
  switch (request->action) {
  case REQUEST_COLLECT_CONTEXT_STATS:
    collectStats(request->context, (UdsContextStats *) request->controlData);
    return UDS_SUCCESS;

  default:
    return ASSERT_FALSE("unsupported context control action %d",
                        request->action);
  }
}
