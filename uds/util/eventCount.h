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
 */

#ifndef EVENT_COUNT_H
#define EVENT_COUNT_H

#include "timeUtils.h"
#include "typeDefs.h"

/**
 * An event count is a lock-free equivalent of a condition variable.
 *
 * Using an event count, a lock-free producer/consumer can wait for a state
 * change (adding an item to an empty queue, for example) without spinning or
 * falling back on the use of mutex-based locks. Signalling is cheap when
 * there are no waiters (a memory fence), and preparing to wait is
 * also inexpensive (an atomic add instruction).
 *
 * A lock-free producer should call event_count_broadcast() after any mutation
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
 *     event_token_t token = event_count_prepare(ec);
 *     if (lockFreeDequeue(&item)) {
 *       event_count_cancel(ec, token);
 *       return item;
 *     }
 *     event_count_wait(ec, token, NULL);
 *     // State has changed, but must check condition again, so loop.
 *   }
 *
 * Once event_count_prepare() is called, the caller should neither dally,
 * sleep, nor perform long-running or blocking actions before passing the token
 * to event_count_cancel() or event_count_wait(). The implementation is
 * xoptimized for a short polling window, and will not perform well if there
 * are outstanding tokens that have been signalled but not waited upon.
 **/

struct event_count;

typedef unsigned int event_token_t;

/**
 * Allocate and initialize a struct event_count.
 *
 * @param ec_ptr  a pointer to hold the new struct event_count
 **/
int __must_check make_event_count(struct event_count **ec_ptr);

/**
 * Free a struct event_count. It must no longer be in use.
 *
 * @param ec  the struct event_count to free
 **/
void free_event_count(struct event_count *ec);

/**
 * Wake all threads that are waiting for the next event.
 *
 * @param ec  the struct event_count to signal
 **/
void event_count_broadcast(struct event_count *ec);

/**
 * Prepare to wait for the event count to change by capturing a token of its
 * current state. The caller MUST eventually either call event_count_wait() or
 * event_count_cancel() exactly once for each token obtained.
 *
 * @param ec  the struct event_count on which to prepare to wait
 *
 * @return an event_token_t to be passed to the next event_count_wait() call
 **/
event_token_t __must_check event_count_prepare(struct event_count *ec);

/**
 * Cancel a wait token that has been prepared but not waited upon. This must
 * be called after event_count_prepare() when event_count_wait() is not going to
 * be invoked on the token.
 *
 * @param ec     the struct event_count from which a wait token was obtained
 * @param token  the wait token that will never be passed to event_count_wait()
 **/
void event_count_cancel(struct event_count *ec, event_token_t token);

/**
 * Check if the current event count state corresponds to the provided token,
 * and if it is, wait for a signal that the state has changed. If an optional
 * timeout is provided, the wait will terminate after the timeout has elapsed.
 * Timing out automatically cancels the wait token, so callers must not
 * attempt to cancel the token on timeout.
 *
 * @param ec       the struct event_count on which to wait
 * @param token    the event_token_t returned by event_count_prepare()
 * @param timeout  either NULL or a nanosecond timeout for the wait operation
 *
 * @return true if the state has already changed or if signalled, otherwise
 *         false if a timeout was provided and the wait timed out
 **/
bool event_count_wait(struct event_count *ec,
		      event_token_t token,
		      const ktime_t *timeout);

#endif /* EVENT_COUNT_H */
