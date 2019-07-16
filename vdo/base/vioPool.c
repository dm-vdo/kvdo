/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioPool.c#4 $
 */

#include "vioPool.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "constants.h"
#include "vio.h"
#include "types.h"

/**
 * An VIOPool is a collection of preallocated VIOs.
 **/
struct vioPool {
  /** The number of objects managed by the pool */
  size_t         size;
  /** The list of objects which are available */
  RingNode       available;
  /** The queue of requestors waiting for objects from the pool */
  WaitQueue      waiting;
  /** The number of objects currently in use */
  size_t         busyCount;
  /** The list of objects which are in use */
  RingNode       busy;
  /** The number of requests when no object was available */
  uint64_t       outageCount;
  /** The ID of the thread on which this pool may be used */
  ThreadID       threadID;
  /** The buffer backing the pool's VIOs */
  char          *buffer;
  /** The pool entries */
  VIOPoolEntry   entries[];
};

/**********************************************************************/
int makeVIOPool(PhysicalLayer   *layer,
                size_t           poolSize,
                ThreadID         threadID,
                VIOConstructor  *vioConstructor,
                void            *context,
                VIOPool        **poolPtr)
{
  VIOPool *pool;
  int result = ALLOCATE_EXTENDED(VIOPool, poolSize, VIOPoolEntry, __func__,
                                 &pool);
  if (result != VDO_SUCCESS) {
    return result;
  }

  pool->threadID = threadID;
  initializeRing(&pool->available);
  initializeRing(&pool->busy);

  result = ALLOCATE(poolSize * VDO_BLOCK_SIZE, char, "VIO pool buffer",
                    &pool->buffer);
  if (result != VDO_SUCCESS) {
    freeVIOPool(&pool);
    return result;
  }

  char *ptr = pool->buffer;
  for (size_t i = 0; i < poolSize; i++) {
    VIOPoolEntry *entry = &pool->entries[i];
    entry->buffer       = ptr;
    entry->context      = context;
    result = vioConstructor(layer, entry, ptr, &entry->vio);
    if (result != VDO_SUCCESS) {
      freeVIOPool(&pool);
      return result;
    }

    ptr += VDO_BLOCK_SIZE;
    initializeRing(&entry->node);
    pushRingNode(&pool->available, &entry->node);
    pool->size++;
  }

  *poolPtr = pool;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeVIOPool(VIOPool **poolPtr)
{
  if (*poolPtr == NULL) {
    return;
  }

  // Remove all available entries from the object pool.
  VIOPool *pool = *poolPtr;
  ASSERT_LOG_ONLY(!hasWaiters(&pool->waiting),
                  "VIO pool must not have any waiters when being freed");
  ASSERT_LOG_ONLY((pool->busyCount == 0),
                  "VIO pool must not have %zu busy entries when being freed",
                  pool->busyCount);
  ASSERT_LOG_ONLY(isRingEmpty(&pool->busy),
                  "VIO pool must not have busy entries when being freed");

  VIOPoolEntry *entry;
  while ((entry = asVIOPoolEntry(chopRingNode(&pool->available))) != NULL) {
    freeVIO(&entry->vio);
  }

  // Make sure every VIOPoolEntry has been removed.
  for (size_t i = 0; i < pool->size; i++) {
    VIOPoolEntry *entry = &pool->entries[i];
    ASSERT_LOG_ONLY(isRingEmpty(&entry->node), "VIO Pool entry still in use:"
                    " VIO is in use for physical block %" PRIu64
                    " for operation %u",
                    entry->vio->physical,
                    entry->vio->operation);
  }

  FREE(pool->buffer);
  FREE(pool);
  *poolPtr = NULL;
}

/**********************************************************************/
bool isVIOPoolBusy(VIOPool *pool)
{
  return (pool->busyCount != 0);
}

/**********************************************************************/
int acquireVIOFromPool(VIOPool *pool, Waiter *waiter)
{
  ASSERT_LOG_ONLY((pool->threadID == getCallbackThreadID()),
                  "acquire from active VIOPool called from correct thread");

  if (isRingEmpty(&pool->available)) {
    pool->outageCount++;
    return enqueueWaiter(&pool->waiting, waiter);
  }

  pool->busyCount++;
  RingNode *entry = chopRingNode(&pool->available);
  pushRingNode(&pool->busy, entry);
  (*waiter->callback)(waiter, entry);
  return VDO_SUCCESS;
}

/**********************************************************************/
void returnVIOToPool(VIOPool *pool, VIOPoolEntry *entry)
{
  ASSERT_LOG_ONLY((pool->threadID == getCallbackThreadID()),
                  "vio pool entry returned on same thread as it was acquired");
  entry->vio->completion.errorHandler = NULL;
  if (hasWaiters(&pool->waiting)) {
    notifyNextWaiter(&pool->waiting, NULL, entry);
    return;
  }

  pushRingNode(&pool->available, &entry->node);
  --pool->busyCount;
}

/**********************************************************************/
uint64_t getVIOPoolOutageCount(VIOPool *pool)
{
  return pool->outageCount;
}
