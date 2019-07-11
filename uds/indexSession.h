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
 * $Id: //eng/uds-releases/jasper/src/uds/indexSession.h#3 $
 */

#ifndef INDEX_SESSION_H
#define INDEX_SESSION_H

#include "atomicDefs.h"
#include "config.h"
#include "cpu.h"
#include "opaqueTypes.h"
#include "threads.h"
#include "uds.h"

typedef enum {
  IS_INIT     = 1,
  IS_READY    = 2,
  IS_DISABLED = 3
} IndexSessionState;

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

struct uds_index_session {
  // atomically updated IndexSessionState
  atomic_t                 state; 
  IndexRouter             *router;
  RequestQueue            *callbackQueue;
  struct udsConfiguration  userConfig;
  // Asynchronous Request synchronization
  Mutex                    requestMutex;
  CondVar                  requestCond;
  int                      requestCount;
  // Request statistics, all owned by the callback thread
  SessionStats             stats;
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
 * Get the current IndexSessionState from an index session.
 *
 * @param indexSession  the session to query
 **/
IndexSessionState getIndexSessionState(struct uds_index_session *indexSession);

/**
 * Set the IndexSessionState of the index session.
 *
 * @param indexSession  the session to be modified
 * @param state         the new session state
 **/
void setIndexSessionState(struct uds_index_session *indexSession,
                          IndexSessionState         state);

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
 * Construct a new index session, initializing the state to IS_INIT.
 *
 * @param indexSessionPtr   The pointer to receive the new session
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeEmptyIndexSession(struct uds_index_session **indexSessionPtr)
  __attribute__((warn_unused_result));

/**
 * Close the index session by saving the underlying grid, unloading the
 * modules referenced by the session, and freeing the underlying session
 * structure.
 *
 * @param indexSession  The index session to be shut down and freed
 **/
int saveAndFreeIndexSession(struct uds_index_session *indexSession);

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
