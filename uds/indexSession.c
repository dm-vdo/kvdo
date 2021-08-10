/*
 * %Copyright%
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
 * $Id: //eng/uds-releases/jasper/src/uds/indexSession.c#11 $
 */

#include "indexSession.h"

#include "indexCheckpoint.h"
#include "indexRouter.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "requestQueue.h"

/**********************************************************************/
static void collectStats(const struct uds_index_session *indexSession,
                         UdsContextStats                *stats)
{
  const SessionStats *sessionStats = &indexSession->stats;

  stats->currentTime = asTimeT(currentTime(CLOCK_REALTIME));

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
    struct uds_index_session *indexSession = request->session;
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
int checkIndexSession(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);
  unsigned int state = indexSession->state;
  unlockMutex(&indexSession->requestMutex);

  if (state == IS_FLAG_LOADED) {
    return UDS_SUCCESS;
  } else if (state & IS_FLAG_DISABLED) {
    return UDS_DISABLED;
  } else if ((state & IS_FLAG_LOADING)
             || (state & IS_FLAG_SUSPENDED)
             || (state & IS_FLAG_WAITING)) {
    return UDS_SUSPENDED;
  }

   return UDS_NO_INDEXSESSION;
}

/**********************************************************************/
int getIndexSession(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);
  indexSession->requestCount++;
  unlockMutex(&indexSession->requestMutex);

  int result = checkIndexSession(indexSession);
  if (result != UDS_SUCCESS) {
    releaseIndexSession(indexSession);
    return result;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
void releaseIndexSession(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);
  if (--indexSession->requestCount == 0) {
    broadcastCond(&indexSession->requestCond);
  }
  unlockMutex(&indexSession->requestMutex);
}

/**********************************************************************/
int startLoadingIndexSession(struct uds_index_session *indexSession)
{
  int result;
  lockMutex(&indexSession->requestMutex);
  if (indexSession->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUSPENDED;
  } else if (indexSession->state != 0) {
    result = UDS_INDEXSESSION_IN_USE;
  } else {
    indexSession->state |= IS_FLAG_LOADING;
    result = UDS_SUCCESS;
  }
  unlockMutex(&indexSession->requestMutex);
  return result;
}

/**********************************************************************/
void finishLoadingIndexSession(struct uds_index_session *indexSession,
                               int                       result)
{
  lockMutex(&indexSession->requestMutex);
  indexSession->state &= ~IS_FLAG_LOADING;
  if (result == UDS_SUCCESS) {
    indexSession->state |= IS_FLAG_LOADED;
  }
  broadcastCond(&indexSession->requestCond);
  unlockMutex(&indexSession->requestMutex);
}

/**********************************************************************/
void disableIndexSession(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);
  indexSession->state |= IS_FLAG_DISABLED;
  unlockMutex(&indexSession->requestMutex);
}

/**********************************************************************/
int makeEmptyIndexSession(struct uds_index_session **indexSessionPtr)
{
  struct uds_index_session *session;
  int result = ALLOCATE(1, struct uds_index_session, __func__, &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initMutex(&session->requestMutex);
  if (result != UDS_SUCCESS) {
    FREE(session);
    return result;
  }

  result = initCond(&session->requestCond);
  if (result != UDS_SUCCESS) {
    destroyMutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = initMutex(&session->loadContext.mutex);
  if (result != UDS_SUCCESS) {
    destroyCond(&session->requestCond);
    destroyMutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = initCond(&session->loadContext.cond);
  if (result != UDS_SUCCESS) {
    destroyMutex(&session->loadContext.mutex);
    destroyCond(&session->requestCond);
    destroyMutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = makeRequestQueue("callbackW", &handleCallbacks,
                            &session->callbackQueue);
  if (result != UDS_SUCCESS) {
    destroyCond(&session->loadContext.cond);
    destroyMutex(&session->loadContext.mutex);
    destroyCond(&session->requestCond);
    destroyMutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  *indexSessionPtr = session;
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsSuspendIndexSession(struct uds_index_session *session, bool save)
{
  int result;
  bool flushIndex = false;
  bool saveIndex = false;
  bool suspendIndex = false;
  lockMutex(&session->requestMutex);
  // Wait for any pending close operation to complete.
  while (session->state & IS_FLAG_CLOSING) {
    waitCond(&session->requestCond, &session->requestMutex);
  }
  if ((session->state & IS_FLAG_WAITING)
      || (session->state & IS_FLAG_DESTROYING)) {
    result = EBUSY;
  } else if (session->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUCCESS;
  } else if (session->state & IS_FLAG_LOADING) {
    session->state |= IS_FLAG_WAITING;
    suspendIndex = true;
    result = UDS_SUCCESS;
  } else if (!(session->state & IS_FLAG_LOADED)) {
    if (session->router != NULL) { 
      flushIndex = true;
      session->state |= IS_FLAG_WAITING;
    } else {
      session->state |= IS_FLAG_SUSPENDED;
      broadcastCond(&session->requestCond);
    }
    result = UDS_SUCCESS;
  } else {
    saveIndex = save;
    if (saveIndex) {
      session->state |= IS_FLAG_WAITING;
    } else if (session->router != NULL) { 
      flushIndex = true;
      session->state |= IS_FLAG_WAITING;
    } else {
      session->state |= IS_FLAG_SUSPENDED;
      broadcastCond(&session->requestCond);
    }
    result = UDS_SUCCESS;
  }
  unlockMutex(&session->requestMutex);

  if (!saveIndex && !suspendIndex && !flushIndex) {
    return result;
  }

  if (flushIndex) {
    result = udsFlushIndexSession(session);
    lockMutex(&session->requestMutex);
    session->state &= ~IS_FLAG_WAITING;
    session->state |= IS_FLAG_SUSPENDED;
    broadcastCond(&session->requestCond);
    unlockMutex(&session->requestMutex);
    return result;
  }

  if (saveIndex) {
    result = udsSaveIndex(session);
    lockMutex(&session->requestMutex);
    session->state &= ~IS_FLAG_WAITING;
    session->state |= IS_FLAG_SUSPENDED;
    broadcastCond(&session->requestCond);
    unlockMutex(&session->requestMutex);
    return result;
  }

  lockMutex(&session->loadContext.mutex);
  switch (session->loadContext.status) {
  case INDEX_OPENING:
    session->loadContext.status = INDEX_SUSPENDING;

    // Wait until the index indicates that it is not replaying.
    while ((session->loadContext.status != INDEX_SUSPENDED)
           && (session->loadContext.status != INDEX_READY)) {
      waitCond(&session->loadContext.cond,
               &session->loadContext.mutex);
    }
    break;

  case INDEX_READY:
    // Index load does not need to be suspended.
    break;

  case INDEX_SUSPENDED:
  case INDEX_SUSPENDING:
  case INDEX_FREEING:
  default:
    // These cases should not happen.
    ASSERT_LOG_ONLY(false, "Bad load context state %u",
                    session->loadContext.status);
    break;
  }
  unlockMutex(&session->loadContext.mutex);

  lockMutex(&session->requestMutex);
  session->state &= ~IS_FLAG_WAITING;
  session->state |= IS_FLAG_SUSPENDED;
  broadcastCond(&session->requestCond);
  unlockMutex(&session->requestMutex);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsResumeIndexSession(struct uds_index_session *session)
{
  lockMutex(&session->requestMutex);
  if (session->state & IS_FLAG_WAITING) {
    unlockMutex(&session->requestMutex);
    return EBUSY;
  }

  /* If not suspended, just succeed */
  if (!(session->state & IS_FLAG_SUSPENDED)) {
    unlockMutex(&session->requestMutex);
    return UDS_SUCCESS;
  }

  if (!(session->state & IS_FLAG_LOADING)) {
    session->state &= ~IS_FLAG_SUSPENDED;
    unlockMutex(&session->requestMutex);
    return UDS_SUCCESS;
  }

  session->state |= IS_FLAG_WAITING;
  unlockMutex(&session->requestMutex);

  lockMutex(&session->loadContext.mutex);
  switch (session->loadContext.status) {
  case INDEX_SUSPENDED:
    session->loadContext.status = INDEX_OPENING;
    // Notify the index to start replaying again.
    broadcastCond(&session->loadContext.cond);
    break;

  case INDEX_READY:
    // There is no index rebuild to resume.
    break;

  case INDEX_OPENING:
  case INDEX_SUSPENDING:
  case INDEX_FREEING:
  default:
    // These cases should not happen; do nothing.
    ASSERT_LOG_ONLY(false, "Bad load context state %u",
                    session->loadContext.status);
    break;
  }
  unlockMutex(&session->loadContext.mutex);

  lockMutex(&session->requestMutex);
  session->state &= ~IS_FLAG_WAITING;
  session->state &= ~IS_FLAG_SUSPENDED;
  broadcastCond(&session->requestCond);
  unlockMutex(&session->requestMutex);
  return UDS_SUCCESS;
}

/**********************************************************************/
static void waitForNoRequestsInProgress(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);
  while (indexSession->requestCount > 0) {
    waitCond(&indexSession->requestCond, &indexSession->requestMutex);
  }
  unlockMutex(&indexSession->requestMutex);
}

/**********************************************************************/
int saveAndFreeIndex(struct uds_index_session *indexSession)
{
  int result = UDS_SUCCESS;
  IndexRouter *router = indexSession->router;
  if (router != NULL) {
    lockMutex(&indexSession->requestMutex);
    bool suspended = (indexSession->state & IS_FLAG_SUSPENDED);
    unlockMutex(&indexSession->requestMutex);
    if (!suspended) {
      result = saveIndexRouter(router);
      if (result != UDS_SUCCESS) {
        logWarningWithStringError(result, "ignoring error from saveIndexRouter");
      }
    }
    freeIndexRouter(router);
    indexSession->router = NULL;

    // Reset all index state that happens to be in the index session, so it
    // doesn't affect any future index.
    lockMutex(&indexSession->loadContext.mutex);
    indexSession->loadContext.status = INDEX_OPENING;
    unlockMutex(&indexSession->loadContext.mutex);

    lockMutex(&indexSession->requestMutex);
    // Only the suspend bit will remain relevant.
    indexSession->state &= IS_FLAG_SUSPENDED;
    unlockMutex(&indexSession->requestMutex);
  }

  logDebug("Closed index");
  return result;
}

/**********************************************************************/
int udsCloseIndex(struct uds_index_session *indexSession)
{
  lockMutex(&indexSession->requestMutex);

  // Wait for any pending suspend, resume or close operations to complete.
  while ((indexSession->state & IS_FLAG_WAITING)
         || (indexSession->state & IS_FLAG_CLOSING)) {
    waitCond(&indexSession->requestCond, &indexSession->requestMutex);
  }

  int result = UDS_SUCCESS;
  if (indexSession->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUSPENDED;
  } else if ((indexSession->state & IS_FLAG_DESTROYING)
             || !(indexSession->state & IS_FLAG_LOADED)) {
    // The index doesn't exist, hasn't finished loading, or is being destroyed.
    result = UDS_NO_INDEXSESSION;
  } else {
    indexSession->state |= IS_FLAG_CLOSING;
  }
  unlockMutex(&indexSession->requestMutex);
  if (result != UDS_SUCCESS) {
    return result;
  }

  logDebug("Closing index");
  waitForNoRequestsInProgress(indexSession);
  result = saveAndFreeIndex(indexSession);

  lockMutex(&indexSession->requestMutex);
  indexSession->state &= ~IS_FLAG_CLOSING;
  broadcastCond(&indexSession->requestCond);
  unlockMutex(&indexSession->requestMutex);
  return result;
}

/**********************************************************************/
int udsDestroyIndexSession(struct uds_index_session *indexSession)
{
  logDebug("Destroying index session");

  bool loadPending = false;
  lockMutex(&indexSession->requestMutex);

  // Wait for any pending suspend, resume, or close operations to complete.
  while ((indexSession->state & IS_FLAG_WAITING)
         || (indexSession->state & IS_FLAG_CLOSING)) {
    waitCond(&indexSession->requestCond, &indexSession->requestMutex);
  }

  if (indexSession->state & IS_FLAG_DESTROYING) {
    unlockMutex(&indexSession->requestMutex);
    return EBUSY;
  }

  indexSession->state |= IS_FLAG_DESTROYING;
  loadPending = ((indexSession->state & IS_FLAG_LOADING)
                 && (indexSession->state & IS_FLAG_SUSPENDED));
  unlockMutex(&indexSession->requestMutex);

  if (loadPending) {
    // Tell the index to terminate the rebuild.
    lockMutex(&indexSession->loadContext.mutex);
    if (indexSession->loadContext.status == INDEX_SUSPENDED) {
      indexSession->loadContext.status = INDEX_FREEING;
      broadcastCond(&indexSession->loadContext.cond);
    }
    unlockMutex(&indexSession->loadContext.mutex);

    // Wait until the load exits before proceeding.
    lockMutex(&indexSession->requestMutex);
    while (indexSession->state & IS_FLAG_LOADING) {
      waitCond(&indexSession->requestCond, &indexSession->requestMutex);
    }
    unlockMutex(&indexSession->requestMutex);
  }

  waitForNoRequestsInProgress(indexSession);
  int result = saveAndFreeIndex(indexSession);
  requestQueueFinish(indexSession->callbackQueue);
  indexSession->callbackQueue = NULL;
  destroyCond(&indexSession->loadContext.cond);
  destroyMutex(&indexSession->loadContext.mutex);
  destroyCond(&indexSession->requestCond);
  destroyMutex(&indexSession->requestMutex);
  logDebug("Destroyed index session");
  FREE(indexSession);
  return result;
}

/**********************************************************************/
int udsFlushIndexSession(struct uds_index_session *indexSession)
{
  waitForNoRequestsInProgress(indexSession);
  // Wait until any open chapter writes are complete
  waitForIdleIndexRouter(indexSession->router);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsSaveIndex(struct uds_index_session *indexSession)
{
  waitForNoRequestsInProgress(indexSession);
  // saveIndexRouter waits for open chapter writes to complete
  return saveIndexRouter(indexSession->router);
}

/**********************************************************************/
int udsSetCheckpointFrequency(struct uds_index_session *indexSession,
                              unsigned int              frequency)
{
  setIndexCheckpointFrequency(indexSession->router->index->checkpoint,
                              frequency);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsGetIndexConfiguration(struct uds_index_session *indexSession,
                             UdsConfiguration         *conf)
{
  if (conf == NULL) {
    return logErrorWithStringError(UDS_CONF_PTR_REQUIRED,
                                   "received a NULL config pointer");
  }
  int result = ALLOCATE(1, struct udsConfiguration, __func__, conf);
  if (result == UDS_SUCCESS) {
    **conf = indexSession->userConfig;
  }
  return result;
}

/**********************************************************************/
int udsGetIndexStats(struct uds_index_session *indexSession,
                     UdsIndexStats            *stats)
{
  if (stats == NULL) {
    return logErrorWithStringError(UDS_INDEX_STATS_PTR_REQUIRED,
                                   "received a NULL index stats pointer");
  }
  getIndexStats(indexSession->router->index, stats);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsGetIndexSessionStats(struct uds_index_session *indexSession,
                            UdsContextStats          *stats)
{
  if (stats == NULL) {
    return logWarningWithStringError(UDS_CONTEXT_STATS_PTR_REQUIRED,
                                     "received a NULL context stats pointer");
  }
  collectStats(indexSession, stats);
  return UDS_SUCCESS;
}
