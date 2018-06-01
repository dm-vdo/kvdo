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
 * $Id: //eng/uds-releases/gloria/src/uds/context.h#3 $
 */

#ifndef CONTEXT_H
#define CONTEXT_H

#include "common.h"
#include "cpu.h"
#include "featureDefs.h"
#include "opaqueTypes.h"
#include "session.h"
#include "uds-block.h"

/**
 * The current context state.
 **/
typedef enum udsContextState {
  UDS_CS_READY     = 1,
  UDS_CS_DISABLED  = 2
} UdsContextState;

/**
 * Context for uds client index requests.
 **/
typedef struct udsContext {
  /* The id of this context */
  unsigned int     id;
  /* The state of this context (whether or not it may be used) */
  UdsContextState  contextState;
  /* The index and session which own this context */
  IndexSession    *indexSession;
  Session          session;
} UdsContext;

/**
 * Open a context.
 *
 * @param [in]  session          The index session on which to open a context
 * @param [out] contextID        A point to hold the id of the new context
 *
 * @return UDS_SUCCESS or an error
 **/
int openContext(UdsIndexSession session, unsigned int *contextID)
  __attribute__((warn_unused_result));

/**
 * Convert an internal to an external error, disabling the context if the
 * error is permanent.
 *
 * @param context   The context which had the error
 * @param errorCode The internal error code
 *
 * @return The external error code
 **/
int handleError(UdsContext *context, int errorCode)
  __attribute__((warn_unused_result));

/**
 * Convert an internal to an external error, disabling the context if the
 * error is permanent, then release the context.
 *
 * @param context   The context which had the error
 * @param errorCode The internal error code
 *
 * @return The external error code
 **/
int handleErrorAndReleaseBaseContext(UdsContext *context, int errorCode)
  __attribute__((warn_unused_result));

/**
 * Construct a new UdsContext, initializing fields excluding the context's
 * session.
 *
 * @param [in]  indexSession     The index session under which the context is
 *                               opened
 * @param [out] contextPtr       The pointer to receive the new context
 **/
int makeBaseContext(IndexSession *indexSession, UdsContext **contextPtr)
  __attribute__((warn_unused_result));

/**
 * Get the non-type-specific underlying context for a given context.
 *
 * @param contextId   The id of the context making the request
 * @param contextPtr  A pointer to receive the base context
 *
 * @return UDS_SUCCESS or an error code
 **/
int getBaseContext(unsigned int contextId, UdsContext **contextPtr)
  __attribute__((warn_unused_result));

/**
 * Release the non-type-specific underlying context for a given context.
 *
 * @param context The context to release
 **/
void releaseBaseContext(UdsContext *context);

/**
 * Flush all outstanding requests on a given base context.
 *
 * @param context The context to flush
 **/
void flushBaseContext(UdsContext *context);

/**
 * Free a context.
 *
 * @param context The context to free
 **/
void freeContext(UdsContext *context);

/**
 * Flush all outstanding requests on a given context.
 *
 * @param contextId   The id of the context to flush
 *
 * @return UDS_SUCCESS or an error code
 **/
int flushContext(unsigned int contextId) __attribute__((warn_unused_result));

/**
 * Close a context.
 *
 * @param contextId   The id of the context to close
 **/
int closeContext(unsigned int contextId) __attribute__((warn_unused_result));

/**
 * Get the index statistics for the index associated with a given context.
 *
 * @param contextId   The id of the context
 * @param stats       A pointer to hold the statistics
 *
 * @return UDS_SUCCESS or an error
 **/
int getContextIndexStats(unsigned int contextId, UdsIndexStats *stats)
  __attribute__((warn_unused_result));

/**
 * Get statistics for a given context.
 *
 * @param contextId   The id of the context
 * @param stats       A pointer to hold the statistics
 *
 * @return UDS_SUCCESS or an error
 **/
int getContextStats(unsigned int contextId, UdsContextStats *stats)
  __attribute__((warn_unused_result));

/**
 * Dispatch a control request to a client context on the callback thread.
 *
 * @param request  the control request to dispatch
 *
 * @return UDS_SUCCESS or an error code
 **/
int dispatchContextControlRequest(Request *request)
  __attribute__((warn_unused_result));

#endif /* CONTEXT_H */
