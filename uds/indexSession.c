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
 * $Id: //eng/uds-releases/jasper/src/uds/indexSession.c#1 $
 */

#include "indexSession.h"

#include "indexCheckpoint.h"
#include "indexRouter.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "requestQueue.h"
#include "udsState.h"

/**********************************************************************/
static void collectStats(const IndexSession *indexSession,
                         UdsContextStats    *stats)
{
  const SessionStats *sessionStats = &indexSession->stats;

  stats->currentTime = asTimeT(currentTime(CT_REALTIME));

  stats->postsFound         = READ_ONCE(sessionStats->postsFound);
  stats->inMemoryPostsFound = READ_ONCE(sessionStats->postsFoundOpenChapter);
  stats->densePostsFound    = READ_ONCE(sessionStats->postsFoundDense);
  stats->sparsePostsFound   = READ_ONCE(sessionStats->postsFoundSparse);
  stats->postsNotFound      = READ_ONCE(sessionStats->postsNotFound);
  stats->updatesFound       = READ_ONCE(sessionStats->updatesFound);
  stats->updatesNotFound    = READ_ONCE(sessionStats->updatesNotFound);
  stats->deletionsFound     = READ_ONCE(sessionStats->deletionsFound);
  stats->deletionsNotFound  = READ_ONCE(sessionStats->deletionsNotFound);
  stats->queriesFound       = READ_ONCE(sessionStats->queriesFound);
  stats->queriesNotFound    = READ_ONCE(sessionStats->queriesNotFound);
  stats->requests           = READ_ONCE(sessionStats->requests);
}

/**********************************************************************/
static void handleCallbacks(Request *request)
{
  if (request->status == UDS_SUCCESS) {
    // Measure the turnaround time of this request and include that time,
    // along with the rest of the request, in the context's StatCounters.
    updateRequestContextStats(request);
  }

  if (request->callback != NULL) {
    // The request has specified its own callback and does not expect to be
    // freed.
    IndexSession *indexSession = request->indexSession;
    request->found = (request->location != LOC_UNAVAILABLE);
    request->callback((UdsRequest *) request);
    // We do this release after the callback because of the contract of the
    // udsFlushIndexSession method.
    releaseIndexSession(indexSession);
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
  int result = UDS_SUCCESS;
  IndexRouter *router = indexSession->router;
  if (router != NULL) {
    result = saveIndexRouter(router);
    if (result != UDS_SUCCESS) {
      logWarningWithStringError(result, "ignoring error from saveIndexRouter");
    }
    freeIndexRouter(router);
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
int udsFlushIndexSession(UdsIndexSession session)
{
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Wait until there are no index requests in progress
  waitForIdleSession(&indexSession->session);
  // Wait until any open chapter writes are complete
  waitForIdleIndexRouter(indexSession->router);

  releaseIndexSession(indexSession);
  return result;
}

/**********************************************************************/
int udsSaveIndex(UdsIndexSession session)
{
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Wait until there are no index requests in progress
  waitForIdleSession(&indexSession->session);
  // saveIndexRouter waits for open chapter writes to complete
  result = saveIndexRouter(indexSession->router);
  releaseIndexSession(indexSession);
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
  setIndexCheckpointFrequency(indexSession->router->index->checkpoint,
                              frequency);
  releaseIndexSession(indexSession);
  return UDS_SUCCESS;
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
    **conf = indexSession->userConfig;
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
  getIndexStats(indexSession->router->index, stats);
  releaseIndexSession(indexSession);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsGetIndexSessionStats(UdsIndexSession session, UdsContextStats *stats)
{
  if (stats == NULL) {
    return logWarningWithStringError(UDS_CONTEXT_STATS_PTR_REQUIRED,
                                     "received a NULL context stats pointer");
  }
  IndexSession *indexSession;
  int result = getIndexSession(session.id, &indexSession);
  if (result != UDS_SUCCESS) {
    return result;
  }
  collectStats(indexSession, stats);
  releaseIndexSession(indexSession);
  return UDS_SUCCESS;


}
