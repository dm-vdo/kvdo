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
 * $Id: //eng/uds-releases/gloria/src/uds/request.h#2 $
 */

#ifndef REQUEST_H
#define REQUEST_H

#include "cacheCounters.h"
#include "common.h"
#include "compiler.h"
#include "context.h"
#include "featureDefs.h"
#include "queue.h"
#include "timeUtils.h"
#include "uds.h"
#include "util/funnelQueue.h"

/**
 * RequestAction values indicate what action, command, or query is to be
 * performed when processing a Request instance.
 **/
typedef enum {
  // Map the API's UdsCallbackType values directly to a corresponding action.
  REQUEST_INDEX  = UDS_POST,
  REQUEST_UPDATE = UDS_UPDATE,
  REQUEST_DELETE = UDS_DELETE,
  REQUEST_QUERY  = UDS_QUERY,

  REQUEST_CONTROL,

  // REQUEST_SPARSE_CACHE_BARRIER is the action for the control request used
  // by localIndexRouter.
  REQUEST_SPARSE_CACHE_BARRIER,

  // REQUEST_ANNOUNCE_CHAPTER_CLOSED is the action for the control
  // request used by an indexZone to signal the other zones that it
  // has closed the current open chapter.
  REQUEST_ANNOUNCE_CHAPTER_CLOSED,

  // REQUEST_OPEN through REQUEST_FINISH are the actions for control requests
  // used by remoteIndexRouter.
  REQUEST_OPEN,
  REQUEST_CLOSE,
  REQUEST_GET_CONFIG,
  REQUEST_GET_STATS,
  REQUEST_GET_SERVER_STATUS,
  REQUEST_WRITE,
  REQUEST_FINISH,

  // REQUEST_COLLECT_CONTEXT_STATS messages are sent to the callback thread
  // from a client thread requesting the context statistics.
  REQUEST_COLLECT_CONTEXT_STATS,
} RequestAction;

/**
 * The block's rough location in the index, if any.
 **/
typedef enum {
  /* the block doesn't exist or the location isn't available */
  LOC_UNAVAILABLE,
  /* if the block was found in the open chapter */
  LOC_IN_OPEN_CHAPTER,
  /* if the block was found in the dense part of the index */
  LOC_IN_DENSE,
  /* if the block was found in the sparse part of the index */
  LOC_IN_SPARSE
} IndexRegion;

/**
 * Abstract request pipeline stages, which can also be viewed as stages in the
 * life-cycle of a request.
 **/
typedef enum {
  STAGE_TRIAGE,
  STAGE_INDEX,
  STAGE_CALLBACK,
} RequestStage;

/**
 * Control message fields for the barrier messages used to coordinate the
 * addition of a chapter to the sparse chapter index cache.
 **/
typedef struct barrierMessageData {
  /** virtual chapter number of the chapter index to add to the sparse cache */
  uint64_t      virtualChapter;
} BarrierMessageData;

/**
 * Control message fields for the chapter closed messages used to inform
 * lagging zones of the first zone to close a given open chapter.
 **/
typedef struct chapterClosedMessageData {
  /** virtual chapter number of the chapter which was closed */
  uint64_t      virtualChapter;
} ChapterClosedMessageData;

/**
 * Union of the all the zone control message fields. The RequestAction field
 * (or launch function argument) selects which of the members is valid.
 **/
typedef union zoneMessageData {
  BarrierMessageData barrier;             // for REQUEST_SPARSE_CACHE_BARRIER
  ChapterClosedMessageData chapterClosed; // for REQUEST_ANNOUNCE_CHAPTER_CLOSED
} ZoneMessageData;

typedef struct zoneMessage {
  /** the index to which the message is directed */
  struct index *index;
  /** the message specific data */
  ZoneMessageData data;
} ZoneMessage;

/**
 * Request context for queuing throughout the uds pipeline
 **/
struct request {
  /*
   * The first part of this structure must be exactly parallel to the
   * UdsRequest structure, which is part of the public UDS API.
   */
  UdsChunkName      hash;         // hash value
  UdsChunkData      oldMetadata;  // metadata from index
  UdsChunkData      newMetadata;  // metadata from request
  UdsChunkCallback *callback;     // callback method when complete
  UdsBlockContext   blockContext; // The block context
  UdsCallbackType   type;         // the type of request
  int               status;       // the success or error code for this request
  bool              found;        // True if the block was found in the index
  bool              update;       // move record to newest chapter if found

  /*
   * The remainder of this structure is private to the UDS implementation.
   */
  FunnelQueueEntry  requestQueueLink; // link for lock-free request queue
  STAILQ_ENTRY(request) link;
  AIPContext       *serverContext; // context for AIP only
  UdsContext       *context;    // context for UDS only: callbacks, etc.
  IndexRouter      *router;     // the router handling this request

  // Data for control message requests
  void            *controlData;
  ZoneMessage      zoneMessage;
  bool             isControlMessage;

  bool           unbatched;     // if true, must wake worker when enqueued
  bool           requeued;
  RequestAction  action;        // the action for the index to perform
  unsigned int   zoneNumber;    // the zone for this request to use
  IndexRegion    location;      // if and where the block was found

  bool             slLocationKnown; // slow lane has determined a location
  IndexRegion      slLocation;      // location determined by slowlane

  SynchronousCallback *synchronous; // wait/wake object if request synchronous
};

typedef void (*RequestRestarter)(Request *);

/**
 * Start a request from an API client on a block context.  The request is
 * asynchronous.
 *
 * @param request  The request.
 *
 * @return UDS_SUCCESS or an error code
 **/
int launchAllocatedClientRequest(Request *request)
  __attribute__((warn_unused_result));

/**
 * Make a control message and enqueue it for processing. If the message
 * is synchronous, this will wait until the request has completed before
 * returning.
 *
 * @param serverContext  The AIP context for the message, ownership of which
 *                       is transferred by this call, successful or not
 * @param controlData    A pointer to data required by the control message
 *                       handler
 * @param controlAction  The control action to perform
 * @param initialStage   The pipeline stage that should start processing
 *                       the control request (typically STAGE_INDEX)
 * @param router         The index router responsible for handling the message
 * @param requestPtr     A pointer to hold the new control message request if
 *                       the message is synchronous, otherwise should be NULL
 *
 * @return              UDS_SUCCESS or an error code
 **/
int launchAIPControlMessage(AIPContext     *serverContext,
                            void           *controlData,
                            RequestAction   controlAction,
                            RequestStage    initialStage,
                            IndexRouter    *router,
                            Request       **requestPtr)
  __attribute__((warn_unused_result));

/**
 * Make a control message for an API client and enqueue it for processing. If
 * the message is synchronous, this will wait until the request has completed
 * before returning.
 *
 * @param [in]  context        The client context for the message, ownership of
 *                             which is transferred to the request by this call
 * @param [in]  controlData    A pointer to data required by the control
 *                             message handler
 * @param [in]  controlAction  The control action to perform
 * @param [in]  initialStage   The pipeline stage that should start processing
 *                             the control request (typically STAGE_CALLBACK)
 * @param [out] requestPtr     A pointer to hold the new control request if the
 *                             message is synchronous, otherwise should be NULL
 *
 * @return UDS_SUCCESS or an error code
 **/
int launchClientControlMessage(UdsContext     *context,
                               void           *controlData,
                               RequestAction   controlAction,
                               RequestStage    initialStage,
                               Request       **requestPtr)
  __attribute__((warn_unused_result));

/**
 * Make an asynchronous control message for an index zone and enqueue it for
 * processing.
 *
 * @param action   The control action to perform
 * @param message  The message to send
 * @param zone     The zone number of the zone to receive the message
 * @param router   The index router responsible for handling the message
 *
 * @return UDS_SUCCESS or an error code
 **/
int launchZoneControlMessage(RequestAction  action,
                             ZoneMessage    message,
                             unsigned int   zone,
                             IndexRouter   *router)
  __attribute__((warn_unused_result));

/**
 * Free an index request.
 *
 * @param request The request to free
 **/
void freeRequest(Request *request);

/**
 * Enqueue a request for the next stage of the pipeline. If there is more than
 * one possible queue for a stage, this function uses the request to decide
 * which queue should handle it.
 *
 * @param request       The request to enqueue
 * @param nextStage     The next stage of the pipeline to process the request
 **/
void enqueueRequest(Request *request, RequestStage nextStage);

/**
 * A method to restart delayed requests.
 *
 * @param request    The request to restart
 **/
void restartRequest(Request *request);

/**
 * Set the function pointer which is used to restart requests.
 * This is needed by albserver code and is used as a test hook by the unit
 * tests.
 *
 * @param restarter   The function to call to restart requests.
 **/
void setRequestRestarter(RequestRestarter restarter);

/**
 * Enter the callback stage of processing for a request, notifying the waiting
 * thread if the request is synchronous, freeing the request if it is an
 * asynchronous control message, or placing it on the callback queue if it is
 * an asynchronous client request.
 *
 * @param request  the request which has completed execution
 **/
void enterCallbackStage(Request *request);

/**
 * Update the context statistics to reflect the successful completion of a
 * client request.
 *
 * @param request  a client request that has successfully completed execution
 **/
void updateRequestContextStats(Request *request);

/**
 * Compute the CacheProbeType value reflecting the request and page type.
 *
 * @param request      The request being processed, or NULL
 * @param isIndexPage  Whether the cache probe will be for an index page
 *
 * @return the cache probe type enumeration
 **/
static INLINE CacheProbeType cacheProbeType(Request *request,
                                            bool     isIndexPage)
{
  if ((request != NULL) && request->requeued) {
    return isIndexPage ? CACHE_PROBE_INDEX_RETRY : CACHE_PROBE_RECORD_RETRY;
  } else {
    return isIndexPage ? CACHE_PROBE_INDEX_FIRST : CACHE_PROBE_RECORD_FIRST;
  }
}
#endif /* REQUEST_H */
