/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/requestQueueKernel.c#3 $
 */

#include "requestQueue.h"

#include <linux/wait.h>

#include "atomicDefs.h"
#include "compiler.h"
#include "logger.h"
#include "request.h"
#include "memoryAlloc.h"
#include "threads.h"
#include "util/funnelQueue.h"

/*
 * Ordering:
 *
 * Multiple retry requests or multiple non-retry requests enqueued from
 * a single producer thread will be processed in the order enqueued.
 *
 * Retry requests will generally be processed before normal requests.
 *
 * HOWEVER, a producer thread can enqueue a retry request (generally given
 * higher priority) and then enqueue a normal request, and they can get
 * processed in the reverse order.  The checking of the two internal queues is
 * very simple and there's a potential race with the producer regarding the
 * "priority" handling.  If an ordering guarantee is needed, it can be added
 * without much difficulty, it just makes the code a bit more complicated.
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
  DEFAULT_WAIT_TIME = 20 * ONE_MICROSECOND,

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
  /* Wait queue for synchronizing producers and consumer */
  struct wait_queue_head  wqhead;
  /* function to process 1 request */
  RequestQueueProcessor  *processOne;
  /* new incoming requests */
  FunnelQueue            *mainQueue;
  /* old requests to retry first */
  FunnelQueue            *retryQueue;
  /* thread id of the worker thread */
  Thread                  thread;
  /* true if the worker was started */
  bool                    started;
  /* when true, requests can be enqueued */
  bool                    alive;
  /* A flag set when the worker is waiting without a timeout */
  atomic_t                dormant;
};

/*****************************************************************************/
/**
 * Poll the underlying lock-free queues for a request to process.  Must only be
 * called by the worker thread.
 *
 * @param queue  the RequestQueue being serviced
 *
 * @return a dequeued request, or NULL if no request was available
 **/
static INLINE Request *pollQueues(RequestQueue *queue)
{
  // The retry queue has higher priority.
  FunnelQueueEntry *entry = funnelQueuePoll(queue->retryQueue);
  if (entry != NULL) {
    return container_of(entry, Request, requestQueueLink);
  }

  // The main queue has lower priority.
  entry = funnelQueuePoll(queue->mainQueue);
  if (entry != NULL) {
    return container_of(entry, Request, requestQueueLink);
  }
  
  // No entry found.
  return NULL;
}

/*****************************************************************************/
/**
 * Check if the underlying lock-free queues appear not just not to have any
 * requests available right now, but also not to be in the intermediate state
 * of getting requests added. Must only be called by the worker thread.
 *
 * @param queue  the RequestQueue being serviced
 *
 * @return true iff both funnel queues are idle
 **/
static INLINE bool areQueuesIdle(RequestQueue *queue)
{
  return (isFunnelQueueIdle(queue->retryQueue) &&
          isFunnelQueueIdle(queue->mainQueue));
}

/*****************************************************************************/
/**
 * Remove the next request to be processed from the queue.  Must only be called
 * by the worker thread.
 *
 * @param queue       the queue from which to remove an entry
 * @param requestPtr  the next request is returned here, or a NULL pointer to
 *                    indicate that there will be no more requests
 * @param waitedPtr   return a boolean to indicate that we need to wait
 *
 * @return True when there is a next request, or when we know that there will
 *         never be another request.  False when we must wait for a request.
 **/
static INLINE bool dequeueRequest(RequestQueue  *queue,
                                  Request      **requestPtr,
                                  bool          *waitedPtr)
{
  // Because of batching, we expect this to be the most common code path.
  Request *request = pollQueues(queue);
  if (request != NULL) {
    // Return because we found a request
    *requestPtr = request;
    return true;
  }

  if (!READ_ONCE(queue->alive)) {
    // Return because we see that shutdown is happening
    *requestPtr = NULL;
    return true;
  }

  // Return indicating that we need to wait.
  *requestPtr = NULL;
  *waitedPtr = true;
  return false;
}

/*****************************************************************************/
static void requestQueueWorker(void *arg)
{
  RequestQueue *queue = (RequestQueue *) arg;
  unsigned long timeBatch = DEFAULT_WAIT_TIME;
  bool dormant = atomic_read(&queue->dormant);
  long currentBatch = 0;

  for (;;) {
    Request *request;
    bool waited = false;
    if (dormant) {
      /*
       * Sleep/wakeup protocol:
       *
       * The enqueue operation updates "newest" in the
       * funnel queue via xchg which is a memory barrier,
       * and later checks "dormant" to decide whether to do
       * a wakeup of the worker thread.
       *
       * The worker thread, when deciding to go to sleep,
       * sets "dormant" and then examines "newest" to decide
       * if the funnel queue is idle. In dormant mode, the
       * last examination of "newest" before going to sleep
       * is done inside the wait_event_interruptible macro,
       * after a point where (one or more) memory barriers
       * have been issued. (Preparing to sleep uses spin
       * locks.) Even if the "next" field update isn't
       * visible yet to make the entry accessible, its
       * existence will kick the worker thread out of
       * dormant mode and back into timer-based mode.
       *
       * So the two threads should agree on the ordering of
       * the updating of the two fields.
       */
      wait_event_interruptible(queue->wqhead,
                               dequeueRequest(queue, &request, &waited) ||
                               !areQueuesIdle(queue));
    } else {
      wait_event_interruptible_hrtimeout(queue->wqhead,
                                         dequeueRequest(queue, &request,
                                                        &waited),
                                         ns_to_ktime(timeBatch));
    }

    if (likely(request != NULL)) {
      // We got a request.
      currentBatch++;
      queue->processOne(request);
    } else if (!READ_ONCE(queue->alive)) {
      // We got no request and we know we are shutting down.
      break;
    }

    if (dormant) {
      // We've been roused from dormancy. Clear the flag so enqueuers can stop
      // broadcasting (no fence needed for this transition).
      atomic_set(&queue->dormant, false);
      dormant = false;
      // Reset the timeout back to the default since we don't know how long
      // we've been asleep and we also want to be responsive to a new burst.
      timeBatch = DEFAULT_WAIT_TIME;
    } else if (waited) {
      // We waited for this request to show up.  Adjust the wait time if the
      // last batch of requests was too small or too large..
      if (currentBatch < MINIMUM_BATCH) {
        // Adjust the wait time if the last batch of requests was too small.
        timeBatch += timeBatch / 4;
        if (timeBatch >= MAXIMUM_WAIT_TIME) {
          // The timeout is getting long enough that we need to switch into
          // dormant mode.
          atomic_set(&queue->dormant, true);
          dormant = true;
        }
      } else if (currentBatch > MAXIMUM_BATCH) {
        // Adjust the wait time if the last batch of requests was too large.
        timeBatch -= timeBatch / 4;
        if (timeBatch < MINIMUM_WAIT_TIME) {
          // But if the producer is very fast or the scheduler doesn't wake up
          // up promptly, waiting for very short times won't make the batches
          // smaller.
          timeBatch = MINIMUM_WAIT_TIME;
        }
      }
      // And we must now start a new batch count
      currentBatch = 0;
    }
  }

  /*
   * Ensure that we see any requests that were guaranteed to have been fully
   * enqueued before shutdown was flagged.  The corresponding write barrier
   * is in requestQueueFinish.
   */
  smp_rmb();

  // Process every request that is still in the queue, and never wait for any
  // new requests to show up.
  for (;;) {
    Request *request = pollQueues(queue);
    if (request == NULL) {
      break;
    }
    queue->processOne(request);
  }
}

/**********************************************************************/
int makeRequestQueue(const char             *queueName,
                     RequestQueueProcessor  *processOne,
                     RequestQueue          **queuePtr)
{
  RequestQueue *queue;
  int result = ALLOCATE(1, RequestQueue, __func__, &queue);
  if (result != UDS_SUCCESS) {
    return result;
  }
  queue->processOne = processOne;
  queue->alive      = true;
  atomic_set(&queue->dormant, false);
  init_waitqueue_head(&queue->wqhead);

  result = makeFunnelQueue(&queue->mainQueue);
  if (result != UDS_SUCCESS) {
    requestQueueFinish(queue);
    return result;
  }

  result = makeFunnelQueue(&queue->retryQueue);
  if (result != UDS_SUCCESS) {
    requestQueueFinish(queue);
    return result;
  }

  result = createThread(requestQueueWorker, queue, queueName, &queue->thread);
  if (result != UDS_SUCCESS) {
    requestQueueFinish(queue);
    return result;
  }

  queue->started = true;
  smp_mb();
  *queuePtr = queue;
  return UDS_SUCCESS;
}

/**********************************************************************/
static INLINE void wakeUpWorker(RequestQueue *queue)
{
  // This is the code sequence recommended in <linux/wait.h>
  smp_mb();
  if (waitqueue_active(&queue->wqhead)) {
    wake_up(&queue->wqhead);
  }
}

/**********************************************************************/
void requestQueueEnqueue(RequestQueue *queue, Request *request)
{
  bool unbatched = request->unbatched;
  funnelQueuePut(request->requeued ? queue->retryQueue : queue->mainQueue,
                 &request->requestQueueLink);

  /*
   * We must wake the worker thread when it is dormant (waiting with no
   * timeout).  An atomic load (read fence) isn't needed here since we know the
   * queue operation acts as one.
   */
  if (atomic_read(&queue->dormant) || unbatched) {
    wakeUpWorker(queue);
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
   * requestQueueWorker.
   */
  smp_wmb();

  // Mark the queue as dead.
  WRITE_ONCE(queue->alive, false);

  if (queue->started) {
    // Wake the worker so it notices that it should exit.
    wakeUpWorker(queue);

    // Wait for the worker thread to finish processing any additional pending
    // work and exit.
    int result = joinThreads(queue->thread);
    if (result != UDS_SUCCESS) {
      logWarningWithStringError(result, "Failed to join worker thread");
    }
  }

  freeFunnelQueue(queue->mainQueue);
  freeFunnelQueue(queue->retryQueue);
  FREE(queue);
}
