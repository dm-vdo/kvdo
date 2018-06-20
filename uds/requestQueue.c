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
 * $Id: //eng/uds-releases/gloria/src/uds/requestQueue.c#3 $
 */

#include "requestQueue.h"

#include "atomicDefs.h"
#include "logger.h"
#include "permassert.h"
#include "request.h"
#include "memoryAlloc.h"
#include "threads.h"
#include "timeUtils.h"
#include "util/eventCount.h"
#include "util/funnelQueue.h"

/*
 * Ordering:
 *
 * Multiple retry requests or multiple non-retry requests enqueued
 * from a single producer thread will be processed in the order
 * enqueued.
 *
 * Retry requests will generally be processed before normal requests.
 *
 * HOWEVER, a producer thread can enqueue a retry request (generally
 * given higher priority) and then enqueue a normal request, and they
 * can get processed in the reverse order.  The checking of the two
 * internal queues is very simple and there's a potential race with
 * the producer regarding the "priority" handling.  If an ordering
 * guarantee is needed, it can be added without much difficulty, it
 * just makes the code a bit more complicated.
 *
 * If requests are enqueued while the processing of another request is
 * happening, and the enqueuing operations complete while the request
 * processing is still in progress, then the retry request(s) *will*
 * get processed next.  (This is used for testing.)
 */

/**
 * Time constants, all in units of nanoseconds.
 **/
enum {
  ONE_NANOSECOND    =    1,
  ONE_MICROSECOND   = 1000 * ONE_NANOSECOND,
  ONE_MILLISECOND   = 1000 * ONE_MICROSECOND,
  ONE_SECOND        = 1000 * ONE_MILLISECOND,

  /** The initial time to wait after waiting with no timeout */
  DEFAULT_WAIT_TIME = 10 * ONE_MICROSECOND,

  /** The minimum time to wait when waiting with a timeout */
  MINIMUM_WAIT_TIME = DEFAULT_WAIT_TIME / 2,

  /** The maximimum time to wait when waiting with a timeout */
  MAXIMUM_WAIT_TIME = ONE_MILLISECOND
};

/**
 * Batch size tuning constants. These are compared to the number of requests
 * that have been processed since the worker thread last woke up.
 **/
enum {
  MINIMUM_BATCH = 32,  // wait time increases if batches are smaller than this
  MAXIMUM_BATCH = 64   // wait time decreases if batches are larger than this
};

struct requestQueue {
  const char            *name;       // name of queue
  RequestQueueProcessor *processOne; // function to process 1 request

  FunnelQueue *mainQueue;       // new incoming requests
  FunnelQueue *retryQueue;      // old requests to retry first
  EventCount  *workEvent;       // signal to wake the worker thread

  Thread thread;                // thread id of the worker thread
  bool   started;               // true if the worker was started

  bool alive;                   // when true, requests can be enqueued

  /** A flag set when the worker is waiting without a timeout */
  atomic_t dormant;

  // The following fields are mutable state private to the worker thread. The
  // first field is aligned to avoid cache line sharing with preceding fields.

  /** requests processed since last wait */
  uint64_t currentBatch __attribute__((aligned(CACHE_LINE_BYTES)));

  /** the amount of time to wait to accumulate a batch of requests */
  uint64_t waitNanoseconds;

  /** the relative time at which to wake when waiting with a timeout */
  RelTime wakeRelTime;
};

/**
 * Adjust the wait time if the last batch of requests was larger or smaller
 * than the tuning constants.
 *
 * @param queue  the request queue
 **/
static void adjustWaitTime(RequestQueue *queue)
{
  // Adjust the wait time by about 25%.
  uint64_t delta = queue->waitNanoseconds / 4;

  if (queue->currentBatch < MINIMUM_BATCH) {
    // Batch too small, so increase the wait a little.
    queue->waitNanoseconds += delta;
  } else if (queue->currentBatch > MAXIMUM_BATCH) {
    // Batch too large, so decrease the wait a little.
    queue->waitNanoseconds -= delta;
  }
}

/**
 * Decide if the queue should wait with a timeout or enter the dormant mode
 * of waiting without a timeout. If timing out, returns an relative wake
 * time to pass to the wait call, otherwise returns NULL. (wakeRelTime is a
 * queue field to make it easy for this function to return NULL).
 *
 * @param queue  the request queue
 *
 * @return a pointer the relative wake time, or NULL if there is no timeout
 **/
static RelTime *getWakeTime(RequestQueue *queue)
{
  if (queue->waitNanoseconds >= MAXIMUM_WAIT_TIME) {
    if (atomic_read(&queue->dormant)) {
      // The dormant flag was set on the last timeout cycle and nothing
      // changed, so wait with no timeout and reset the wait time.
      queue->waitNanoseconds = DEFAULT_WAIT_TIME;
      return NULL;
    }
    // Wait one time with the dormant flag set, ensuring that enqueuers will
    // have a chance to see that the flag is set.
    queue->waitNanoseconds = MAXIMUM_WAIT_TIME;
    atomic_set_release(&queue->dormant, true);
  } else if (queue->waitNanoseconds < MINIMUM_WAIT_TIME) {
    // If the producer is very fast or the scheduler just doesn't wake us
    // promptly, waiting for very short times won't make the batches smaller.
    queue->waitNanoseconds = MINIMUM_WAIT_TIME;
  }

  RelTime *wake = &queue->wakeRelTime;
  *wake = nanosecondsToRelTime(queue->waitNanoseconds);
  return wake;
}

/**********************************************************************/
static Request *removeHead(FunnelQueue *queue)
{
  FunnelQueueEntry *entry = funnelQueuePoll(queue);
  if (entry == NULL) {
    return NULL;
  }
  return container_of(entry, Request, requestQueueLink);
}

/**
 * Poll the underlying lock-free queues for a request to process. Requests in
 * the retry queue have higher priority, so that queue is polled first.
 *
 * @param queue  the RequestQueue being serviced
 *
 * @return a dequeued request, or NULL if no request was available
 **/
static Request *pollQueues(RequestQueue *queue)
{
  Request *request = removeHead(queue->retryQueue);
  if (request == NULL) {
    request = removeHead(queue->mainQueue);
  }
  return request;
}

/**
 * Remove the next request to be processed from the queue, waiting for a
 * request if the queue is empty. Must only be called by the worker thread.
 *
 * @param queue  the queue from which to remove an entry
 *
 * @return the next request in the queue, or NULL if the queue has been
 *         shut down and the worker thread should exit
 **/
static Request *dequeueRequest(RequestQueue *queue)
{
  for (;;) {
    // Assume we'll find a request to return; if not, it'll be zeroed later.
    queue->currentBatch += 1;

    // Fast path: pull an item off a non-blocking queue and return it.
    Request *request = pollQueues(queue);
    if (request != NULL) {
      return request;
    }

    // Looks like there's no work. Prepare to wait for more work. If the
    // EventCount is signalled after this returns, we won't wait later on.
    EventToken waitToken = eventCountPrepare(queue->workEvent);

    // First poll for shutdown to ensure we don't miss work that was enqueued
    // immediately before a shutdown request.
    bool shuttingDown = !READ_ONCE(queue->alive);
    if (shuttingDown) {
      /*
       * Ensure that we see any requests that were guaranteed to have been
       * fully enqueued before shutdown was flagged.  The corresponding write
       * barrier is in requestQueueFinish.
       */
      smp_rmb();
    }

    // Poll again before waiting--a request may have been enqueued just before
    // we got the event key.
    request = pollQueues(queue);
    if ((request != NULL) || shuttingDown) {
      eventCountCancel(queue->workEvent, waitToken);
      return request;
    }

    // We're about to wait again, so update the wait time to reflect the batch
    // of requests we processed since the last wait.
    adjustWaitTime(queue);

    // If the EventCount hasn't been signalled since we got the waitToken,
    // wait until it is signalled or until the wait times out.
    RelTime *wakeTime = getWakeTime(queue);
    eventCountWait(queue->workEvent, waitToken, wakeTime);

    if (wakeTime == NULL) {
      // We've been roused from dormancy. Clear the flag so enqueuers can stop
      // broadcasting (no fence needed for this transition).
      atomic_set(&queue->dormant, false);
      // Reset the timeout back to the default since we don't know how long
      // we've been asleep and we also want to be responsive to a new burst.
      queue->waitNanoseconds = DEFAULT_WAIT_TIME;
    }

    // Just finished waiting, so start counting a new batch.
    queue->currentBatch = 0;
  }
}

/**********************************************************************/
static void requestQueueWorker(void *arg)
{
  RequestQueue *queue = (RequestQueue *) arg;
  logDebug("%s queue starting", queue->name);
  Request *request;
  while ((request = dequeueRequest(queue)) != NULL) {
    queue->processOne(request);
  }
  logDebug("%s queue done", queue->name);
}

/**********************************************************************/
static int initializeQueue(RequestQueue          *queue,
                           const char            *queueName,
                           RequestQueueProcessor *processOne)
{
  queue->name            = queueName;
  queue->processOne      = processOne;
  queue->alive           = true;
  queue->currentBatch    = 0;
  queue->waitNanoseconds = DEFAULT_WAIT_TIME;

  int result = makeFunnelQueue(&queue->mainQueue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeFunnelQueue(&queue->retryQueue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeEventCount(&queue->workEvent);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = createThread(requestQueueWorker, queue, queueName, &queue->thread);
  if (result != UDS_SUCCESS) {
    return result;
  }

  queue->started = true;
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeRequestQueue(const char             *queueName,
                     RequestQueueProcessor  *processOne,
                     RequestQueue          **queuePtr)
{
  RequestQueue *queue;
  int result = ALLOCATE(1, struct requestQueue, "request queue", &queue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initializeQueue(queue, queueName, processOne);
  if (result != UDS_SUCCESS) {
    requestQueueFinish(queue);
    return result;
  }

  *queuePtr = queue;
  return UDS_SUCCESS;
}

/**********************************************************************/
void requestQueueEnqueue(RequestQueue *queue, Request *request)
{
  bool unbatched = request->unbatched;
  funnelQueuePut(request->requeued ? queue->retryQueue : queue->mainQueue,
                 &request->requestQueueLink);

  /*
   * We must wake the worker thread when it is dormant (waiting with no
   * timeout). An atomic load (read fence) isn't needed here since we know the
   * queue operation acts as one.
   */
  if (atomic_read(&queue->dormant) || unbatched) {
    eventCountBroadcast(queue->workEvent);
  }
}

/**********************************************************************/
void requestQueueFinish(RequestQueue *queue)
{
  if (queue == NULL) {
    return;
  }

  /*
   * This memory barrier ensures that any requests we queued will be seen.  The
   * point is that when dequeueRequest sees the following update to the alive
   * flag, it will also be able to see any change we made to a next field in
   * the FunnelQueue entry.  The corresponding read barrier is in
   * dequeueRequest.
   */
  smp_wmb();

  // Mark the queue as dead.
  WRITE_ONCE(queue->alive, false);

  if (queue->started) {
    // Wake the worker so it notices that it should exit.
    eventCountBroadcast(queue->workEvent);

    // Wait for the worker thread to finish processing any additional pending
    // work and exit.
    int result = joinThreads(queue->thread);
    if (result != 0) {
      logErrorWithStringError(result, "Failed to join worker thread");
    }
  }

  freeEventCount(queue->workEvent);
  freeFunnelQueue(queue->mainQueue);
  freeFunnelQueue(queue->retryQueue);
  FREE(queue);
}
