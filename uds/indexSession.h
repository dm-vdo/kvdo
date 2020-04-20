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
 * $Id: //eng/uds-releases/krusty/src/uds/indexSession.h#2 $
 */

#ifndef INDEX_SESSION_H
#define INDEX_SESSION_H

#include "atomicDefs.h"
#include "config.h"
#include "cpu.h"
#include "opaqueTypes.h"
#include "threads.h"
#include "uds.h"

/**
 * The bit position of flags used to indicate index session states.
 **/
typedef enum {
  IS_FLAG_BIT_START      = 8,
  /** Flag indicating that the session is loading */
  IS_FLAG_BIT_LOADING    = IS_FLAG_BIT_START,
  /** Flag indicating that that the session has been loaded */
  IS_FLAG_BIT_LOADED,
  /** Flag indicating that the session is disabled permanently */
  IS_FLAG_BIT_DISABLED,
  /** Flag indicating that the session is suspended */
  IS_FLAG_BIT_SUSPENDED,
  /** Flag indicating that the session is waiting for an index state change */
  IS_FLAG_BIT_WAITING,
} IndexSessionFlagBit;

/**
 * The index session state flags.
 **/
typedef enum {
  IS_FLAG_LOADED    = (1 << IS_FLAG_BIT_LOADED),
  IS_FLAG_LOADING   = (1 << IS_FLAG_BIT_LOADING),
  IS_FLAG_DISABLED  = (1 << IS_FLAG_BIT_DISABLED),
  IS_FLAG_SUSPENDED = (1 << IS_FLAG_BIT_SUSPENDED),
  IS_FLAG_WAITING   = (1 << IS_FLAG_BIT_WAITING),
} IndexSessionFlag;

typedef struct __attribute__((aligned(CACHE_LINE_BYTES))) sessionStats {
  uint64_t postsFound;            /* Post calls that found an entry */
  uint64_t postsFoundOpenChapter; /* Post calls found in the open chapter */
  uint64_t postsFoundDense;       /* Post calls found in the dense index */
  uint64_t postsFoundSparse;      /* Post calls found in the sparse index */
  uint64_t postsNotFound;         /* Post calls that did not find an entry */
  uint64_t updatesFound;          /* Update calls that found an entry */
  uint64_t updatesNotFound;       /* Update calls that did not find an entry */
  uint64_t deletionsFound;        /* Delete calls that found an entry */
  uint64_t deletionsNotFound;     /* Delete calls that did not find an entry */
  uint64_t queriesFound;          /* Query calls that found an entry */
  uint64_t queriesNotFound;       /* Query calls that did not find an entry */
  uint64_t requests;              /* Total number of requests */
} SessionStats;

/**
 * States used in the index load context, reflecting the state of the index.
 **/
typedef enum {
  /** The index has not been loaded or rebuilt completely */
  INDEX_OPENING    = 0,
  /** The index is able to handle requests */
  INDEX_READY,
  /** The index has a pending request to suspend */
  INDEX_SUSPENDING,
  /** The index is suspended in the midst of a rebuild */
  INDEX_SUSPENDED,
  /** The index is being shut down while suspended */
  INDEX_FREEING,
} IndexSuspendStatus;

/**
 * The CondVar here must be notified when the status changes to
 * INDEX_SUSPENDED, in order to wake up the waiting udsSuspendIndexSession()
 * call. It must also be notified when the status changes away from
 * INDEX_SUSPENDED, to resume rebuild the index from checkForSuspend() in the
 * index.
 **/
typedef struct indexLoadContext {
  Mutex              mutex;
  CondVar            cond;
  IndexSuspendStatus status;  // Covered by indexLoadContext.mutex.
} IndexLoadContext;

/**
 * The request CondVar here must be notified when IS_FLAG_WAITING is cleared,
 * in case udsCloseIndex() or udDestroyIndexSesion() is waiting for it. It
 * must also be notified when IS_FLAG_LOADING is cleared, to inform
 * udsDestroyIndexSession() that the index session can be safely freed.
 **/
struct uds_index_session {
  unsigned int              state;   // Covered by requestMutex.
  IndexRouter              *router;
  RequestQueue             *callbackQueue;
  struct uds_configuration  userConfig;
  IndexLoadContext          loadContext;
  // Asynchronous Request synchronization
  Mutex                     requestMutex;
  CondVar                   requestCond;
  int                       requestCount;
  // Request statistics, all owned by the callback thread
  SessionStats              stats;
};

/**
 * Check that the index session is usable.
 *
 * @param indexSession  the session to query
 *
 * @return UDS_SUCCESS or an error code
 **/
int checkIndexSession(struct uds_index_session *indexSession)
  __attribute__((warn_unused_result));

/**
 * Make sure that the IndexSession is allowed to load an index, and if so, set
 * its state to indicate that the load has started.
 *
 * @param indexSession  the session to load with
 *
 * @return UDS_SUCCESS, or an error code if an index already exists.
 **/
int startLoadingIndexSession(struct uds_index_session *indexSession)
  __attribute__((warn_unused_result));

/**
 * Update the IndexSession state after attempting to load an index, to indicate
 * that the load has completed, and whether or not it succeeded.
 *
 * @param indexSession  the session that was loading
 * @param result        the result of the load operation
 **/
void finishLoadingIndexSession(struct uds_index_session *indexSession,
                               int                       result);

/**
 * Disable an index session due to an error.
 *
 * @param indexSession  the session to be disabled
 **/
void disableIndexSession(struct uds_index_session *indexSession);

/**
 * Acquire the index session for an asynchronous index request.
 *
 * The pointer must eventually be released with a corresponding call to
 * releaseIndexSession().
 *
 * @param indexSession  The index session
 *
 * @return UDS_SUCCESS or an error code
 **/
int getIndexSession(struct uds_index_session *indexSession)
  __attribute__((warn_unused_result));

/**
 * Release a pointer to an index session.
 *
 * @param indexSession  The session to release
 **/
void releaseIndexSession(struct uds_index_session *indexSession);

/**
 * Construct a new, empty index session.
 *
 * @param indexSessionPtr   The pointer to receive the new session
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeEmptyIndexSession(struct uds_index_session **indexSessionPtr)
  __attribute__((warn_unused_result));

/**
 * Save an index while the session is quiescent.
 *
 * During the call to #udsSaveIndex, there should be no other call to
 * #udsSaveIndex and there should be no calls to #udsStartChunkOperation.
 *
 * @param indexSession  The session to save
 *
 * @return Either #UDS_SUCCESS or an error code
 **/
int udsSaveIndex(struct uds_index_session *indexSession)
  __attribute__((warn_unused_result));

/**
 * Close the index by saving the underlying index.
 *
 * @param indexSession  The index session to be shut down and freed
 **/
int saveAndFreeIndex(struct uds_index_session *indexSession);

/**
 * Set the checkpoint frequency of the grid.
 *
 * @param session    The index session to be modified.
 * @param frequency  New checkpoint frequency.
 *
 * @return          Either UDS_SUCCESS or an error code.
 *
 **/
int udsSetCheckpointFrequency(struct uds_index_session *session,
                              unsigned int              frequency)
  __attribute__((warn_unused_result));

#endif /* INDEX_SESSION_H */
