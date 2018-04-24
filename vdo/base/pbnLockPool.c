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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/pbnLockPool.c#2 $
 */

#include "pbnLockPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "ringNode.h"
#include "pbnLock.h"

/**
 * Unused (idle) PBN locks are kept in a ring. Just like in a malloc
 * implementation, the lock structure is unused memory, so we can save a bit
 * of space (and not pollute the lock structure proper) by using a union to
 * overlay the lock structure with the free list.
 **/
typedef union idlePBNLock {
  /** Only used while locks are in the pool */
  RingNode node;
  /** Only used while locks are not in the pool */
  PBNLock  lock;
} IdlePBNLock;

/**
 * The lock pool is little more than the memory allocated for the locks.
 **/
struct pbnLockPool {
  /** The number of locks allocated for the pool */
  size_t      capacity;
  /** The number of locks currently borrowed from the pool */
  size_t      borrowed;
  /** A ring containing all idle PBN lock instances */
  RingNode    idleRing;
  /** The memory for all the locks allocated by this pool */
  IdlePBNLock locks[];
};

/**********************************************************************/
int makePBNLockPool(size_t capacity, PBNLockPool **poolPtr)
{
  PBNLockPool *pool;
  int result = ALLOCATE_EXTENDED(PBNLockPool, capacity, IdlePBNLock, __func__,
                                 &pool);
  if (result != VDO_SUCCESS) {
    return result;
  }

  pool->capacity = capacity;
  pool->borrowed = capacity;
  initializeRing(&pool->idleRing);

  for (size_t i = 0; i < capacity; i++) {
    PBNLock *lock = &pool->locks[i].lock;
    returnPBNLockToPool(pool, &lock);
  }

  *poolPtr = pool;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freePBNLockPool(PBNLockPool **poolPtr)
{
  if (*poolPtr == NULL) {
    return;
  }

  PBNLockPool *pool = *poolPtr;
  ASSERT_LOG_ONLY(pool->borrowed == 0,
                  "All PBN locks must be returned to the pool before it is"
                  " freed, but %zu locks are still on loan",
                  pool->borrowed);
  FREE(pool);
  *poolPtr = NULL;
}

/**********************************************************************/
int borrowPBNLockFromPool(PBNLockPool  *pool,
                          PBNLockType   type,
                          PBNLock     **lockPtr)
{
  if (pool->borrowed >= pool->capacity) {
    return logErrorWithStringError(VDO_LOCK_ERROR,
                                   "no free PBN locks left to borrow");
  }
  pool->borrowed += 1;

  RingNode *idleNode = popRingNode(&pool->idleRing);
  // The lock was zeroed when it was placed in the pool, but the overlapping
  // ring pointers are non-zero after a pop.
  memset(idleNode, 0, sizeof(*idleNode));

  STATIC_ASSERT(offsetof(IdlePBNLock, node) == offsetof(IdlePBNLock, lock));
  PBNLock *lock = (PBNLock *) idleNode;
  initializePBNLock(lock, type);

  *lockPtr = lock;
  return VDO_SUCCESS;
}

/**********************************************************************/
void returnPBNLockToPool(PBNLockPool *pool, PBNLock **lockPtr)
{
  // Take what should be the last lock reference from the caller
  PBNLock *lock = *lockPtr;
  *lockPtr = NULL;

  // A bit expensive, but will promptly catch some use-after-free errors.
  memset(lock, 0, sizeof(*lock));

  RingNode *idleNode = (RingNode *) lock;
  initializeRing(idleNode);
  pushRingNode(&pool->idleRing, idleNode);

  ASSERT_LOG_ONLY(pool->borrowed > 0, "shouldn't return more than borrowed");
  pool->borrowed -= 1;
}
