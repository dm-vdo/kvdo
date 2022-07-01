// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * An event count is a lock-free equivalent of a condition variable.
 *
 * Using an event count, a lock-free producer/consumer can wait for a state
 * change (adding an item to an empty queue, for example) without spinning or
 * falling back on the use of mutex-based locks. Signalling is cheap when
 * there are no waiters (a memory fence), and preparing to wait is also
 * inexpensive (an atomic add instruction).
 *
 * A lock-free producer should call event_count_broadcast() after any mutation
 * to the lock-free data structure that a consumer might be waiting on. The
 * consumers should poll for work like this:
 *
 *   for (;;) {
 *     // Fast path--no additional cost to consumer.
 *     if (lockfree_dequeue(&item)) {
 *       return item;
 *     }
 *     // Two-step wait: get current token and poll state, either cancelling
 *     // the wait or waiting for the token to be signalled.
 *     event_token_t token = event_count_prepare(event_count);
 *     if (lockfree_dequeue(&item)) {
 *       event_count_cancel(event_count, token);
 *       return item;
 *     }
 *     event_count_wait(event_count, token, NULL);
 *     // State has changed, but must check condition again, so loop.
 *   }
 *
 * Once event_count_prepare() is called, the caller should neither sleep nor
 * perform long-running or blocking actions before passing the token to
 * event_count_cancel() or event_count_wait(). The implementation is optimized
 * for a short polling window, and will not perform well if there are
 * outstanding tokens that have been signalled but not waited upon.
 *
 * This event count implementation uses a posix semaphore for portability,
 * although a futex would be slightly superior to use and easy to substitute.
 * It is designed to make signalling as cheap as possible, since that is the
 * code path likely triggered on most updates to a lock-free data structure.
 * Waiters are likely going to sleep, so optimizing for that case isn't
 * necessary.
 *
 * The critical field is the state, which is really two fields that can be
 * atomically updated in unison: an event counter and a waiter count. Every
 * call to event_count_prepare() issues a wait token by atomically incrementing
 * the waiter count. The key invariant is a strict accounting of the number of
 * tokens issued. Every token returned by event_count_prepare() is a contract
 * that the caller will call uds_acquire_semaphore() and a signaller will call
 * uds_release_semaphore(), each exactly once. Atomic updates to the state
 * field ensure that each token is counted once and that tokens are not lost.
 * Cancelling a token attempts to take a fast-path by simply decrementing the
 * waiters field, but if the token has already been claimed by a signaller, the
 * canceller must still wait on the semaphore to consume the transferred token.
 *
 * The state field is 64 bits, partitioned into a 16-bit waiter field and a
 * 48-bit counter. We are unlikely to have 2^16 threads, much less 2^16 threads
 * waiting on any single event transition. 2^48 microseconds is several years,
 * so a token holder would have to wait that long for the counter to wrap
 * around, and then call event_count_wait() at the exact right time to see the
 * re-used counter, in order to lose a wakeup due to counter wrap-around. Using
 * a 32-bit state field would greatly increase that chance, but if forced to do
 * so, the implementation could likely tolerate it since callers are supposed
 * to hold tokens for miniscule periods of time.  Fortunately, x64 has 64-bit
 * compare-and-swap, and the performance of interlocked 64-bit operations
 * appears to be about the same as for 32-bit ones, so being paranoid and using
 * 64 bits costs us nothing.
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
 * The event count structure is aligned, sized, and allocated to cache line
 * boundaries to avoid any false sharing between the event count and other
 * shared state. The state field and semaphore should fit on a single cache
 * line. The instrumentation counters increase the size of the structure so it
 * rounds up to use two (64-byte x86) cache lines.
 **/

#include "event-count.h"

#include <linux/atomic.h>

#include "common.h"
#include "compiler.h"
#include "cpu.h"
#include "logger.h"
#include "memory-alloc.h"
#include "uds-threads.h"

enum {
	/* value used to increment the waiters field */
	ONE_WAITER = 1,
	/* value used to increment the event counter */
	ONE_EVENT = (1 << 16),
	/* bit mask to access the waiters field */
	WAITERS_MASK = (ONE_EVENT - 1),
	/* bit mask to access the event counter */
	EVENTS_MASK = ~WAITERS_MASK,
};

struct event_count {
	/*
	 * Atomically mutable state:
	 * low  16 bits: the number of wait tokens not posted to the semaphore
	 * high 48 bits: current event counter
	 */
	atomic64_t state;

	/* Semaphore used to block threads when waiting is required. */
	struct semaphore semaphore;

	/* Declare alignment so we don't share a cache line. */
} __attribute__((aligned(CACHE_LINE_BYTES)));

static INLINE bool same_event(event_token_t token1, event_token_t token2)
{
	return (token1 & EVENTS_MASK) == (token2 & EVENTS_MASK);
}

/* Wake all threads that are waiting for the next event. */
void event_count_broadcast(struct event_count *count)
{
	uint64_t waiters;
	uint64_t state;
	uint64_t old_state;

	/* Even if there are no waiters (yet), we will need a memory barrier. */
	smp_mb();

	state = old_state = atomic64_read(&count->state);
	do {
		event_token_t new_state;

		/*
		 * Check if there are any tokens that have not yet been been
		 * transferred to the semaphore. This is the fast no-waiters
		 * path.
		 */
		waiters = (state & WAITERS_MASK);
		if (waiters == 0) {
			/*
			 * Fast path first time through -- no need to signal or
			 * post if there are no observers.
			 */
			return;
		}

		/*
		 * Attempt to atomically claim all the wait tokens and bump the
		 * event count using an atomic compare-and-swap. This operation
		 * contains a memory barrier.
		 */
		new_state = ((state & ~WAITERS_MASK) + ONE_EVENT);
		old_state = state;
		state = atomic64_cmpxchg(&count->state, old_state, new_state);
		/*
		 * The cmpxchg fails when we lose a race with a new waiter or
		 * another signaller, so try again.
		 */
	} while (unlikely(state != old_state));

	/*
	 * Wake the waiters by posting to the semaphore. This effectively
	 * transfers the wait tokens to the semaphore. There's sadly no bulk
	 * post for posix semaphores, so we've got to loop to do them all.
	 */
	while (waiters-- > 0) {
		uds_release_semaphore(&count->semaphore);
	}
}

/*
 * Attempt to cancel a prepared wait token by decrementing the number of
 * waiters in the current state. This can only be done safely if the event
 * count hasn't been incremented. Returns true if the wait was successfully
 * cancelled.
 */
static INLINE bool fast_cancel(struct event_count *count, event_token_t token)
{
	event_token_t current_token = atomic64_read(&count->state);
	event_token_t new_token;

	while (same_event(current_token, token)) {
		/*
		 * Try to decrement the waiter count via compare-and-swap as if
		 * we had never prepared to wait.
		 */
		new_token = atomic64_cmpxchg(&count->state,
					     current_token,
					     current_token - 1);
		if (new_token == current_token) {
			return true;
		}

		current_token = new_token;
	}

	return false;
}

/*
 * Consume a token from the semaphore, waiting (with an optional timeout) if
 * one is not currently available. Returns true if a token was consumed.
 */
static bool consume_wait_token(struct event_count *count,
			       const ktime_t *timeout)
{
	/* Try to grab a token without waiting. */
	if (uds_attempt_semaphore(&count->semaphore, 0)) {
		return true;
	}

	if (timeout == NULL) {
		uds_acquire_semaphore(&count->semaphore);
	} else if (!uds_attempt_semaphore(&count->semaphore, *timeout)) {
		return false;
	}

	return true;
}

int make_event_count(struct event_count **count_ptr)
{
	/*
	 * The event count will be allocated on a cache line boundary so there
	 * will not be false sharing of the line with any other data structure.
	 */
	int result;
	struct event_count *count = NULL;

	result = UDS_ALLOCATE(1, struct event_count, "event count", &count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	atomic64_set(&count->state, 0);
	result = uds_initialize_semaphore(&count->semaphore, 0);
	if (result != UDS_SUCCESS) {
		UDS_FREE(count);
		return result;
	}

	*count_ptr = count;
	return UDS_SUCCESS;
}

/* Free a struct event_count. It must no longer be in use. */
void free_event_count(struct event_count *count)
{
	if (count == NULL) {
		return;
	}

	uds_destroy_semaphore(&count->semaphore);
	UDS_FREE(count);
}

/*
 * Prepare to wait for the event count to change by capturing a token of its
 * current state. The caller MUST eventually either call event_count_wait() or
 * event_count_cancel() exactly once for each token obtained.
 */
event_token_t event_count_prepare(struct event_count *count)
{
	return atomic64_add_return(ONE_WAITER, &count->state);
}

/*
 * Cancel a wait token that has been prepared but not waited upon. This must
 * be called after event_count_prepare() when event_count_wait() is not going to
 * be invoked on the token.
 */
void event_count_cancel(struct event_count *count, event_token_t token)
{
	/* Decrement the waiter count if the event hasn't been signalled. */
	if (fast_cancel(count, token)) {
		return;
	}

	/*
	 * A signaller has already transferred (or promised to transfer) our
	 * token to the semaphore, so we must consume it from the semaphore by
	 * waiting.
	 */
	event_count_wait(count, token, NULL);
}

/*
 * Check if the current event count state corresponds to the provided token,
 * and if it is, wait for a signal that the state has changed. If a timeout is
 * provided, the wait will terminate after the timeout has elapsed. Timing out
 * automatically cancels the wait token, so callers must not attempt to cancel
 * the token in this case. The timeout is measured in nanoseconds. This
 * function returns true if the state changed, or false if it timed out.
 */
bool event_count_wait(struct event_count *count,
		      event_token_t token,
		      const ktime_t *timeout)
{
	for (;;) {
		/*
		 * Wait for a signaller to transfer our wait token to the
		 * semaphore.
		 */
		if (!consume_wait_token(count, timeout)) {
			/*
			 * The wait timed out, so we must cancel the token
			 * instead. Try to decrement the waiter count if the
			 * event hasn't been signalled.
			 */
			if (fast_cancel(count, token)) {
				return false;
			}
			/*
			 * We timed out, but a signaller came in before we
			 * could cancel the wait. We have no choice but to wait
			 * for the semaphore to be posted. Since the signaller
			 * has promised to do it, the wait should be short. The
			 * timeout and the signal happened at about the same
			 * time, so either outcome could be returned. It's
			 * simpler to ignore the timeout.
			 */
			timeout = NULL;
			continue;
		}

		/* A wait token has now been consumed from the semaphore. */

		/*
		 * Stop waiting if the count has changed since the token was
		 * acquired.
		 */
		if (!same_event(token, atomic64_read(&count->state))) {
			return true;
		}

		/*
		 * We consumed someone else's wait token. Put it back in the
		 * semaphore, which will wake another waiter, hopefully one who
		 * can stop waiting.
		 */
		uds_release_semaphore(&count->semaphore);

		/* Attempt to give an earlier waiter a shot at the semaphore. */
		uds_yield_scheduler();
	}
}
