/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashZone.c#8 $
 */

#include "hashZone.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "hashLockInternals.h"
#include "pointerMap.h"
#include "ringNode.h"
#include "statistics.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoInternal.h"

enum {
  LOCK_POOL_CAPACITY = MAXIMUM_USER_VIOS,
};

/**
 * These fields are only modified by the locks sharing the hash zone thread,
 * but are queried by other threads.
 **/
struct atomic_hash_lock_statistics {
  /** Number of times the UDS advice proved correct */
  Atomic64 dedupeAdviceValid;

  /** Number of times the UDS advice proved incorrect */
  Atomic64 dedupeAdviceStale;

  /** Number of writes with the same data as another in-flight write */
  Atomic64 concurrentDataMatches;

  /** Number of writes whose hash collided with an in-flight write */
  Atomic64 concurrentHashCollisions;
};

struct hash_zone {
  /** Which hash zone this is */
  ZoneCount	                     zoneNumber;

  /** The thread ID for this zone */
  ThreadID                           threadID;

  /** Mapping from chunkName fields to HashLocks */
  struct pointer_map                *hashLockMap;

  /** Ring containing all unused HashLocks */
  RingNode                           lockPool;

  /** Statistics shared by all hash locks in this zone */
  struct atomic_hash_lock_statistics statistics;

  /** Array of all HashLocks */
  struct hash_lock                  *lockArray;
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
static inline struct hash_lock *asHashLock(RingNode *poolNode)
{
  STATIC_ASSERT(offsetof(struct hash_lock, pool_node) == 0);
  return (struct hash_lock *) poolNode;
}

/**********************************************************************/
int makeHashZone(struct vdo        *vdo,
                 ZoneCount          zoneNumber,
                 struct hash_zone **zonePtr)
{
  struct hash_zone *zone;
  int result = ALLOCATE(1, struct hash_zone, __func__, &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makePointerMap(LOCK_MAP_CAPACITY, 0, compareKeys, hashKey,
                          &zone->hashLockMap);
  if (result != VDO_SUCCESS) {
    freeHashZone(&zone);
    return result;
  }

  zone->zoneNumber = zoneNumber;
  zone->threadID   = getHashZoneThread(getThreadConfig(vdo), zoneNumber);
  initializeRing(&zone->lockPool);

  result = ALLOCATE(LOCK_POOL_CAPACITY, struct hash_lock,
                    "hash_lock array", &zone->lockArray);
  if (result != VDO_SUCCESS) {
    freeHashZone(&zone);
    return result;
  }

  VIOCount i;
  for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
    struct hash_lock *lock = &zone->lockArray[i];
    initialize_hash_lock(lock);
    pushRingNode(&zone->lockPool, &lock->pool_node);
  }

  *zonePtr = zone;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeHashZone(struct hash_zone **zonePtr)
{
  if (*zonePtr == NULL) {
    return;
  }

  struct hash_zone *zone = *zonePtr;
  freePointerMap(&zone->hashLockMap);
  FREE(zone->lockArray);
  FREE(zone);
  *zonePtr = NULL;
}

/**********************************************************************/
ZoneCount getHashZoneNumber(const struct hash_zone *zone)
{
  return zone->zoneNumber;
}

/**********************************************************************/
ThreadID getHashZoneThreadID(const struct hash_zone *zone)
{
  return zone->threadID;
}

/**********************************************************************/
HashLockStatistics getHashZoneStatistics(const struct hash_zone *zone)
{
  const struct atomic_hash_lock_statistics *atoms = &zone->statistics;
  return (HashLockStatistics) {
    .dedupeAdviceValid     = relaxedLoad64(&atoms->dedupeAdviceValid),
    .dedupeAdviceStale     = relaxedLoad64(&atoms->dedupeAdviceStale),
    .concurrentDataMatches = relaxedLoad64(&atoms->concurrentDataMatches),
    .concurrentHashCollisions
      = relaxedLoad64(&atoms->concurrentHashCollisions),
  };
}

/**
 * Return a hash lock to the zone's pool and null out the reference to it.
 *
 * @param [in]     zone     The zone from which the lock was borrowed
 * @param [in,out] lockPtr  The last reference to the lock being returned
 **/
static void returnHashLockToPool(struct hash_zone  *zone,
                                 struct hash_lock **lockPtr)
{
  struct hash_lock *lock = *lockPtr;
  *lockPtr = NULL;

  memset(lock, 0, sizeof(*lock));
  initialize_hash_lock(lock);
  pushRingNode(&zone->lockPool, &lock->pool_node);
}

/**********************************************************************/
int acquireHashLockFromZone(struct hash_zone    *zone,
                            const UdsChunkName  *hash,
                            struct hash_lock    *replaceLock,
                            struct hash_lock   **lockPtr)
{
  // Borrow and prepare a lock from the pool so we don't have to do two
  // pointer_map accesses in the common case of no lock contention.
  struct hash_lock *newLock = asHashLock(popRingNode(&zone->lockPool));
  int result = ASSERT(newLock != NULL,
                      "never need to wait for a free hash lock");
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Fill in the hash of the new lock so we can map it, since we have to use
  // the hash as the map key.
  newLock->hash = *hash;

  struct hash_lock *lock;
  result = pointerMapPut(zone->hashLockMap, &newLock->hash, newLock,
                         (replaceLock != NULL), (void **) &lock);
  if (result != VDO_SUCCESS) {
    returnHashLockToPool(zone, &newLock);
    return result;
  }

  if (replaceLock != NULL) {
    // XXX on mismatch put the old lock back and return a severe error
    ASSERT_LOG_ONLY(lock == replaceLock,
                    "old lock must have been in the lock map");
    // XXX check earlier and bail out?
    ASSERT_LOG_ONLY(replaceLock->registered,
                    "old lock must have been marked registered");
    replaceLock->registered = false;
  }

  if (lock == replaceLock) {
    lock = newLock;
    lock->registered = true;
  } else {
    // There's already a lock for the hash, so we don't need the borrowed lock.
    returnHashLockToPool(zone, &newLock);
  }

  *lockPtr = lock;
  return VDO_SUCCESS;
}

/**********************************************************************/
void returnHashLockToZone(struct hash_zone *zone, struct hash_lock **lockPtr)
{
  struct hash_lock *lock = *lockPtr;
  *lockPtr = NULL;

  if (lock->registered) {
    struct hash_lock *removed = pointerMapRemove(zone->hashLockMap,
                                                 &lock->hash);
    ASSERT_LOG_ONLY(lock == removed,
                    "hash lock being released must have been mapped");
  } else {
    ASSERT_LOG_ONLY(lock != pointerMapGet(zone->hashLockMap, &lock->hash),
                    "unregistered hash lock must not be in the lock map");
  }

  ASSERT_LOG_ONLY(!hasWaiters(&lock->waiters),
                  "hash lock returned to zone must have no waiters");
  ASSERT_LOG_ONLY((lock->duplicate_lock == NULL),
                  "hash lock returned to zone must not reference a PBN lock");
  ASSERT_LOG_ONLY((lock->state == HASH_LOCK_DESTROYING),
                  "returned hash lock must not be in use with state %s",
                  get_hash_lock_state_name(lock->state));
  ASSERT_LOG_ONLY(isRingEmpty(&lock->pool_node),
                  "hash lock returned to zone must not be in a pool ring");
  ASSERT_LOG_ONLY(isRingEmpty(&lock->duplicate_ring),
                  "hash lock returned to zone must not reference DataVIOs");

  returnHashLockToPool(zone, &lock);
}

/**
 * Dump a compact description of hash_lock to the log if the lock is not on the
 * free list.
 *
 * @param lock  The hash lock to dump
 **/
static void dumpHashLock(const struct hash_lock *lock)
{
  if (!isRingEmpty(&lock->pool_node)) {
    // This lock is on the free list.
    return;
  }

  // Necessarily cryptic since we can log a lot of these. First three chars of
  // state is unambiguous. 'U' indicates a lock not registered in the map.
  const char *state = get_hash_lock_state_name(lock->state);
  logInfo("  hl %" PRIptr ": %3.3s %c%llu/%u rc=%u wc=%zu agt=%" PRIptr,
          (const void *) lock,
          state,
          (lock->registered ? 'D' : 'U'),
          lock->duplicate.pbn,
          lock->duplicate.state,
          lock->reference_count,
          countWaiters(&lock->waiters),
          (void *) lock->agent);
}

/**********************************************************************/
void bumpHashZoneValidAdviceCount(struct hash_zone *zone)
{
  // Must only be mutated on the hash zone thread.
  relaxedAdd64(&zone->statistics.dedupeAdviceValid, 1);
}

/**********************************************************************/
void bumpHashZoneStaleAdviceCount(struct hash_zone *zone)
{
  // Must only be mutated on the hash zone thread.
  relaxedAdd64(&zone->statistics.dedupeAdviceStale, 1);
}

/**********************************************************************/
void bumpHashZoneDataMatchCount(struct hash_zone *zone)
{
  // Must only be mutated on the hash zone thread.
  relaxedAdd64(&zone->statistics.concurrentDataMatches, 1);
}

/**********************************************************************/
void bumpHashZoneCollisionCount(struct hash_zone *zone)
{
  // Must only be mutated on the hash zone thread.
  relaxedAdd64(&zone->statistics.concurrentHashCollisions, 1);
}

/**********************************************************************/
void dumpHashZone(const struct hash_zone *zone)
{
  if (zone->hashLockMap == NULL) {
    logInfo("struct hash_zone %u: NULL map", zone->zoneNumber);
    return;
  }

  logInfo("struct hash_zone %u: mapSize=%zu",
          zone->zoneNumber, pointerMapSize(zone->hashLockMap));
  VIOCount i;
  for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
    dumpHashLock(&zone->lockArray[i]);
  }
}
