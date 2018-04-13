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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/objectPool.c#1 $
 */

#include "objectPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

/**
 * An ObjectPool is a collection of preallocated objects.
 **/
struct objectPool {
  size_t         poolSize;
  RingNode       available;     // entries
  WaitQueue      waiting;
  size_t         busyCount;
  RingNode       busy;          // entries
  AdminState     adminState;
  VDOCompletion *completion;
  uint64_t       outageCount;
  uint64_t       suspendCount;
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
  if (pool->adminState == ADMIN_STATE_CLOSED) {
    return VDO_SHUTTING_DOWN;
  }

  if ((pool->adminState == ADMIN_STATE_SUSPENDED)
      || isRingEmpty(&pool->available)) {
    int result = enqueueWaiter(&pool->waiting, waiter);
    if (result != VDO_SUCCESS) {
      return result;
    }

    if (pool->adminState == ADMIN_STATE_SUSPENDED) {
      pool->suspendCount++;
    } else {
      pool->outageCount++;
    }

    return VDO_SUCCESS;
  }

  grantObject(pool, waiter);
  return VDO_SUCCESS;
}

/**********************************************************************/
static void checkNotBusy(ObjectPool *pool)
{
  if (pool->busyCount != 0) {
    return;
  }

  VDOCompletion *completion = pool->completion;
  if (completion != NULL) {
    pool->completion = NULL;
    if (pool->adminState == ADMIN_STATE_CLOSING) {
      pool->adminState = ADMIN_STATE_CLOSED;
    }
    finishCompletion(completion, VDO_SUCCESS);
  }
}

/**********************************************************************/
void returnEntryToObjectPool(ObjectPool *pool, RingNode *entry)
{
  if ((pool->adminState != ADMIN_STATE_SUSPENDED)
      && hasWaiters(&pool->waiting)) {
    notifyNextWaiter(&pool->waiting, NULL, entry);
    return;
  }

  pool->busyCount--;
  pushRingNode(&pool->available, entry);
  if (pool->adminState != ADMIN_STATE_NORMAL_OPERATION) {
    checkNotBusy(pool);
  }
}

/**********************************************************************/
void suspendObjectPool(ObjectPool *pool, VDOCompletion *completion)
{
  ASSERT_LOG_ONLY((pool->adminState == ADMIN_STATE_NORMAL_OPERATION),
                  "object pool in normal operation");
  pool->completion = completion;
  pool->adminState = ADMIN_STATE_SUSPENDED;
  checkNotBusy(pool);
}

/**********************************************************************/
void resumeObjectPool(ObjectPool *pool)
{
  if (!(pool->adminState == ADMIN_STATE_SUSPENDED)) {
    return;
  }

  pool->adminState    = ADMIN_STATE_NORMAL_OPERATION;
  pool->outageCount  += pool->suspendCount;
  pool->suspendCount  = 0;
  while (hasWaiters(&pool->waiting) && !isRingEmpty(&pool->available)) {
    grantObject(pool, dequeueNextWaiter(&pool->waiting));
  }
}

/**********************************************************************/
void openObjectPool(ObjectPool *pool)
{
  ASSERT_LOG_ONLY((pool->adminState == ADMIN_STATE_CLOSED),
                  "object pool is closed");
  pool->adminState = ADMIN_STATE_NORMAL_OPERATION;
}

/**********************************************************************/
void closeObjectPool(ObjectPool *pool, VDOCompletion *completion)
{
  ASSERT_LOG_ONLY((pool->adminState == ADMIN_STATE_NORMAL_OPERATION),
                  "object pool in normal operation");
  pool->completion = completion;
  pool->adminState = ADMIN_STATE_CLOSING;
  checkNotBusy(pool);
}

/**********************************************************************/
uint64_t getObjectPoolOutageCount(ObjectPool *pool)
{
  return pool->outageCount;
}
