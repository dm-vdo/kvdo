/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/util/eventCount.c#2 $
 */

/**
 * This EventCount implementation uses a posix semaphore for portability,
 * although a futex would be slightly superior to use and easy to substitute.
 * It is designed to make signalling as cheap as possible, since that is the
 * code path likely triggered on most updates to a lock-free data structure.
 * Waiters are likely going to sleep, so optimizing for that case isn't
 * necessary.
 *
 * The critical field is the state, which is really two fields that can be
 * atomically updated in unison: an event counter and a waiter count. Every
 * call to eventCountPrepare() issues a wait token by atomically incrementing
 * the waiter count. The key invariant is a strict accounting of the number of
 * tokens issued. Every token returned by eventCountPrepare() is a contract
 * that the caller will call acquireSemaphore() and a signaller will call
 * releaseSemapore(), each exactly once. Atomic updates to the state field
 * ensure that each token is counted once and that tokens are not lost.
 * Cancelling a token attempts to take a fast-path by simply decrementing the
 * waiters field, but if the token has already been claimed by a signaller,
 * the canceller must still wait on the semaphore to consume the transferred
 * token.
 *
 * The state field is 64 bits, partitioned into a 16-bit waiter field and a
 * 48-bit counter. We are unlikely to have 2^16 threads, much less 2^16
 * threads waiting on any single event transition. 2^48 microseconds is
 * several years, so a token holder would have to wait that long for the
 * counter to wrap around, and then call eventCountWait() at the exact right
 * time to see the re-used counter, in order to lose a wakeup due to counter
 * wrap-around. Using a 32-bit state field would greatly increase that chance,
 * but if forced to do so, the implementation could likely tolerate it since
 * callers are supposed to hold tokens for miniscule periods of time.
 * Fortunately, x64 has 64-bit compare-and-swap, and the performance of
 * interlocked 64-bit operations appears to be about the same as for 32-bit
 * ones, so being paranoid and using 64 bits costs us nothing.
 *
 * Here are some sequences of calls and state transitions:
 *
 *    action                    postcondition
 *                        counter   waiters   semaphore
 *    initialized           0          0          0
 *    prepare               0          1          0
 *    wait (blocks)         0          1          0
 *    signal                1          0          1
 *    wait (unblocks)       1          0          0
 *
 *    signal (fast-path)    1          0          0
 *    signal (fast-path)    1          0          0
 *
 *    prepare A             1          1          0
 *    prepare B             1          2          0
 *    signal                2          0          2
 *    wait B (fast-path)    2          0          1
 *    wait A (fast-path)    2          0          0
 *
 *    prepare               2          1          0
 *    cancel (fast-path)    2          0          0
 *
 *    prepare               2          1          0
 *    signal                3          0          1
 *    cancel (must wait)    3          0          0
 *
 * The EventCount structure is aligned, sized, and allocated to cache line
 * boundaries to avoid any false sharing between the EventCount and other
 * shared state. The state field and semaphore should fit on a single cache
 * line. The instrumentation counters increase the size of the structure so it
 * rounds up to use two (64-byte x86) cache lines.
 *
 * XXX Should instrumentation fields be #ifdef'ed out if not enabled?
 * XXX Need interface to access or display instrumentation counters.
 **/

#include "eventCount.h"

#include "atomic.h"
#include "common.h"
#include "compiler.h"
#include "cpu.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "threads.h"

enum {
  ONE_WAITER   = 1,               // value used to increment the waiters field
  ONE_EVENT    = (1 << 16),       // value used to increment the event counter
  WAITERS_MASK = (ONE_EVENT - 1), // bit mask to access the waiters field
  EVENTS_MASK  = ~WAITERS_MASK,   // bit mask to access the event counter

  // Change to "true" to enable instrumentation counters.
  INSTRUMENTED = false
};

struct eventCount {
  // Atomically mutable state:
  // low  16 bits: the number of wait tokens not posted to the semaphore
  // high 48 bits: current event counter
  Atomic64 state;

  // Semaphore used to block threads when waiting is required.
  Semaphore semaphore;

  // Instrumentation counters.
  Atomic64 signals;   // # of signal() calls
  Atomic64 wakeups;   // # of releaseSemapore() calls
  Atomic64 waits;     // # of eventCountWait() calls
  Atomic64 sleeps;    // # of acquireSemaphore() calls
  Atomic64 timeouts;  // # of sem_timedwait() timeouts

  // Declare alignment so we don't share a cache line.
}  __attribute__((aligned(CACHE_LINE_BYTES)));

/**
 * Atomically add a value to an instrumentation counter. This will be a no-op
 * if instrumentation is not enabled at compile time.
 *
 * @param counter  pointer to the counter
 * @param delta    the value to add
 **/
static INLINE void instrumentedAdd(Atomic64 *counter, int64_t delta)
{
  if (INSTRUMENTED) {
    atomicAdd64(counter, delta);
  }
}

/**
 * Test the event field in two tokens for equality.
 *
 * @return  true iff the tokens contain the same event field value
 **/
static INLINE bool sameEvent(EventToken token1, EventToken token2)
{
  return ((token1 & EVENTS_MASK) == (token2 & EVENTS_MASK));
}

/**********************************************************************/
void eventCountBroadcast(EventCount *ec)
{
  instrumentedAdd(&ec->signals, 1);

  memoryFence();

  unsigned int waiters;
  for (;;) {
    uint64_t state = relaxedLoad64(&ec->state);
    // Check if there are any tokens that have not yet been been transferred
    // to the semaphore. This is the fast no-waiters path.
    waiters = (state & WAITERS_MASK);
    if (waiters == 0) {
      // Fast path first time through--no need to signal or post if there are
      // no observers.
      return;
    }

    /*
     * Attempt to atomically claim all the wait tokens and bump the event
     * count using an atomic compare-and-swap.
     */
    EventToken newState = ((state & ~WAITERS_MASK) + ONE_EVENT);
    if (likely(compareAndSwap64(&ec->state, state, newState))) {
      break;
    }
    // The CAS failed because we lost a race with a new waiter or another
    // signaller, so try again.
  }

  instrumentedAdd(&ec->wakeups, waiters);

  /*
   * Wake the waiters by posting to the semaphore. This effectively transfers
   * the wait tokens to the semaphore. There's sadly no bulk post for posix
   * semaphores, so we've got to loop to do them all.
   */
  while (waiters-- > 0) {
    releaseSemaphore(&ec->semaphore, __func__);
  }
}

/**
 * Attempt to cancel a prepared wait token by decrementing the
 * number of waiters in the current state. This can only be done
 * safely if the event count hasn't been bumped.
 *
 * @param ec     the event count on which the wait token was issued
 * @param token  the wait to cancel
 *
 * @return true if the wait was cancelled, false if the caller must
 *         still wait on the semaphore
 **/
static INLINE bool fastCancel(EventCount *ec, EventToken token)
{
  EventToken currentToken = relaxedLoad64(&ec->state);
  while (sameEvent(currentToken, token)) {
    // Try to decrement the waiter count via compare-and-swap as if we had
    // never prepared to wait.
    if (compareAndSwap64(&ec->state, currentToken, currentToken - 1)) {
      return true;
    }
    currentToken = relaxedLoad64(&ec->state);
  }
  return false;
}

/**
 * Consume a token from the semaphore, waiting (with an optional timeout) if
 * one is not currently available. Also attempts to count the number of times
 * we'll actually have to wait because there are no tokens (permits) available
 * in the semaphore, and the number of times the wait times out.
 *
 * @param ec       the event count instance
 * @param timeout  an optional timeout value to pass to attemptSemaphore()
 *
 * @return true if a token was consumed, otherwise false only if a timeout
 *         was specified and we timed out
 **/
static bool consumeWaitToken(EventCount *ec, const RelTime *timeout)
{
  // Try to grab a token without waiting.
  if (attemptSemaphore(&ec->semaphore, 0, __func__)) {
    return true;
  }

  instrumentedAdd(&ec->sleeps, 1);

  if (timeout == NULL) {
    acquireSemaphore(&ec->semaphore, __func__);
  } else if (!attemptSemaphore(&ec->semaphore, *timeout, __func__)) {
    instrumentedAdd(&ec->timeouts, 1);
    return false;
  }
  return true;
}

/**********************************************************************/
int makeEventCount(EventCount **ecPtr)
{
  // Allocate the event count on a cache line boundary so there will not
  // be false sharing of the line with any other data structure.
  EventCount *ec = NULL;
  int result = allocateCacheAligned(sizeof(EventCount), "event count", &ec);
  if (result != UDS_SUCCESS) {
    return result;
  }

  relaxedStore64(&ec->state, 0);
  result = initializeSemaphore(&ec->semaphore, 0, __func__);
  if (result != UDS_SUCCESS) {
    FREE(ec);
    return result;
  }

  *ecPtr = ec;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeEventCount(EventCount *ec)
{
  if (ec == NULL) {
    return;
  }
  destroySemaphore(&ec->semaphore, __func__);
  FREE(ec);
}

/**********************************************************************/
EventToken eventCountPrepare(EventCount *ec)
{
  return atomicAdd64(&ec->state, ONE_WAITER);
}

/**********************************************************************/
int eventCountCancel(EventCount *ec, EventToken token)
{
  // Decrement the waiter count if the event hasn't been signalled.
  if (fastCancel(ec, token)) {
    return UDS_SUCCESS;
  }
  // A signaller has already transferred (or promised to transfer) our token
  // to the semaphore, so we must consume it from the semaphore by waiting.
  return eventCountWait(ec, token, NULL);
}

/**********************************************************************/
bool eventCountWait(EventCount *ec, EventToken token, const RelTime *timeout)
{
  instrumentedAdd(&ec->waits, 1);

  for (;;) {
    // Wait for a signaller to transfer our wait token to the semaphore.
    if (!consumeWaitToken(ec, timeout)) {
      // The wait timed out, so we must cancel the token instead. Try to
      // decrement the waiter count if the event hasn't been signalled.
      if (fastCancel(ec, token)) {
        return false;
      }
      /*
       * We timed out, but a signaller came in before we could cancel the
       * wait. We have no choice but to wait for the semaphore to be posted.
       * Since signaller has promised to do it, the wait will be short. The
       * timeout and the signal happened at about the same time, so either
       * outcome could be returned. It's simpler to ignore the timeout.
       */
      timeout = NULL;
      continue;
    }

    // A wait token has now been consumed from the semaphore.

    // Stop waiting if the count has changed since the token was acquired.
    if (!sameEvent(token, relaxedLoad64(&ec->state))) {
      return true;
    }

    // We consumed someone else's wait token. Put it back in the semaphore,
    // which will wake another waiter, hopefully one who can stop waiting.
    releaseSemaphore(&ec->semaphore, __func__);

    // Attempt to give an earlier waiter a shot at the semaphore.
    yieldScheduler();
  }
}
