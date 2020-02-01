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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadCondVarLinuxKernel.c#1 $
 */

#include "threads.h"
#include "timeUtils.h"
#include "uds-error.h"

/**********************************************************************/
int initCond(CondVar *cv)
{
  cv->eventCount = NULL;
  return makeEventCount(&cv->eventCount);
}

/**********************************************************************/
int signalCond(CondVar *cv)
{
  eventCountBroadcast(cv->eventCount);
  return UDS_SUCCESS;
}

/**********************************************************************/
int broadcastCond(CondVar *cv)
{
  eventCountBroadcast(cv->eventCount);
  return UDS_SUCCESS;
}

/**********************************************************************/
int waitCond(CondVar *cv, Mutex *mutex)
{
  EventToken token = eventCountPrepare(cv->eventCount);
  unlockMutex(mutex);
  eventCountWait(cv->eventCount, token, NULL);
  lockMutex(mutex);
  return UDS_SUCCESS;
}

/**********************************************************************/
int timedWaitCond(CondVar *cv, Mutex *mutex, RelTime timeout)
{
  EventToken token = eventCountPrepare(cv->eventCount);
  unlockMutex(mutex);
  bool happened = eventCountWait(cv->eventCount, token, &timeout);
  lockMutex(mutex);
  return happened ? UDS_SUCCESS : ETIMEDOUT;
}

/**********************************************************************/
int destroyCond(CondVar *cv)
{
  freeEventCount(cv->eventCount);
  cv->eventCount = NULL;
  return UDS_SUCCESS;
}
