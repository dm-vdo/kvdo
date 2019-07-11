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
 * $Id: //eng/uds-releases/jasper/src/uds/udsState.c#2 $
 */

#include "udsState.h"

#include "atomicDefs.h"
#include "errors.h"
#include "featureDefs.h"
#include "logger.h"
#include "stringUtils.h"
#include "threads.h"
#include "uds.h"

/*
 * XXX This module is mostly needed for user mode, and it needs a major
 *     overhaul.
 *
 * XXX The destructor code was used to ensure that any open index was saved
 *     when the process exits.  This code path kicked in when an albserver was
 *     killed.  Today we do not have an albserver, so it is not absolutely
 *     necessary.  Nor do we have any other user mode usage that needs to save
 *     the open index automatically.  The kernel does not have such a
 *     destructor.  Eliminating the session mechanism deleted our list of open
 *     index sessions.  Rather than create new code to maintain this list, we
 *     leave that work until (and if) we ever have a real user mode need for
 *     it.
 */

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

static atomic_t initState = ATOMIC_INIT(STATE_UNINITIALIZED);

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

/*
 * ===========================================================================
 * UDS system initialization and shutdown
 * ===========================================================================
 */

/**********************************************************************/
static int udsInitializeOnce(void)
{
  ensureStandardErrorBlocks();
  openLogger();
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
  udsState.currentState = UDS_GS_RUNNING;
  unlockMutex(&udsState.mutex);
  return UDS_SUCCESS;
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
  // Prevent the creation of new sessions.
  udsState.currentState = UDS_GS_SHUTTING_DOWN;

  unlockMutex(&udsState.mutex);

  destroyMutex(&udsState.mutex);
  closeLogger();
}

/**********************************************************************/
void udsInitialize(void)
{
  for (;;) {
    switch (atomic_cmpxchg(&initState, STATE_UNINITIALIZED,
                           STATE_IN_TRANSIT)) {
    case STATE_UNINITIALIZED:
      if (udsInitializeOnce() == UDS_SUCCESS) {
        atomic_set_release(&initState, STATE_RUNNING);
        return;
      }
      udsShutdownOnce();
      atomic_set_release(&initState, STATE_UNINITIALIZED);
      return;
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
    switch (atomic_cmpxchg(&initState, STATE_RUNNING, STATE_IN_TRANSIT)) {
    case STATE_UNINITIALIZED:
    default:
      return;
    case STATE_IN_TRANSIT:
      yieldScheduler();
      break;
    case STATE_RUNNING:
      udsShutdownOnce();
      atomic_set_release(&initState, STATE_UNINITIALIZED);
      return;
    }
  }
}
