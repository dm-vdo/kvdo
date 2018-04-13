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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/waitQueue.h#1 $
 */

#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include "common.h"

/**
 * A wait queue is a circular list of entries waiting to be notified of a
 * change in a condition. Keeping a circular list allows the queue structure
 * to simply be a pointer to the tail (newest) entry in the queue, supporting
 * constant-time enqueue and dequeue operations. A null pointer is an empty
 * queue.
 *
 *   An empty queue:
 *     queue0.lastWaiter -> NULL
 *
 *   A singleton queue:
 *     queue1.lastWaiter -> entry1 -> entry1 -> [...]
 *
 *   A three-element queue:
 *     queue2.lastWaiter -> entry3 -> entry1 -> entry2 -> entry3 -> [...]
 **/

typedef struct waiter Waiter;

typedef struct {
  /** The tail of the queue, the last (most recently added) entry */
  Waiter *lastWaiter;
  /** The number of waiters currently in the queue */
  size_t  queueLength;
} WaitQueue;

/**
 * Callback type for functions which will be called to resume processing of a
 * waiter after it has been removed from its wait queue.
 **/
typedef void WaiterCallback(Waiter *waiter, void *context);

/**
 * Method type for Waiter matching methods.
 *
 * A WaiterMatch method returns false if the waiter does not match.
 **/
typedef bool WaiterMatch(Waiter *waiter, void *context);

/**
 * The queue entry structure for entries in a WaitQueue.
 **/
struct waiter {
  /**
   * The next waiter in the queue. If this entry is the last waiter, then this
   * is actually a pointer back to the head of the queue.
   **/
  struct waiter  *nextWaiter;

  /** Optional waiter-specific callback to invoke when waking this waiter. */
  WaiterCallback *callback;
};

/**
 * Check whether a Waiter is waiting.
 *
 * @param waiter  The waiter to check
 *
 * @return <code>true</code> if the waiter is on some WaitQueue
 **/
static inline bool isWaiting(Waiter *waiter)
{
  return (waiter->nextWaiter != NULL);
}

/**
 * Initialize a wait queue.
 *
 * @param queue  The queue to initialize
 **/
static inline void initializeWaitQueue(WaitQueue *queue)
{
  *queue = (WaitQueue) {
    .lastWaiter  = NULL,
    .queueLength = 0,
  };
}

/**
 * Check whether a wait queue has any entries waiting in it.
 *
 * @param queue  The queue to query
 *
 * @return <code>true</code> if there are any waiters in the queue
 **/
__attribute__((warn_unused_result))
static inline bool hasWaiters(const WaitQueue *queue)
{
  return (queue->lastWaiter != NULL);
}

/**
 * Add a waiter to the tail end of a wait queue. The waiter must not already
 * be waiting in a queue.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The waiter to add to the queue
 *
 * @return VDO_SUCCESS or an error code
 **/
int enqueueWaiter(WaitQueue *queue, Waiter *waiter)
  __attribute__((warn_unused_result));

/**
 * Notify all the entries waiting in a queue to continue execution by invoking
 * a callback function on each of them in turn. The queue is copied and
 * emptied before invoking any callbacks, and only the waiters that were in
 * the queue at the start of the call will be notified.
 *
 * @param queue     The wait queue containing the waiters to notify
 * @param callback  The function to call to notify each waiter, or NULL
 *                  to invoke the callback field registered in each waiter
 * @param context   The context to pass to the callback function
 **/
void notifyAllWaiters(WaitQueue      *queue,
                      WaiterCallback *callback,
                      void           *context);

/**
 * Notify the next entry waiting in a queue to continue execution by invoking
 * a callback function on it after removing it from the queue.
 *
 * @param queue     The wait queue containing the waiter to notify
 * @param callback  The function to call to notify the waiter, or NULL
 *                  to invoke the callback field registered in the waiter
 * @param context   The context to pass to the callback function
 *
 * @return <code>true</code> if there was a waiter in the queue
 **/
bool notifyNextWaiter(WaitQueue      *queue,
                      WaiterCallback *callback,
                      void           *context);

/**
 * Transfer all waiters from one wait queue to a second queue, emptying the
 * first queue.
 *
 * @param fromQueue  The queue containing the waiters to move
 * @param toQueue    The queue that will receive the waiters from the
 *                   the first queue
 **/
void transferAllWaiters(WaitQueue *fromQueue, WaitQueue *toQueue);

/**
 * Return the waiter that is at the head end of a wait queue.
 *
 * @param queue  The queue from which to get the first waiter
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
Waiter *getFirstWaiter(const WaitQueue *queue);

/**
 * Remove all waiters that match based on the specified matching method and
 * append them to a WaitQueue.
 *
 * @param queue         The wait queue to process
 * @param matchMethod   The method to determine matching
 * @param matchContext  Contextual info for the match method
 * @param matchedQueue  A WaitQueue to store matches
 *
 * @return VDO_SUCCESS or an error code
 **/
int dequeueMatchingWaiters(WaitQueue   *queue,
                           WaiterMatch *matchMethod,
                           void        *matchContext,
                           WaitQueue   *matchedQueue);

/**
 * Remove the first waiter from the head end of a wait queue. The caller will
 * be responsible for waking the waiter by invoking the correct callback
 * function to resume its execution.
 *
 * @param queue  The wait queue from which to remove the first entry
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
Waiter *dequeueNextWaiter(WaitQueue *queue);

/**
 * Count the number of waiters in a wait queue.
 *
 * @param queue  The wait queue to query
 *
 * @return the number of waiters in the queue
 **/
__attribute__((warn_unused_result))
static inline size_t countWaiters(const WaitQueue *queue)
{
  return queue->queueLength;
}

/**
 * Get the waiter after this one, for debug iteration.
 *
 * @param queue   The wait queue
 * @param waiter  A waiter
 *
 * @return the next waiter, or NULL
 **/
const Waiter *getNextWaiter(const WaitQueue *queue, const Waiter *waiter)
  __attribute__((warn_unused_result));

#endif // WAIT_QUEUE_H
