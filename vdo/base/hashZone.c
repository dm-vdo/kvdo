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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/hashZone.c#1 $
 */

#include "hashZone.h"

#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "hashLockInternals.h"
#include "pointerMap.h"
#include "ringNode.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoInternal.h"

enum {
  LOCK_POOL_CAPACITY = MAXIMUM_USER_VIOS,
};

struct hashZone {
  /** Which hash zone this is */
  ZoneCount         zoneNumber;
  /** The per-thread data for this zone */
  const ThreadData *threadData;
  /** Mapping from chunkName fields to HashLocks */
  PointerMap       *hashLockMap;
  /** Ring containing all unused HashLocks */
  RingNode         lockPool;
  /** Array of all HashLocks */
  HashLock         *lockArray;
};

/**
 * Implements PointerKeyComparator.
 **/
static bool compareKeys(const void *thisKey, const void *thatKey)
{
  // Null keys are not supported.
  return (memcmp(thisKey, thatKey, sizeof(UdsChunkName)) == 0);
}

/**
 * Implements PointerKeyComparator.
 **/
static uint32_t hashKey(const void *key)
{
  const UdsChunkName *name = key;
  /*
   * Use a fragment of the chunk name as a hash code. It must not overlap with
   * fragments used elsewhere to ensure uniform distributions.
   */
  // XXX pick an offset in the chunk name that isn't used elsewhere
  return getUInt32LE(&name->name[4]);
}

/**********************************************************************/
static inline HashLock *asHashLock(RingNode *poolNode)
{
  STATIC_ASSERT(offsetof(HashLock, poolNode) == 0);
  return (HashLock *) poolNode;
}

/**********************************************************************/
int makeHashZone(VDO *vdo, ZoneCount zoneNumber, HashZone **zonePtr)
{
  HashZone *zone;
  int result = ALLOCATE(1, HashZone, __func__, &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makePointerMap(LOCK_MAP_CAPACITY, 0, compareKeys, hashKey,
                          &zone->hashLockMap);
  if (result != VDO_SUCCESS) {
    freeHashZone(&zone);
    return result;
  }

  ThreadID threadID = getHashZoneThread(getThreadConfig(vdo), zoneNumber);
  zone->zoneNumber  = zoneNumber;
  zone->threadData  = &vdo->threadData[threadID];
  ASSERT_LOG_ONLY(threadID == getHashZoneThreadID(zone),
                  "thread IDs in config and thread data must match");
  initializeRing(&zone->lockPool);

  result = ALLOCATE(LOCK_POOL_CAPACITY, HashLock, "HashLock array",
                    &zone->lockArray);
  if (result != VDO_SUCCESS) {
    freeHashZone(&zone);
    return result;
  }

  for (VIOCount i = 0; i < LOCK_POOL_CAPACITY; i++) {
    HashLock *lock = &zone->lockArray[i];
    initializeHashLock(lock);
    pushRingNode(&zone->lockPool, &lock->poolNode);
  }

  *zonePtr = zone;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeHashZone(HashZone **zonePtr)
{
  if (*zonePtr == NULL) {
    return;
  }

  HashZone *zone = *zonePtr;
  freePointerMap(&zone->hashLockMap);
  FREE(zone->lockArray);
  FREE(zone);
  *zonePtr = NULL;
}

/**********************************************************************/
ZoneCount getHashZoneNumber(const HashZone *zone)
{
  return zone->zoneNumber;
}

/**********************************************************************/
ThreadID getHashZoneThreadID(const HashZone *zone)
{
  return zone->threadData->threadID;
}

/**
 * Return a hash lock to the zone's pool and null out the reference to it.
 *
 * @param [in]     zone     The zone from which the lock was borrowed
 * @param [in,out] lockPtr  The last reference to the lock being returned
 **/
static void returnHashLockToPool(HashZone *zone, HashLock **lockPtr)
{
  HashLock *lock = *lockPtr;
  *lockPtr = NULL;

  ASSERT_LOG_ONLY(((lock->state == HASH_LOCK_DESTROYING)
                   || (lock->state == HASH_LOCK_INITIALIZING)),
                  "returned hash lock must not be in use with state %s",
                  getHashLockStateName(lock->state));
  ASSERT_LOG_ONLY(isRingEmpty(&lock->poolNode),
                  "hash lock returned to pool must not be in a pool ring");
  ASSERT_LOG_ONLY(isRingEmpty(&lock->duplicateRing),
                  "hash lock returned to pool must not reference DataVIOs");

  memset(lock, 0, sizeof(*lock));
  initializeHashLock(lock);
  pushRingNode(&zone->lockPool, &lock->poolNode);
}

/**********************************************************************/
int acquireHashLockFromZone(HashZone            *zone,
                            const UdsChunkName  *hash,
                            HashLock           **lockPtr)
{
  // Borrow and prepare a lock from the pool so we don't have to do two
  // PointerMap accesses in the common case of no lock contention.
  HashLock *newLock = asHashLock(popRingNode(&zone->lockPool));
  int result = ASSERT(newLock != NULL,
                      "never need to wait for a free hash lock");
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Fill in the hash of the new lock so we can map it, since we have to use
  // the hash as the map key.
  newLock->hash = *hash;

  HashLock *lock;
  result = pointerMapPut(zone->hashLockMap, &newLock->hash, newLock,
                         false, (void **) &lock);
  if (result != VDO_SUCCESS) {
    returnHashLockToPool(zone, &newLock);
    return result;
  }

  if (lock == NULL) {
    // There was no existing lock, so newLock is now the lock on the hash.
    lock = newLock;
  } else {
    // There's already a lock for the hash, so we don't need the borrowed lock.
    returnHashLockToPool(zone, &newLock);
  }

  *lockPtr = lock;
  return VDO_SUCCESS;
}

/**********************************************************************/
void returnHashLockToZone(HashZone *zone, HashLock **lockPtr)
{
  HashLock *lock = *lockPtr;
  *lockPtr = NULL;

  HashLock *removed = pointerMapRemove(zone->hashLockMap, &lock->hash);
  ASSERT_LOG_ONLY(lock == removed,
                  "hash lock being released must have been mapped");

  returnHashLockToPool(zone, &lock);
}
