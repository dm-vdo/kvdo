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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/hashLockInternals.h#2 $
 */

#ifndef HASH_LOCK_INTERNALS_H
#define HASH_LOCK_INTERNALS_H

#include "completion.h"
#include "ringNode.h"
#include "types.h"
#include "uds.h"
#include "waitQueue.h"

typedef enum {
  /** State for locks that are not in use or are being initialized. */
  HASH_LOCK_INITIALIZING = 0,

  // This is the sequence of states typically used on the non-dedupe path.
  HASH_LOCK_QUERYING,
  HASH_LOCK_WRITING,
  HASH_LOCK_UPDATING,

  // The remaining states are typically used on the dedupe path in this order.
  HASH_LOCK_LOCKING,
  HASH_LOCK_VERIFYING,
  HASH_LOCK_DEDUPING,
  HASH_LOCK_UNLOCKING,

  // XXX This is a temporary state denoting a lock which is sending VIOs back
  // to the old dedupe and vioWrite pathways. It won't be in the final version
  // of VDOSTORY-190.
  HASH_LOCK_BYPASSING,

  /**
   * Terminal state for locks returning to the pool. Must be last both because
   * it's the final state, and also because it's used to count the states.
   **/
  HASH_LOCK_DESTROYING,
} HashLockState;

struct hashLock {
  /** When the lock is unused, this RingNode allows the lock to be pooled */
  RingNode       poolNode;

  /** The block hash covered by this lock */
  UdsChunkName   hash;

  /**
   * A ring containing the DataVIOs sharing this lock, all having the same
   * chunk name and data block contents, linked by their hashLockNode fields.
   **/
  RingNode       duplicateRing;

  /** The number of DataVIOs sharing this lock instance */
  VIOCount       referenceCount;

  /** The maximum value of referenceCount in the lifetime of this lock */
  VIOCount       maxReferences;

  /** The current state of this lock */
  HashLockState  state;

  /** True if the UDS index should be updated with new advice */
  bool           updateAdvice;

  /** True if the advice has been verified to be a true duplicate */
  bool           verified;

  /** True if the lock has already accounted for an initial verification */
  bool           verifyCounted;

  /** True if this lock is registered in the lock map (cleared on rollover) */
  bool           registered;

  /**
   * If verified is false, this is the location of a possible duplicate.
   * If verified is true, is is the verified location of a true duplicate.
   **/
  ZonedPBN       duplicate;

  /** The PBN lock on the block containing the duplicate data */
  PBNLock       *duplicateLock;

  /** The DataVIO designated to act on behalf of the lock */
  DataVIO       *agent;

  /**
   * Other DataVIOs with data identical to the agent who are currently waiting
   * for the agent to get the information they all need to deduplicate--either
   * against each other, or against an existing duplicate on disk.
   **/
  WaitQueue      waiters;
};

/**
 * Initialize a HashLock instance which has been newly allocated.
 *
 * @param lock  The lock to initialize
 **/
static inline void initializeHashLock(HashLock *lock)
{
  initializeRing(&lock->poolNode);
  initializeRing(&lock->duplicateRing);
  initializeWaitQueue(&lock->waiters);
}

/**
 * Get the string representation of a hash lock state.
 *
 * @param state  The hash lock state
 *
 * @return The short string representing the state
 **/
const char *getHashLockStateName(HashLockState state)
  __attribute__((warn_unused_result));

#endif // HASH_LOCK_INTERNALS_H
