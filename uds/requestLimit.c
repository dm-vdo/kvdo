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
 * $Id: //eng/uds-releases/flanders/src/uds/requestLimit.c#2 $
 */

#include "requestLimit.h"

#include "compiler.h"
#include "cpu.h"
#include "memoryAlloc.h"
#include "threads.h"
#include "uds.h"
#include "util/atomic.h"

/**
 * This implementation leverages the asymmetry of the pipeline by using two
 * separate counters for request permits, called piles. Threads that are
 * borrowing permits all decrement from one pile, called the readyPile, and
 * threads that are returning permits all increment a separate pile, called
 * the returnedPile. The two piles are kept on separate cache lines to reduce
 * cache contention between client threads and pipeline threads. In the common
 * case of a single client thread and single callback thread for a given
 * request context and request limit, this eliminates contention completely in
 * the fast paths, which is a big advantage over the simple POSIX semaphore
 * that was previously used to implement the request limit.
 *
 * To further reduce contention, only atomic operations are used on a fast
 * path instead of going through a mutex, allowing maximum concurrency. The
 * borrowing operations use compare-and-swap, so it can fail and loop if there
 * is contention, but that should be equivalent in cost to acquiring a
 * semaphore. Returning permits uses atomic increment in the common fast-path.
 *
 * When no permits are in the readyPile, it is likely necessary to wait. This
 * slow-path uses a mutex and a condition variable to serialize a "sweep"
 * operation that transfers permits from the returnedPile to the readyPile,
 * waiting only when both piles are empty. If the returnedPile was not empty,
 * the sweeping thread claims a permit for itself and transfers the rest to
 * the readyPile for other borrowers to claim. Any blocked concurrent
 * borrowers will have to wake and re-acquire the mutex first, while newly
 * arriving borrowers will be free to claim permits using CAS. This could lead
 * to a loss of fairness if there are a great many clients contending for
 * permits, which could be solved in the future (if needed) by sweeping
 * permits to a pile for lock holders to consume exclusively.
 *
 * There is a slow-path when returning permits created by the need to wake any
 * threads waiting for there to be permits to borrow. The cost of this is
 * reduced by only waking threads when the size of the returnedPile crosses a
 * specific threshold. That threshold not the empty-to-non-empty transition,
 * but is selected to be an arbitrary fraction of the total number of requests
 * for two reasons: it reduces the number of times a returning thread is
 * likely to use the slow path to wake a sweeper, and it also "batches" the
 * permits available to fast clients. If we were to wake the client thread the
 * moment a single permit is available to borrow, it would very likely be able
 * to create a request, enqueue it, and attempt to borrow a second permit
 * before a second permit could be returned, causing it to wait again after
 * doing very little work. Batching returned permits therefore reduces the
 * number of context switches for fast client threads. This replaces the
 * 250-microsecond sleep heuristic that was used in the previous
 * semaphore-based implementation when no permits were available.
 **/
struct requestLimit {
  // The following field is frequently used by threads borrowing permits.

  /** the number of permits currently ready to be borrowed */
  Atomic32 readyPile;

  // The following fields are frequently used by threads returning permits.

  /** the number of permits that have been returned, but not swept */
  Atomic32 returnedPile __attribute__((aligned(CACHE_LINE_BYTES)));

  /** the size of the returned pile that triggers a sweep */
  Atomic32 sweepThreshold;

  // The following fields are used less often, and mostly by borrowers.

  /** acquired by borrowers when no permits are ready */
  Mutex    sweepLock __attribute__((aligned(CACHE_LINE_BYTES)));

  /** signalled when there are enough returned requests to sweep */
  CondVar  sweepCondition;

  // The following fields are rarely accessed and rarely change.

  /** the total number of permits that have been assigned to this limit */
  uint32_t totalPermits __attribute__((aligned(CACHE_LINE_BYTES)));

  /** a lock used to serialize any changes to totalPermits */
  Mutex    totalPermitsLock;
};

/**
 * These constants are used to configure the sweepThreshold batch size.
 **/
enum {
  /** the number of batches into which the divide the total permits */
  SWEEP_THRESHOLD_DIVISOR = 4,
  /** the maximum value to use as the sweepThreshold batch size */
  MAXIMUM_SWEEP_THRESHOLD = 32
};

/**
 * Select and assign the sweepCondition broadcast threshold.
 *
 * @param limit    the request limit
 * @param permits  the total number of permits on which to base the
 *                 choice of a threshold
 **/
static void setSweepThreshold(RequestLimit *limit, uint32_t permits)
{
  uint32_t threshold = (permits / SWEEP_THRESHOLD_DIVISOR);
  if (threshold < 1) {
    threshold = 1;
  } else if (threshold > MAXIMUM_SWEEP_THRESHOLD) {
    threshold = MAXIMUM_SWEEP_THRESHOLD;
  }

  // Store atomically to ensure visibility to threads returning permits while
  // the total number of permits is being reduced.
  atomicStore32(&limit->sweepThreshold, threshold);

  // Changing the threshold makes it possible for no return thread to uniquely
  // cross it, losing the broadcast, so wake any waiters just in case.
  broadcastCond(&limit->sweepCondition);
}

/**********************************************************************/
int makeRequestLimit(uint32_t permits, RequestLimit **limitPtr)
{
  // Allocate the limit on a cache line boundary so the cache line aligment of
  // the fields will be correct and the limit won't share cache lines.
  RequestLimit *limit;
  int result = ALLOCATE(1, RequestLimit, "requestLimit", &limit);
  if (result != UDS_SUCCESS) {
    return result;
  }

  limit->totalPermits = permits;

  // All permits are initially ready to be borrowed.
  relaxedStore32(&limit->readyPile, permits);
  relaxedStore32(&limit->returnedPile, 0);

  initMutex(&limit->totalPermitsLock);
  initMutex(&limit->sweepLock);
  initCond(&limit->sweepCondition);

  // Select and assign the initial sweepCondition broadcast threshold.
  setSweepThreshold(limit, permits);

  *limitPtr = limit;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeRequestLimit(RequestLimit *limit)
{
  if (limit == NULL) {
    return;
  }

  // Wait for all permits to be returned before freeing.
  setRequestPermitLimit(limit, 0);

  destroyMutex(&limit->totalPermitsLock);
  destroyMutex(&limit->sweepLock);
  destroyCond(&limit->sweepCondition);
  FREE(limit);
}

/**********************************************************************/
uint32_t getRequestPermitLimit(RequestLimit *limit)
{
  // Technically protected by the lock, but since it could change moments
  // after we return and there is no exterior locking, it matters not.
  return limit->totalPermits;
}

/**********************************************************************/
void setRequestPermitLimit(RequestLimit *limit, uint32_t permits)
{
  /*
   * We can't change the size atomically because of existing outstanding
   * requests and concurrent activity; we can only ratchet it up or down one
   * at a time. Lock other threads out so we can't have two fighting to move
   * the number in different directions.
   */
  lockMutex(&limit->totalPermitsLock);

  // Change the sweep threshold first to ensure sweepCondition broadcasts when
  // reducing the number of permits below the current sweep threshold.
  setSweepThreshold(limit, permits);

  // If the new limit is larger, just add the additional permits by pretending
  // to return them.
  if (limit->totalPermits < permits) {
    returnRequestPermits(limit, permits - limit->totalPermits);
    limit->totalPermits = permits;
  }

  // If the new limit is smaller, repeatedly borrow permits and throw them
  // away until we reach the new limit.
  while (limit->totalPermits > permits) {
    borrowRequestPermit(limit);
    limit->totalPermits -= 1;
  }

  unlockMutex(&limit->totalPermitsLock);
}

/**
 * Attempt to borrow a permit from the ready pile without blocking or waiting.
 * This is the fast path for borrowing a permit.
 *
 * @param limit  the request limit
 *
 * @return <code>true</code> if a permit was borrowed, <code>false</code>
 *         otherwise, which indicates no permits are in the ready pile
 **/
static bool borrowFromReadyPile(RequestLimit *limit)
{
  // Try to use CAS to atomically decrement a non-empty ready pile, looping
  // only if we lose races with another borrower or a sweeper.
  for (;;) {
    uint32_t ready = relaxedLoad32(&limit->readyPile);
    if (ready == 0) {
      // No permits appear to be available, so fall back to the slow path.
      return false;
    }
    if (compareAndSwap32(&limit->readyPile, ready, ready - 1)) {
      // A successful decrement claimed a permit from the ready bin.
      return true;
    }
    // The CAS failed due to concurrent access to the pile, so try again.
  }
}

/**
 * Attempt to borrow a permit from the returned pile and sweep the remaining
 * permits in it to the ready pile without blocking or waiting. The caller
 * must already hold the sweep lock when calling this function.
 *
 * @param limit  the request limit
 *
 * @return <code>true</code> if a permit was borrowed, <code>false</code>
 *         otherwise, which indicates no permits are in the returned pile
 **/
static bool sweepPermits(RequestLimit *limit)
{
  int32_t returned = relaxedLoad32(&limit->returnedPile);
  if (returned == 0) {
    // There are no permits available to borrow or sweep.
    return false;
  }

  /*
   * There are returned permits available, so claim one and sweep the rest.
   * Holding the sweep lock ensures only one thread tries to do this at a
   * time, creating an atomic "subtract from returned and add to ready"
   * operation even though the mutex doesn't directly protect the piles. Only
   * a sweeper holding the sweep lock ever decrements returnedPile, making it
   * safe to just (atomically) subtract the permits we saw there when we
   * entered this function.
   */
  atomicAdd32(&limit->returnedPile, -returned);
  if (returned > 1) {
    // Add those permits to the ready pile except one we borrowed for ourself.
    atomicAdd32(&limit->readyPile, returned - 1);
  }
  return true;
}

/**********************************************************************/
void borrowRequestPermit(RequestLimit *limit)
{
  // First try the fast path, borrowing a permit from the ready bin.
  if (borrowFromReadyPile(limit)) {
    return;
  }

  // The slow path requires a mutex so we can wait when no permits are
  // available.
  lockMutex(&limit->sweepLock);

  // Loop until we have borrowed a permit from the ready bin or swept one from
  // the returned pile, waiting whenever both of those operations fail.
  while (!borrowFromReadyPile(limit) && !sweepPermits(limit)) {
    waitCond(&limit->sweepCondition, &limit->sweepLock);
  }
  unlockMutex(&limit->sweepLock);
}

/**********************************************************************/
void returnRequestPermits(RequestLimit *limit, uint32_t permits)
{
  // Atomically add the permits to the returned pile, which cannot fail and
  // cannot lose the permits, and capture the new size of the pile.
  uint32_t newPile = atomicAdd32(&limit->returnedPile, permits);

  // If the permits we returned caused the pile to cross the sweep size
  // threshold, wake any waiting sweepers.
  uint32_t threshold = relaxedLoad32(&limit->sweepThreshold);
  if (unlikely(newPile >= threshold) && ((newPile - permits) < threshold)) {
    lockMutex(&limit->sweepLock);
    /*
     * We MUST broadcast while holding the mutex since we're signalling a
     * change in a variable (returnedPile) that isn't protected by the mutex.
     * If we didn't, both the broadcast and the value change could be missed
     * by a thread about to wait, leading to a hang.
     */
    broadcastCond(&limit->sweepCondition);
    unlockMutex(&limit->sweepLock);
  }
}
