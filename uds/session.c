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
 * $Id: //eng/uds-releases/gloria/src/uds/session.c#2 $
 */

#include "session.h"

#include "common.h"
#include "memoryAlloc.h"
#include "threads.h"
#include "typeDefs.h"

typedef enum {
  GROUP_READY    = 1,
  GROUP_SHUTDOWN = 2
} SessionGroupState;

static const unsigned int SESSION_ID_MAX = UINT_MAX;

LIST__HEAD(sessionListHead, session);
typedef struct sessionListHead SessionListHead;

struct sessionGroup {
  Mutex             mutex;
  CondVar           releaseCond; // signalled when refCount decremented
  unsigned int      refCount;
  SessionGroupState state;
  SessionListHead   head;
  SessionID         nextSessionID;
  SessionFree       free;
  int               notFoundResult;
};

/**********************************************************************/
static int checkSessionGroupLocked(SessionGroup *group)
{
  switch (group->state) {
  case GROUP_SHUTDOWN:
    return UDS_SHUTTINGDOWN;
  default:
    return UDS_SUCCESS;
  }
}

/**********************************************************************/
static void acquireSession(Session *session)
{
  lockMutex(&session->mutex);
  session->refCount++;
  unlockMutex(&session->mutex);
}

/**********************************************************************/
static Session *searchList(SessionGroup *group, SessionID id)
{
  Session *session;
  LIST_FOREACH(session, &group->head, links) {
    if (session->id == id) {
      return session;
    }
  }
  return NULL;
}

/**********************************************************************/
int getSession(SessionGroup *group, SessionID id,
               Session **sessionPtr)
{
  lockMutex(&group->mutex);
  int result = checkSessionGroupLocked(group);
  if (result != UDS_SUCCESS) {
    unlockMutex(&group->mutex);
    return result;
  }

  Session *session = searchList(group, id);
  if (session != NULL) {
    acquireSession(session);
    *sessionPtr = session;
    result = UDS_SUCCESS;
  } else {
    result = group->notFoundResult;
  }

  unlockMutex(&group->mutex);
  return result;
}

/**********************************************************************/
SessionContents getSessionContents(Session *session)
{
  return session->contents;
}

/**********************************************************************/
void releaseSession(Session *session)
{
  lockMutex(&session->mutex);
  session->refCount--;
  broadcastCond(&session->releaseCond);
  unlockMutex(&session->mutex);
}

/**********************************************************************/
int initializeSession(SessionGroup    *group,
                      Session         *session,
                      SessionContents  contents,
                      SessionID       *sessionID)
{
  int result = initMutex(&session->mutex);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initCond(&session->releaseCond);
  if (result != UDS_SUCCESS) {
    destroyMutex(&session->mutex);
    return result;
  }

  lockMutex(&group->mutex);
  SessionID id         = group->nextSessionID;
  group->nextSessionID = ((group->nextSessionID == SESSION_ID_MAX) ?
                          SESSION_ID_INIT : group->nextSessionID + 1);

  session->refCount = 1; // start with one reference on the session
  session->contents = contents;
  session->id       = id;
  LIST_INSERT_HEAD(&group->head, session, links);
  unlockMutex(&group->mutex);

  *sessionID = id;
  return UDS_SUCCESS;
}

/**********************************************************************/
static void orphanSessionLocked(Session *session)
{
  if (session->id != SESSION_ID_NONE) {
    LIST_REMOVE(session, links);
    session->id = SESSION_ID_NONE;
  }
}

/**********************************************************************/
void waitForIdleSession(Session *session)
{
  lockMutex(&session->mutex);
  session->idleWaiters += 1;
  while (session->refCount > session->idleWaiters) {
    waitCond(&session->releaseCond, &session->mutex);
  }
  session->idleWaiters -= 1;
  unlockMutex(&session->mutex);
}

/**********************************************************************/
static void waitForSessionToStop(Session *session)
{
  lockMutex(&session->mutex);
  // Don't deadlock if there's still a thread in waitForIdleSession().
  session->idleWaiters += 1;
  // Wait until this is the only thread still waiting, which must eventually
  // become true since finishSession() may only be called once.
  while (session->refCount > 1) {
    waitCond(&session->releaseCond, &session->mutex);
  }
  // Moot, but good form.
  session->idleWaiters -= 1;
  unlockMutex(&session->mutex);
}

/**********************************************************************/
void finishSession(SessionGroup *group, Session *session)
{
  lockMutex(&group->mutex);
  orphanSessionLocked(session);
  unlockMutex(&group->mutex);
  waitForSessionToStop(session);
  destroyCond(&session->releaseCond);
  destroyMutex(&session->mutex);
}

/**********************************************************************/
int makeSessionGroup(int notFoundResult, SessionFree free,
                     SessionGroup **groupPtr)
{
  SessionGroup *group;
  int result = ALLOCATE(1, SessionGroup, "session group", &group);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initMutex(&group->mutex);
  if (result != UDS_SUCCESS) {
    FREE(group);
    return result;
  }
  result = initCond(&group->releaseCond);
  if (result != UDS_SUCCESS) {
    destroyMutex(&group->mutex);
    FREE(group);
    return result;
  }

  LIST_INIT(&group->head);
  group->nextSessionID = SESSION_ID_INIT;
  group->refCount = 1; // whoever stores the group has a reference.
  group->state = GROUP_READY;
  group->free = free;
  group->notFoundResult = notFoundResult;

  *groupPtr = group;
  return UDS_SUCCESS;
}

/**********************************************************************/
int acquireSessionGroup(SessionGroup *group)
{
  lockMutex(&group->mutex);
  int result = checkSessionGroupLocked(group);
  if (result == UDS_SUCCESS) {
    group->refCount++;
  }
  unlockMutex(&group->mutex);
  return result;
}

/**********************************************************************/
void releaseSessionGroup(SessionGroup *group)
{
  lockMutex(&group->mutex);
  group->refCount--;
  broadcastCond(&group->releaseCond);
  unlockMutex(&group->mutex);
}


/**********************************************************************/
void shutdownSessionGroup(SessionGroup *group)
{
  Session         *session;
  SessionListHead  tempHead;
  SessionFree      freeFunc;
  LIST_INIT(&tempHead);

  lockMutex(&group->mutex);
  group->state = GROUP_SHUTDOWN;
  while (group->refCount > 1) {
    waitCond(&group->releaseCond, &group->mutex);
  }

  freeFunc = group->free;
  while (!LIST_EMPTY(&group->head)) {
    session = LIST_FIRST(&group->head);
    acquireSession(session);
    orphanSessionLocked(session);
    LIST_INSERT_HEAD(&tempHead, session, links);
  }
  unlockMutex(&group->mutex);

  while (!LIST_EMPTY(&tempHead)) {
    session = LIST_FIRST(&tempHead);
    waitForSessionToStop(session);
    LIST_REMOVE(session, links);
    destroyCond(&session->releaseCond);
    destroyMutex(&session->mutex);
    if (freeFunc != NULL) {
      freeFunc(session->contents);
    }
  }

  destroyCond(&group->releaseCond);
  destroyMutex(&group->mutex);
  FREE(group);
}
