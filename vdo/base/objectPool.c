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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/objectPool.c#4 $
 */

#include "objectPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "completion.h"
#include "types.h"
#include "waitQueue.h"

/**
 * An ObjectPool is a collection of preallocated objects.
 **/
struct objectPool {
  /** The number of objects managed by the pool */
  size_t         poolSize;
  /** The list of objects which are available */
  RingNode       available;
  /** The queue of requestors waiting for objects from the pool */
  WaitQueue      waiting;
  /** The number of objects currently in use */
  size_t         busyCount;
  /** The list of objects which are in use */
  RingNode       busy;
  /** The administrative state of the pool */
  AdminState     adminState;
  /** The number of requests when no object was available */
  uint64_t       outageCount;
  /** The number of requests while the pool was quiescent */
  uint64_t       suspendCount;
  /** The objects managed by the pool */
  void          *entryData;
};

/**********************************************************************/
int makeObjectPool(void *entryData, ObjectPool **poolPtr)
{
  ObjectPool *pool;
  int result = ALLOCATE(1, ObjectPool, __func__, &pool);
  if (result != VDO_SUCCESS) {
    return result;
  }

  initializeRing(&pool->available);
  initializeRing(&pool->busy);
  pool->entryData = entryData;
  *poolPtr        = pool;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeObjectPool(ObjectPool **poolPtr)
{
  if (*poolPtr == NULL) {
    return;
  }

  ObjectPool *pool = *poolPtr;
  ASSERT_LOG_ONLY(!hasWaiters(&pool->waiting),
                  "pool must not have any waiters when being freed");
  ASSERT_LOG_ONLY((pool->busyCount == 0),
                  "pool must not have %zu busy entries when being freed",
                  pool->busyCount);
  ASSERT_LOG_ONLY(isRingEmpty(&pool->busy),
                  "pool must not have busy entries when being freed");
  FREE(pool);
  *poolPtr = NULL;
}

/**********************************************************************/
void *getObjectPoolEntryData(ObjectPool *pool)
{
  return pool->entryData;
}

/**********************************************************************/
bool isPoolBusy(ObjectPool *pool)
{
  return (pool->busyCount != 0);
}

/**********************************************************************/
void addEntryToObjectPool(ObjectPool *pool, RingNode *entry)
{
  initializeRing(entry);
  pushRingNode(&pool->available, entry);
  pool->poolSize++;
}

/**********************************************************************/
RingNode *removeEntryFromObjectPool(ObjectPool *pool)
{
  RingNode *entry = chopRingNode(&pool->available);
  if (entry != NULL) {
    pool->poolSize--;
  }

  return entry;
}

/**
 * Grant an object from a pool to a waiter.
 *
 * @param pool    The object pool
 * @param waiter  The waiter receiving the object
 **/
static void grantObject(ObjectPool *pool, Waiter *waiter)
{
  pool->busyCount++;
  RingNode *entry = chopRingNode(&pool->available);
  pushRingNode(&pool->busy, entry);
  (*waiter->callback)(waiter, entry);
}

/**********************************************************************/
int acquireEntryFromObjectPool(ObjectPool *pool, Waiter *waiter)
{
  bool suspended = isQuiescent(&pool->adminState);
  if (!suspended && !isRingEmpty(&pool->available)) {
    grantObject(pool, waiter);
    return VDO_SUCCESS;
  }

  if (suspended) {
    pool->suspendCount++;
  } else {
    pool->outageCount++;
  }

  return enqueueWaiter(&pool->waiting, waiter);
}

/**********************************************************************/
void returnEntryToObjectPool(ObjectPool *pool, RingNode *entry)
{
  ASSERT_LOG_ONLY(!isQuiescent(&pool->adminState),
                  "Quiescent object pool has no outstanding objects");
  if (hasWaiters(&pool->waiting)) {
    notifyNextWaiter(&pool->waiting, NULL, entry);
    return;
  }

  pushRingNode(&pool->available, entry);
  if (--pool->busyCount == 0) {
    finishDraining(&pool->adminState);
  }
}

/**********************************************************************/
void drainObjectPool(ObjectPool     *pool,
                     AdminStateCode  drainRequest,
                     VDOCompletion  *completion)
{
  if (startDraining(&pool->adminState, drainRequest, completion)
      && (pool->busyCount == 0)) {
    finishDraining(&pool->adminState);
  }
}

/**********************************************************************/
void resumeObjectPool(ObjectPool *pool)
{
  if (!resumeIfQuiescent(&pool->adminState)) {
    return;
  }

  pool->outageCount  += pool->suspendCount;
  pool->suspendCount  = 0;
  while (hasWaiters(&pool->waiting) && !isRingEmpty(&pool->available)) {
    grantObject(pool, dequeueNextWaiter(&pool->waiting));
  }
}

/**********************************************************************/
uint64_t getObjectPoolOutageCount(ObjectPool *pool)
{
  return pool->outageCount;
}
