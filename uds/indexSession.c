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
 * $Id: //eng/uds-releases/krusty/src/uds/indexSession.c#16 $
 */

#include "indexSession.h"

#include "indexCheckpoint.h"
#include "indexRouter.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "requestQueue.h"

/**********************************************************************/
static void collectStats(const struct uds_index_session *indexSession,
                         struct uds_context_stats       *stats)
{
  const SessionStats *sessionStats = &indexSession->stats;

  stats->current_time = absTimeToSeconds(currentTime(CLOCK_REALTIME));

  stats->posts_found           = READ_ONCE(sessionStats->postsFound);
  stats->in_memory_posts_found = READ_ONCE(sessionStats->postsFoundOpenChapter);
  stats->dense_posts_found     = READ_ONCE(sessionStats->postsFoundDense);
  stats->sparse_posts_found    = READ_ONCE(sessionStats->postsFoundSparse);
  stats->posts_not_found       = READ_ONCE(sessionStats->postsNotFound);
  stats->updates_found         = READ_ONCE(sessionStats->updatesFound);
  stats->updates_not_found     = READ_ONCE(sessionStats->updatesNotFound);
  stats->deletions_found       = READ_ONCE(sessionStats->deletionsFound);
  stats->deletions_not_found   = READ_ONCE(sessionStats->deletionsNotFound);
  stats->queries_found         = READ_ONCE(sessionStats->queriesFound);
  stats->queries_not_found     = READ_ONCE(sessionStats->queriesNotFound);
  stats->requests              = READ_ONCE(sessionStats->requests);
}

/**********************************************************************/
static void handleCallbacks(Request *request)
{
  if (request->status == UDS_SUCCESS) {
    // Measure the turnaround time of this request and include that time,
    // along with the rest of the request, in the context's StatCounters.
    update_request_context_stats(request);
  }

  if (request->callback != NULL) {
    // The request has specified its own callback and does not expect to be
    // freed.
    struct uds_index_session *indexSession = request->session;
    request->found = (request->location != LOC_UNAVAILABLE);
    request->callback((struct uds_request *) request);
    // We do this release after the callback because of the contract of the
    // uds_flush_index_session method.
    releaseIndexSession(indexSession);
  }
}

/**********************************************************************/
int checkIndexSession(struct uds_index_session *indexSession)
{
  lock_mutex(&indexSession->requestMutex);
  unsigned int state = indexSession->state;
  unlock_mutex(&indexSession->requestMutex);

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
  lock_mutex(&indexSession->requestMutex);
  indexSession->requestCount++;
  unlock_mutex(&indexSession->requestMutex);

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
  lock_mutex(&indexSession->requestMutex);
  if (--indexSession->requestCount == 0) {
    broadcast_cond(&indexSession->requestCond);
  }
  unlock_mutex(&indexSession->requestMutex);
}

/**********************************************************************/
int startLoadingIndexSession(struct uds_index_session *indexSession)
{
  int result;
  lock_mutex(&indexSession->requestMutex);
  if (indexSession->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUSPENDED;
  } else if (indexSession->state != 0) {
    result = UDS_INDEXSESSION_IN_USE;
  } else {
    indexSession->state |= IS_FLAG_LOADING;
    result = UDS_SUCCESS;
  }
  unlock_mutex(&indexSession->requestMutex);
  return result;
}

/**********************************************************************/
void finishLoadingIndexSession(struct uds_index_session *indexSession,
                               int                       result)
{
  lock_mutex(&indexSession->requestMutex);
  indexSession->state &= ~IS_FLAG_LOADING;
  if (result == UDS_SUCCESS) {
    indexSession->state |= IS_FLAG_LOADED;
  }
  broadcast_cond(&indexSession->requestCond);
  unlock_mutex(&indexSession->requestMutex);
}

/**********************************************************************/
void disableIndexSession(struct uds_index_session *indexSession)
{
  lock_mutex(&indexSession->requestMutex);
  indexSession->state |= IS_FLAG_DISABLED;
  unlock_mutex(&indexSession->requestMutex);
}

/**********************************************************************/
int makeEmptyIndexSession(struct uds_index_session **indexSessionPtr)
{
  struct uds_index_session *session;
  int result = ALLOCATE(1, struct uds_index_session, __func__, &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = init_mutex(&session->requestMutex);
  if (result != UDS_SUCCESS) {
    FREE(session);
    return result;
  }

  result = init_cond(&session->requestCond);
  if (result != UDS_SUCCESS) {
    destroy_mutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = init_mutex(&session->loadContext.mutex);
  if (result != UDS_SUCCESS) {
    destroy_cond(&session->requestCond);
    destroy_mutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = init_cond(&session->loadContext.cond);
  if (result != UDS_SUCCESS) {
    destroy_mutex(&session->loadContext.mutex);
    destroy_cond(&session->requestCond);
    destroy_mutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  result = make_request_queue("callbackW", &handleCallbacks,
                              &session->callbackQueue);
  if (result != UDS_SUCCESS) {
    destroy_cond(&session->loadContext.cond);
    destroy_mutex(&session->loadContext.mutex);
    destroy_cond(&session->requestCond);
    destroy_mutex(&session->requestMutex);
    FREE(session);
    return result;
  }

  *indexSessionPtr = session;
  return UDS_SUCCESS;
}

/**********************************************************************/
int uds_suspend_index_session(struct uds_index_session *session, bool save)
{
  int result;
  bool save_index = false;
  bool suspendIndex = false;
  lock_mutex(&session->requestMutex);
  if (session->state & IS_FLAG_WAITING) {
    result = EBUSY;
  } else if (session->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUCCESS;
  } else if (!(session->state & IS_FLAG_LOADING)) {
    session->state |= IS_FLAG_SUSPENDED;
    broadcast_cond(&session->requestCond);
    save_index = save;
    result = UDS_SUCCESS;
  } else {
    session->state |= IS_FLAG_WAITING;
    suspendIndex = true;
    result = UDS_SUCCESS;
  }
  unlock_mutex(&session->requestMutex);
  if (save_index) {
    result = udsSaveIndex(session);
  }
  if (!suspendIndex) {
    return result;
  }

  lock_mutex(&session->loadContext.mutex);
  switch (session->loadContext.status) {
  case INDEX_OPENING:
    session->loadContext.status = INDEX_SUSPENDING;

    // Wait until the index indicates that it is not replaying.
    while ((session->loadContext.status != INDEX_SUSPENDED)
           && (session->loadContext.status != INDEX_READY)) {
      wait_cond(&session->loadContext.cond,
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
  unlock_mutex(&session->loadContext.mutex);

  lock_mutex(&session->requestMutex);
  session->state &= ~IS_FLAG_WAITING;
  session->state |= IS_FLAG_SUSPENDED;
  broadcast_cond(&session->requestCond);
  unlock_mutex(&session->requestMutex);
  return UDS_SUCCESS;
}

/**********************************************************************/
int uds_resume_index_session(struct uds_index_session *session)
{
  lock_mutex(&session->requestMutex);
  if (session->state & IS_FLAG_WAITING) {
    unlock_mutex(&session->requestMutex);
    return EBUSY;
  }

  /* If not suspended, just succeed */
  if (!(session->state & IS_FLAG_SUSPENDED)) {
    unlock_mutex(&session->requestMutex);
    return UDS_SUCCESS;
  }

  if (!(session->state & IS_FLAG_LOADING)) {
    session->state &= ~IS_FLAG_SUSPENDED;
    unlock_mutex(&session->requestMutex);
    return UDS_SUCCESS;
  }

  session->state |= IS_FLAG_WAITING;
  unlock_mutex(&session->requestMutex);

  lock_mutex(&session->loadContext.mutex);
  switch (session->loadContext.status) {
  case INDEX_SUSPENDED:
    session->loadContext.status = INDEX_OPENING;
    // Notify the index to start replaying again.
    broadcast_cond(&session->loadContext.cond);
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
  unlock_mutex(&session->loadContext.mutex);

  lock_mutex(&session->requestMutex);
  session->state &= ~IS_FLAG_WAITING;
  session->state &= ~IS_FLAG_SUSPENDED;
  broadcast_cond(&session->requestCond);
  unlock_mutex(&session->requestMutex);
  return UDS_SUCCESS;
}

/**********************************************************************/
static void waitForNoRequestsInProgress(struct uds_index_session *indexSession)
{
  lock_mutex(&indexSession->requestMutex);
  while (indexSession->requestCount > 0) {
    wait_cond(&indexSession->requestCond, &indexSession->requestMutex);
  }
  unlock_mutex(&indexSession->requestMutex);
}

/**********************************************************************/
int saveAndFreeIndex(struct uds_index_session *indexSession)
{
  int result = UDS_SUCCESS;
  struct index_router *router = indexSession->router;
  if (router != NULL) {
    lock_mutex(&indexSession->requestMutex);
    bool suspended = (indexSession->state & IS_FLAG_SUSPENDED);
    unlock_mutex(&indexSession->requestMutex);
    if (!suspended) {
      result = save_index_router(router);
      if (result != UDS_SUCCESS) {
        logWarningWithStringError(result, "ignoring error from save_index_router");
      }
    }
    free_index_router(router);
    indexSession->router = NULL;

    // Reset all index state that happens to be in the index session, so it
    // doesn't affect any future index.
    lock_mutex(&indexSession->loadContext.mutex);
    indexSession->loadContext.status = INDEX_OPENING;
    unlock_mutex(&indexSession->loadContext.mutex);

    lock_mutex(&indexSession->requestMutex);
    // Only the suspend bit will remain relevant.
    indexSession->state &= IS_FLAG_SUSPENDED;
    unlock_mutex(&indexSession->requestMutex);
  }

  logDebug("Closed index");
  return result;
}

/**********************************************************************/
int uds_close_index(struct uds_index_session *indexSession)
{
  lock_mutex(&indexSession->requestMutex);
  // Wait for any pending operation to complete.
  while (indexSession->state & IS_FLAG_WAITING) {
    wait_cond(&indexSession->requestCond, &indexSession->requestMutex);
  }

  int result = UDS_SUCCESS;
  if (indexSession->state & IS_FLAG_SUSPENDED) {
    result = UDS_SUSPENDED;
  } else if (!(indexSession->state & IS_FLAG_LOADED)) {
    // Either the index hasn't finished loading, or it doesn't exist.
    result = UDS_NO_INDEXSESSION;
  }
  unlock_mutex(&indexSession->requestMutex);
  if (result != UDS_SUCCESS) {
    return result;
  }

  logDebug("Closing index");
  waitForNoRequestsInProgress(indexSession);
  return saveAndFreeIndex(indexSession);
}

/**********************************************************************/
int uds_destroy_index_session(struct uds_index_session *indexSession)
{
  logDebug("Destroying index session");

  bool loadPending = false;
  lock_mutex(&indexSession->requestMutex);

  // Wait for any pending operation to complete.
  while (indexSession->state & IS_FLAG_WAITING) {
    wait_cond(&indexSession->requestCond, &indexSession->requestMutex);
  }

  loadPending = ((indexSession->state & IS_FLAG_LOADING)
                 && (indexSession->state & IS_FLAG_SUSPENDED));
  if (loadPending) {
    // Prevent any more suspend or resume operations.
    indexSession->state |= IS_FLAG_WAITING;
  }
  unlock_mutex(&indexSession->requestMutex);

  if (loadPending) {
    // Tell the index to terminate the rebuild.
    lock_mutex(&indexSession->loadContext.mutex);
    if (indexSession->loadContext.status == INDEX_SUSPENDED) {
      indexSession->loadContext.status = INDEX_FREEING;
      broadcast_cond(&indexSession->loadContext.cond);
    }
    unlock_mutex(&indexSession->loadContext.mutex);

    // Wait until the load exits before proceeding.
    lock_mutex(&indexSession->requestMutex);
    while (indexSession->state & IS_FLAG_LOADING) {
      wait_cond(&indexSession->requestCond, &indexSession->requestMutex);
    }
    unlock_mutex(&indexSession->requestMutex);
  }

  waitForNoRequestsInProgress(indexSession);
  int result = saveAndFreeIndex(indexSession);
  request_queue_finish(indexSession->callbackQueue);
  indexSession->callbackQueue = NULL;
  destroy_cond(&indexSession->loadContext.cond);
  destroy_mutex(&indexSession->loadContext.mutex);
  destroy_cond(&indexSession->requestCond);
  destroy_mutex(&indexSession->requestMutex);
  logDebug("Destroyed index session");
  FREE(indexSession);
  return result;
}

/**********************************************************************/
int uds_flush_index_session(struct uds_index_session *indexSession)
{
  waitForNoRequestsInProgress(indexSession);
  // Wait until any open chapter writes are complete
  wait_for_idle_index_router(indexSession->router);
  return UDS_SUCCESS;
}

/**********************************************************************/
int udsSaveIndex(struct uds_index_session *indexSession)
{
  waitForNoRequestsInProgress(indexSession);
  // save_index_router waits for open chapter writes to complete
  return save_index_router(indexSession->router);
}

/**********************************************************************/
int udsSetCheckpointFrequency(struct uds_index_session *indexSession,
                              unsigned int              frequency)
{
  set_index_checkpoint_frequency(indexSession->router->index->checkpoint,
                                 frequency);
  return UDS_SUCCESS;
}

/**********************************************************************/
int uds_get_index_configuration(struct uds_index_session  *indexSession,
                                struct uds_configuration **conf)
{
  if (conf == NULL) {
    return logErrorWithStringError(UDS_CONF_PTR_REQUIRED,
                                   "received a NULL config pointer");
  }
  int result = ALLOCATE(1, struct uds_configuration, __func__, conf);
  if (result == UDS_SUCCESS) {
    **conf = indexSession->userConfig;
  }
  return result;
}

/**********************************************************************/
int uds_get_index_stats(struct uds_index_session *indexSession,
                        struct uds_index_stats   *stats)
{
  if (stats == NULL) {
    return logErrorWithStringError(UDS_INDEX_STATS_PTR_REQUIRED,
                                   "received a NULL index stats pointer");
  }
  get_index_stats(indexSession->router->index, stats);
  return UDS_SUCCESS;
}

/**********************************************************************/
int uds_get_index_session_stats(struct uds_index_session *indexSession,
                                struct uds_context_stats *stats)
{
  if (stats == NULL) {
    return logWarningWithStringError(UDS_CONTEXT_STATS_PTR_REQUIRED,
                                     "received a NULL context stats pointer");
  }
  collectStats(indexSession, stats);
  return UDS_SUCCESS;
}
