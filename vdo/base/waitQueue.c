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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/waitQueue.c#3 $
 */

#include "waitQueue.h"

#include "permassert.h"

#include "statusCodes.h"

/**********************************************************************/
int enqueueWaiter(struct wait_queue *queue, struct waiter *waiter)
{
  int result = ASSERT((waiter->nextWaiter == NULL),
                      "new waiter must not already be in a waiter queue");
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (queue->lastWaiter == NULL) {
    // The queue is empty, so form the initial circular list by self-linking
    // the initial waiter.
    waiter->nextWaiter = waiter;
  } else {
    // Splice the new waiter in at the end of the queue.
    waiter->nextWaiter = queue->lastWaiter->nextWaiter;
    queue->lastWaiter->nextWaiter = waiter;
  }
  // In both cases, the waiter we added to the ring becomes the last waiter.
  queue->lastWaiter   = waiter;
  queue->queueLength += 1;
  return VDO_SUCCESS;
}

/**********************************************************************/
void transferAllWaiters(struct wait_queue *fromQueue, struct wait_queue *toQueue)
{
  // If the source queue is empty, there's nothing to do.
  if (!hasWaiters(fromQueue)) {
    return;
  }

  if (hasWaiters(toQueue)) {
    // Both queues are non-empty. Splice the two circular lists together by
    // swapping the next (head) pointers in the list tails.
    struct waiter *fromHead = fromQueue->lastWaiter->nextWaiter;
    struct waiter *toHead   = toQueue->lastWaiter->nextWaiter;
    toQueue->lastWaiter->nextWaiter   = fromHead;
    fromQueue->lastWaiter->nextWaiter = toHead;
  }

  toQueue->lastWaiter   = fromQueue->lastWaiter;
  toQueue->queueLength += fromQueue->queueLength;
  initializeWaitQueue(fromQueue);
}

/**********************************************************************/
void notifyAllWaiters(struct wait_queue *queue,
                      WaiterCallback    *callback,
                      void              *context)
{
  // Copy and empty the queue first, avoiding the possibility of an infinite
  // loop if entries are returned to the queue by the callback function.
  struct wait_queue waiters;
  initializeWaitQueue(&waiters);
  transferAllWaiters(queue, &waiters);

  // Drain the copied queue, invoking the callback on every entry.
  while (notifyNextWaiter(&waiters, callback, context)) {
    // All the work is done by the loop condition.
  }
}

/**********************************************************************/
struct waiter *getFirstWaiter(const struct wait_queue *queue)
{
  struct waiter *lastWaiter = queue->lastWaiter;
  if (lastWaiter == NULL) {
    // There are no waiters, so we're done.
    return NULL;
  }

  // The queue is circular, so the last entry links to the head of the queue.
  return lastWaiter->nextWaiter;
}

/**********************************************************************/
int dequeueMatchingWaiters(struct wait_queue *queue,
                           WaiterMatch       *matchMethod,
                           void              *matchContext,
                           struct wait_queue *matchedQueue)
{
  struct wait_queue matchedWaiters;
  initializeWaitQueue(&matchedWaiters);

  struct wait_queue iterationQueue;
  initializeWaitQueue(&iterationQueue);
  transferAllWaiters(queue, &iterationQueue);
  while (hasWaiters(&iterationQueue)) {
    struct waiter *waiter = dequeueNextWaiter(&iterationQueue);
    int     result = VDO_SUCCESS;
    if (!matchMethod(waiter, matchContext)) {
      result = enqueueWaiter(queue, waiter);
    } else {
      result = enqueueWaiter(&matchedWaiters, waiter);
    }
    if (result != VDO_SUCCESS) {
      transferAllWaiters(&matchedWaiters, queue);
      transferAllWaiters(&iterationQueue, queue);
      return result;
    }
  }

  transferAllWaiters(&matchedWaiters, matchedQueue);
  return VDO_SUCCESS;
}

/**********************************************************************/
struct waiter *dequeueNextWaiter(struct wait_queue *queue)
{
  struct waiter *firstWaiter = getFirstWaiter(queue);
  if (firstWaiter == NULL) {
    return NULL;
  }

  struct waiter *lastWaiter = queue->lastWaiter;
  if (firstWaiter == lastWaiter) {
    // The queue has a single entry, so just empty it out by nulling the tail.
    queue->lastWaiter = NULL;
  } else {
    // The queue has more than one entry, so splice the first waiter out of
    // the circular queue.
    lastWaiter->nextWaiter = firstWaiter->nextWaiter;
  }

  // The waiter is no longer in a wait queue.
  firstWaiter->nextWaiter  = NULL;
  queue->queueLength      -= 1;
  return firstWaiter;
}

/**********************************************************************/
bool notifyNextWaiter(struct wait_queue *queue,
                      WaiterCallback    *callback,
                      void              *context)
{
  struct waiter *waiter = dequeueNextWaiter(queue);
  if (waiter == NULL) {
    return false;
  }

  if (callback == NULL) {
    callback = waiter->callback;
  }
  (*callback)(waiter, context);
  return true;
}

/**********************************************************************/
const struct waiter *getNextWaiter(const struct wait_queue *queue,
                                   const struct waiter     *waiter)
{
  struct waiter *firstWaiter = getFirstWaiter(queue);
  if (waiter == NULL) {
    return firstWaiter;
  }
  return ((waiter->nextWaiter != firstWaiter) ? waiter->nextWaiter : NULL);
}
