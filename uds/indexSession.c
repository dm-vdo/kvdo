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
 * $Id: //eng/uds-releases/flanders-rhel7.5/src/uds/indexSession.c#1 $
 */

#include "indexSession.h"

#include "grid.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "notificationDefs.h"
#include "udsState.h"

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
  return (IndexSessionState) (int32_t) atomicLoad32(&indexSession->state);
}

/**********************************************************************/
void setIndexSessionState(IndexSession      *indexSession,
                          IndexSessionState  state)
{
  atomicStore32(&indexSession->state, state);
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
  int result
    = ALLOCATE(1, IndexSession, "empty index session", &session);
  if (result != UDS_SUCCESS) {
    return result;
  }

  setIndexSessionState(session, IS_INIT);

  *indexSessionPtr = session;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int saveIndexSession(IndexSession *indexSession)
{
  if (indexSession == NULL) {
    return UDS_SUCCESS;
  }

  int result = saveGrid(indexSession->grid);
  freeGrid(indexSession->grid);
  return result;
}

/**********************************************************************/
int saveAndFreeIndexSession(IndexSession *indexSession)
{
  int result = saveIndexSession(indexSession);
  if (result != UDS_SUCCESS) {
    logInfoWithStringError(result, "ignoring error from saveIndexSession");
  }
  logDebug("Closed index session (%u, %p)", indexSession->session.id,
           (void *) indexSession);
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

  logDebug("Closing index session (%u, %p)", session.id,
           (void *) indexSession);
  finishSession(indexSessionGroup, &indexSession->session);
  result = saveAndFreeIndexSession(indexSession);
  releaseSessionGroup(indexSessionGroup);
  notifyIndexClosed(session);
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
