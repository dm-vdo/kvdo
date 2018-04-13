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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/waitQueue.c#1 $
 */

#include "waitQueue.h"

#include "permassert.h"

#include "statusCodes.h"

/**********************************************************************/
int enqueueWaiter(WaitQueue *queue, Waiter *waiter)
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
void transferAllWaiters(WaitQueue *fromQueue, WaitQueue *toQueue)
{
  // If the source queue is empty, there's nothing to do.
  if (!hasWaiters(fromQueue)) {
    return;
  }

  if (hasWaiters(toQueue)) {
    // Both queues are non-empty. Splice the two circular lists together by
    // swapping the next (head) pointers in the list tails.
    Waiter *fromHead = fromQueue->lastWaiter->nextWaiter;
    Waiter *toHead   = toQueue->lastWaiter->nextWaiter;
    toQueue->lastWaiter->nextWaiter   = fromHead;
    fromQueue->lastWaiter->nextWaiter = toHead;
  }

  toQueue->lastWaiter   = fromQueue->lastWaiter;
  toQueue->queueLength += fromQueue->queueLength;
  initializeWaitQueue(fromQueue);
}

/**********************************************************************/
void notifyAllWaiters(WaitQueue      *queue,
                      WaiterCallback *callback,
                      void           *context)
{
  // Copy and empty the queue first, avoiding the possibility of an infinite
  // loop if entries are returned to the queue by the callback function.
  WaitQueue waiters;
  initializeWaitQueue(&waiters);
  transferAllWaiters(queue, &waiters);

  // Drain the copied queue, invoking the callback on every entry.
  while (notifyNextWaiter(&waiters, callback, context)) {
    // All the work is done by the loop condition.
  }
}

/**********************************************************************/
Waiter *getFirstWaiter(const WaitQueue *queue)
{
  Waiter *lastWaiter = queue->lastWaiter;
  if (lastWaiter == NULL) {
    // There are no waiters, so we're done.
    return NULL;
  }

  // The queue is circular, so the last entry links to the head of the queue.
  return lastWaiter->nextWaiter;
}

/**********************************************************************/
int dequeueMatchingWaiters(WaitQueue   *queue,
                           WaiterMatch *matchMethod,
                           void        *matchContext,
                           WaitQueue   *matchedQueue)
{
  WaitQueue matchedWaiters;
  initializeWaitQueue(&matchedWaiters);

  WaitQueue iterationQueue;
  initializeWaitQueue(&iterationQueue);
  transferAllWaiters(queue, &iterationQueue);
  while (hasWaiters(&iterationQueue)) {
    Waiter *waiter = dequeueNextWaiter(&iterationQueue);
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
Waiter *dequeueNextWaiter(WaitQueue *queue)
{
  Waiter *firstWaiter = getFirstWaiter(queue);
  if (firstWaiter == NULL) {
    return NULL;
  }

  Waiter *lastWaiter = queue->lastWaiter;
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
bool notifyNextWaiter(WaitQueue      *queue,
                      WaiterCallback *callback,
                      void           *context)
{
  Waiter *waiter = dequeueNextWaiter(queue);
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
const Waiter *getNextWaiter(const WaitQueue *queue, const Waiter *waiter)
{
  Waiter *firstWaiter = getFirstWaiter(queue);
  if (waiter == NULL) {
    return firstWaiter;
  }
  return ((waiter->nextWaiter != firstWaiter) ? waiter->nextWaiter : NULL);
}
