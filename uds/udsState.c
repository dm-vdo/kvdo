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
 * $Id: //eng/uds-releases/flanders/src/uds/udsState.c#7 $
 */

#include "udsState.h"

#include "context.h"
#include "errors.h"
#include "featureDefs.h"
#include "indexSession.h"
#include "indexRouter.h"
#include "isCallbackThreadDefs.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "request.h"

/**
 * The current library state.
 **/
typedef enum {
  UDS_GS_UNINIT        = 1,
  UDS_GS_RUNNING       = 2,
  UDS_GS_SHUTTING_DOWN = 3
} UdsGState;

typedef struct {
  char              cookie[16]; // for locating udsState in core file
  char              version[16]; // for double-checking library version
  Mutex             mutex;
#if GRID
  RequestQueue     *remoteQueue;
#endif /* GRID */
  SessionGroup     *indexSessions;
  SessionGroup     *contexts;
  UdsGState         currentState;
#if DESTRUCTOR
  UdsShutdownHook  *shutdownHook;
#endif /* DESTRUCTOR */
} UdsGlobalState;

static UdsGlobalState udsState;

/**********************************************************************/

enum {
  STATE_UNINITIALIZED = 0,
  STATE_IN_TRANSIT    = 1,
  STATE_RUNNING       = 2,
};

static Atomic32 initState = ATOMIC_INITIALIZER(STATE_UNINITIALIZED);

/**********************************************************************/
void lockGlobalStateMutex(void)
{
  lockMutex(&udsState.mutex);
}

/**********************************************************************/
void unlockGlobalStateMutex(void)
{
  unlockMutex(&udsState.mutex);
}

/**********************************************************************/
int checkLibraryRunning(void)
{
  switch (udsState.currentState) {
  case UDS_GS_RUNNING:
    return UDS_SUCCESS;

  case UDS_GS_SHUTTING_DOWN:
    return UDS_SHUTTINGDOWN;

  case UDS_GS_UNINIT:
  default:
    return UDS_UNINITIALIZED;
  }
}

#if GRID
/**
 * Request processing function for the remote index request queue.
 *
 * @param request  the request to send to a remote index
 **/
static void remoteIndexRequestProcessor(Request *request)
{
  request->router->methods->execute(request->router, request);
}

/**********************************************************************/
int initializeRemoteQueue(void)
{
  int ret = UDS_SUCCESS;
  if (udsState.remoteQueue == NULL) {
    lockMutex(&udsState.mutex);
    if (udsState.remoteQueue == NULL) {
      ret = makeRequestQueue("uds:remoteW", &remoteIndexRequestProcessor,
                             &udsState.remoteQueue);
    }
    unlockMutex(&udsState.mutex);
  }
  return ret;
}

/**********************************************************************/
RequestQueue *getRemoteQueue(void)
{
  return udsState.remoteQueue;
}

/**********************************************************************/
void freeRemoteQueue(void)
{
  requestQueueFinish(udsState.remoteQueue);
  udsState.remoteQueue = NULL;
}
#endif /* GRID */

/**********************************************************************/
SessionGroup *getContextGroup(void)
{
  udsInitialize();
  return udsState.contexts;
}

/**********************************************************************/
SessionGroup *getIndexSessionGroup(void)
{
  udsInitialize();
  return udsState.indexSessions;
}

/*
 * ===========================================================================
 * UDS system initialization and shutdown
 * ===========================================================================
 */

/**********************************************************************/
static void forceFreeContext(SessionContents contents)
{
  freeContext((UdsContext *) contents);
}

/**********************************************************************/
static void forceFreeIndexSession(SessionContents contents)
{
  saveAndFreeIndexSession((IndexSession *) contents);
}

/**********************************************************************/
static int udsInitializeLocked(void)
{
  // Create the session and context containers.
  int result = makeSessionGroup(UDS_NOCONTEXT, forceFreeContext,
                                &udsState.contexts);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = makeSessionGroup(UDS_NO_INDEXSESSION, forceFreeIndexSession,
                            &udsState.indexSessions);
  if (result != UDS_SUCCESS) {
    return result;
  }

  udsState.currentState = UDS_GS_RUNNING;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int udsInitializeOnce(void)
{
  ensureStandardErrorBlocks();
  openLogger();
  createCallbackThread();
  memset(&udsState, 0, sizeof(udsState));
  strncpy(udsState.cookie, "udsStateCookie", sizeof(udsState.cookie));
#ifdef UDS_VERSION
  strncpy(udsState.version, UDS_VERSION, sizeof(udsState.version));
#else
  strncpy(udsState.version, "internal", sizeof(udsState.version));
#endif
  udsState.currentState = UDS_GS_UNINIT;
#if DESTRUCTOR
  udsState.shutdownHook = udsShutdown;
#endif /* DESTRUCTOR */
  initializeMutex(&udsState.mutex, true);

  lockMutex(&udsState.mutex);
  int result = udsInitializeLocked();
  unlockMutex(&udsState.mutex);
  return result;
}

#if DESTRUCTOR
/**********************************************************************/
UdsShutdownHook *udsSetShutdownHook(UdsShutdownHook *shutdownHook)
{
  udsInitialize();
  UdsShutdownHook *oldUdsShutdownHook = udsState.shutdownHook;
  udsState.shutdownHook = shutdownHook;
  return oldUdsShutdownHook;
}

/**********************************************************************/
__attribute__((destructor))
static void udsShutdownDestructor(void)
{
  if (udsState.shutdownHook != NULL) {
    (*udsState.shutdownHook)();
  }
}
#endif /* DESTRUCTOR */

/**********************************************************************/
static void udsShutdownOnce(void)
{
  lockMutex(&udsState.mutex);
  // Prevent the creation of new contexts.
  udsState.currentState = UDS_GS_SHUTTING_DOWN;

  // Shut down all contexts, waiting for outstanding requests to complete and
  // release their contexts.
  if (udsState.contexts != NULL) {
    shutdownSessionGroup(udsState.contexts);
    udsState.contexts = NULL;
  }

  // Shut down all index sessions, waiting for outstanding operations to
  // complete.
  if (udsState.indexSessions != NULL) {
    shutdownSessionGroup(udsState.indexSessions);
    udsState.indexSessions = NULL;
  }

  // Shut down the queues
#if GRID
  freeRemoteQueue();
#endif /* GRID */
  unlockMutex(&udsState.mutex);

  destroyMutex(&udsState.mutex);
  closeLogger();
  deleteCallbackThread();
}

/**********************************************************************/
void udsInitialize(void)
{
  for (;;) {
    switch (atomicLoad32(&initState)) {
    case STATE_UNINITIALIZED:
      if (compareAndSwap32(&initState, STATE_UNINITIALIZED,
                           STATE_IN_TRANSIT)) {
        if (udsInitializeOnce() == UDS_SUCCESS) {
          atomicStore32(&initState, STATE_RUNNING);
          return;
        }
        udsShutdownOnce();
        atomicStore32(&initState, STATE_UNINITIALIZED);
        return;
      }
      break;
    case STATE_IN_TRANSIT:
      yieldScheduler();
      break;
    case STATE_RUNNING:
    default:
      return;
    }
  }
}

/**********************************************************************/
void udsShutdown(void)
{
  for (;;) {
    switch (atomicLoad32(&initState)) {
    case STATE_UNINITIALIZED:
    default:
      return;
    case STATE_IN_TRANSIT:
      yieldScheduler();
      break;
    case STATE_RUNNING:
      if (compareAndSwap32(&initState, STATE_RUNNING,
                           STATE_IN_TRANSIT)) {
        udsShutdownOnce();
        atomicStore32(&initState, STATE_UNINITIALIZED);
        return;
      }
      break;
    }
  }
}
