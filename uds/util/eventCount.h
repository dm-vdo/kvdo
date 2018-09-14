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
 * $Id: //eng/uds-releases/gloria/src/uds/util/eventCount.h#2 $
 */

#ifndef EVENT_COUNT_H
#define EVENT_COUNT_H

#include "timeUtils.h"
#include "typeDefs.h"

/**
 * An EventCount is a lock-free equivalent of a condition variable.
 *
 * Using an EventCount, a lock-free producer/consumer can wait for a state
 * change (adding an item to an empty queue, for example) without spinning or
 * falling back on the use of mutex-based locks. Signalling is cheap when
 * there are no waiters (a memory fence), and preparing to wait is
 * also inexpensive (an atomic add instruction).
 *
 * A lock-free producer should call eventCountBroadcast() after any mutation
 * to the lock-free data structure that a consumer might be waiting on. The
 * consumers should poll for work like this:
 *
 *   for (;;) {
 *     // Fast path--no additional cost to consumer.
 *     if (lockFreeDequeue(&item)) {
 *       return item;
 *     }
 *     // Two-step wait: get current token and poll state, either cancelling
 *     // the wait or waiting for the token to be signalled.
 *     EventToken token = eventCountPrepare(ec);
 *     if (lockFreeDequeue(&item)) {
 *       eventCountCancel(ec, token);
 *       return item;
 *     }
 *     eventCountWait(ec, token, NULL);
 *     // State has changed, but must check condition again, so loop.
 *   }
 *
 * Once eventCountPrepare() is called, the caller should neither dally, sleep,
 * nor perform long-running or blocking actions before passing the token to
 * eventCountCancel() or eventCountWait(). The implementation is optimized for
 * a short polling window, and will not perform well if there are outstanding
 * tokens that have been signalled but not waited upon.
 **/

typedef struct eventCount EventCount;

typedef unsigned int EventToken;

/**
 * Allocate and initialize an EventCount.
 *
 * @param ecPtr  a pointer to hold the new EventCount
 **/
__attribute__((warn_unused_result))
int makeEventCount(EventCount **ecPtr);

/**
 * Free an EventCount. It must no longer be in use.
 *
 * @param ec  the EventCount to free
 **/
void freeEventCount(EventCount *ec);

/**
 * Wake all threads that are waiting for the next event.
 *
 * @param ec  the EventCount to signal
 **/
void eventCountBroadcast(EventCount *ec);

/**
 * Prepare to wait for the EventCount to change by capturing a token of its
 * current state. The caller MUST eventually either call eventCountWait() or
 * eventCountCancel() exactly once for each token obtained.
 *
 * @param ec  the EventCount on which to prepare to wait
 *
 * @return an EventToken to be passed to the next eventCountWait() call
 **/
EventToken eventCountPrepare(EventCount *ec)
  __attribute__((warn_unused_result));

/**
 * Cancel a wait token that has been prepared but not waited upon. This must
 * be called after eventCountPrepare() when eventCountWait() is not going to
 * be invoked on the token.
 *
 * @param ec     the EventCount from which a wait token was obtained
 * @param token  the wait token that will never be passed to eventCountWait()
 **/
void eventCountCancel(EventCount *ec, EventToken token);

/**
 * Check if the current event count state corresponds to the provided token,
 * and if it is, wait for a signal that the state has changed. If an optional
 * timeout is provided, the wait will terminate after the timeout has elapsed.
 * Timing out automatically cancels the wait token, so callers must not
 * attempt to cancel the token on timeout.
 *
 * @param ec       the EventCount on which to wait
 * @param token    the EventToken returned by eventCountPrepare()
 * @param timeout  either NULL or a relative timeout for the wait operation
 *
 * @return true if the state has already changed or if signalled, otherwise
 *         false if a timeout was provided and the wait timed out
 **/
bool eventCountWait(EventCount *ec, EventToken token, const RelTime *timeout);

#endif /* EVENT_COUNT_H */
