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
 * $Id: //eng/uds-releases/gloria/src/uds/indexSession.c#5 $
 */

#include "indexSession.h"

#include "grid.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "udsState.h"

/**********************************************************************/
static void handleCallbacks(Request *request)
{
  if (request->isControlMessage) {
    request->status = dispatchContextControlRequest(request);
    /*
     * This is a synchronous control request for collecting or resetting the
     * context statistics, so we use enterCallbackStage() to return the
     * request to the client thread even though this is the callback thread.
     */
    enterCallbackStage(request);
    return;
  }

  if (request->status == UDS_SUCCESS) {
    // Measure the turnaround time of this request and include that time,
    // along with the rest of the request, in the context's StatCounters.
    updateRequestContextStats(request);
  }

  if (request->callback != NULL) {
    // The request has specified its own callback and does not expect to be
    // freed, but free the serverContext that's hidden from the client.
    FREE(request->serverContext);
    request->serverContext = NULL;
    UdsContext *context = request->context;
    request->found = (request->location != LOC_UNAVAILABLE);
    request->callback((UdsRequest *) request);
    releaseBaseContext(context);
    return;
  }

  // Should not get here, because this is either a control message or it has a
  // callback method.
  freeRequest(request);
}

/**********************************************************************/
int checkIndexSession(IndexSession *indexSession)
{
  switch (getIndexSessionState(indexSession)) {
    case IS_READY:
      return UDS_SUCCESS;
    case IS_DISABLED:
      return UDS_DISABLED;
    case IS_INIT:
    default:
      return UDS_NO_INDEXSESSION;
  }
}

/**********************************************************************/
IndexSessionState getIndexSessionState(IndexSession *indexSession)
{
  return atomic_read_acquire(&indexSession->state);
}

/**********************************************************************/
void setIndexSessionState(IndexSession      *indexSession,
                          IndexSessionState  state)
{
  atomic_set_release(&indexSession->state, state);
}

/**********************************************************************/
int getIndexSession(unsigned int   indexSessionID,
                    IndexSession **indexSessionPtr)
{
  Session *session;
  int result = getSession(getIndexSessionGroup(), indexSessionID, &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexSession *indexSession = (IndexSession *) getSessionContents(session);
  result = checkIndexSession(indexSession);
  if (result != UDS_SUCCESS) {
    releaseSession(session);
    return result;
  }

  *indexSessionPtr = indexSession;
  return UDS_SUCCESS;
}

/**********************************************************************/
void releaseIndexSession(IndexSession *indexSession)
{
  releaseSession(&indexSession->session);
}

/**********************************************************************/
int makeEmptyIndexSession(IndexSession **indexSessionPtr)
{
  IndexSession *session;
  int result = ALLOCATE(1, IndexSession, "empty index session", &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeRequestQueue("callbackW", &handleCallbacks,
                            &session->callbackQueue);
  if (result != UDS_SUCCESS) {
    FREE(session);
    return result;
  }

  setIndexSessionState(session, IS_INIT);
  *indexSessionPtr = session;
  return UDS_SUCCESS;
}

/**********************************************************************/
int saveAndFreeIndexSession(IndexSession *indexSession)
{
  int result = saveAndFreeGrid(indexSession->grid, true);
  if (result != UDS_SUCCESS) {
    logInfoWithStringError(result, "ignoring error from saveAndFreeGrid");
  }
  requestQueueFinish(indexSession->callbackQueue);
  indexSession->callbackQueue = NULL;
  logDebug("Closed index session %u", indexSession->session.id);
  FREE(indexSession);
  return result;
}

/**********************************************************************/
int udsCloseIndexSession(UdsIndexSession session)
{
  SessionGroup *indexSessionGroup = getIndexSessionGroup();
  int result = acquireSessionGroup(indexSessionGroup);

  Session *baseSession;
  result = getSession(indexSessionGroup, session.id, &baseSession);
  if (result != UDS_SUCCESS) {
    releaseSessionGroup(indexSessionGroup);
    return result;
  }
  IndexSession *indexSession
    = (IndexSession *) getSessionContents(baseSession);

  logDebug("Closing index session %u", session.id);
  finishSession(indexSessionGroup, &indexSession->session);
  result = saveAndFreeIndexSession(indexSession);
  releaseSessionGroup(indexSessionGroup);
  return result;
}

/**********************************************************************/
int udsSetCheckpointFrequency(UdsIndexSession session, unsigned int frequency)
{
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = setGridCheckpointFrequency(indexSession->grid, frequency);
  releaseIndexSession(indexSession);
  return result;
}

/**********************************************************************/
int udsGetIndexConfiguration(UdsIndexSession session, UdsConfiguration *conf)
{
  if (conf == NULL) {
    return logErrorWithStringError(UDS_CONF_PTR_REQUIRED,
                                   "received a NULL config pointer");
  }
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ALLOCATE(1, struct udsConfiguration, __func__, conf);
  if (result == UDS_SUCCESS) {
    **conf = indexSession->grid->userConfig;
  }
  releaseIndexSession(indexSession);
  return result;
}

/**********************************************************************/
int udsGetIndexStats(UdsIndexSession session, UdsIndexStats *stats)
{
  if (stats == NULL) {
    return logErrorWithStringError(UDS_INDEX_STATS_PTR_REQUIRED,
                                   "received a NULL index stats pointer");
  }
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }
  IndexRouterStatCounters routerStats;
  result = getGridStatistics(indexSession->grid, &routerStats);
  releaseIndexSession(indexSession);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "%s failed", __func__);
  }
  stats->entriesIndexed   = routerStats.entriesIndexed;
  stats->memoryUsed       = routerStats.memoryUsed;
  stats->diskUsed         = routerStats.diskUsed;
  stats->collisions       = routerStats.collisions;
  stats->entriesDiscarded = routerStats.entriesDiscarded;
  stats->checkpoints      = routerStats.checkpoints;
  return UDS_SUCCESS;
}
