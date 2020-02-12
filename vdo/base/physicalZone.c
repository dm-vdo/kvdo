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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/physicalZone.c#12 $
 */

#include "physicalZone.h"

#include "memoryAlloc.h"

#include "blockAllocator.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "dataVIO.h"
#include "flush.h"
#include "hashLock.h"
#include "intMap.h"
#include "pbnLock.h"
#include "pbnLockPool.h"
#include "slabDepot.h"
#include "vdoInternal.h"

enum {
  // Each user DataVIO needs a PBN read lock and write lock, and each packer
  // output bin has an AllocatingVIO that needs a PBN write lock.
  LOCK_POOL_CAPACITY = 2 * MAXIMUM_USER_VIOS + DEFAULT_PACKER_OUTPUT_BINS,
};

struct physical_zone {
  /** Which physical zone this is */
  ZoneCount               zoneNumber;
  /** The thread ID for this zone */
  ThreadID                threadID;
  /** In progress operations keyed by PBN */
  struct int_map         *pbnOperations;
  /** Pool of unused pbn_lock instances */
  struct pbn_lock_pool   *lockPool;
  /** The block allocator for this zone */
  struct block_allocator *allocator;
};

/**********************************************************************/
int makePhysicalZone(struct vdo            *vdo,
                     ZoneCount              zoneNumber,
                     struct physical_zone **zonePtr)
{
  struct physical_zone *zone;
  int result = ALLOCATE(1, struct physical_zone, __func__, &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = make_int_map(LOCK_MAP_CAPACITY, 0, &zone->pbnOperations);
  if (result != VDO_SUCCESS) {
    freePhysicalZone(&zone);
    return result;
  }

  result = make_pbn_lock_pool(LOCK_POOL_CAPACITY, &zone->lockPool);
  if (result != VDO_SUCCESS) {
    freePhysicalZone(&zone);
    return result;
  }

  zone->zoneNumber = zoneNumber;
  zone->threadID   = getPhysicalZoneThread(getThreadConfig(vdo), zoneNumber);
  zone->allocator  = getBlockAllocatorForZone(vdo->depot, zoneNumber);

  *zonePtr = zone;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freePhysicalZone(struct physical_zone **zonePtr)
{
  if (*zonePtr == NULL) {
    return;
  }

  struct physical_zone *zone = *zonePtr;
  free_pbn_lock_pool(&zone->lockPool);
  free_int_map(&zone->pbnOperations);
  FREE(zone);
  *zonePtr = NULL;
}

/**********************************************************************/
ZoneCount getPhysicalZoneNumber(const struct physical_zone *zone)
{
  return zone->zoneNumber;
}

/**********************************************************************/
ThreadID getPhysicalZoneThreadID(const struct physical_zone *zone)
{
  return zone->threadID;
}

/**********************************************************************/
struct block_allocator *getBlockAllocator(const struct physical_zone *zone)
{
  return zone->allocator;
}

/**********************************************************************/
struct pbn_lock *getPBNLock(struct physical_zone *zone, PhysicalBlockNumber pbn)
{
  return ((zone == NULL) ? NULL : int_map_get(zone->pbnOperations, pbn));
}

/**********************************************************************/
int attemptPBNLock(struct physical_zone  *zone,
                   PhysicalBlockNumber    pbn,
                   pbn_lock_type          type,
                   struct pbn_lock      **lockPtr)
{
  // Borrow and prepare a lock from the pool so we don't have to do two int_map
  // accesses in the common case of no lock contention.
  struct pbn_lock *newLock;
  int result = borrow_pbn_lock_from_pool(zone->lockPool, type, &newLock);
  if (result != VDO_SUCCESS) {
    ASSERT_LOG_ONLY(false, "must always be able to borrow a PBN lock");
    return result;
  }

  struct pbn_lock *lock;
  result = int_map_put(zone->pbnOperations, pbn, newLock, false,
                       (void **) &lock);
  if (result != VDO_SUCCESS) {
    return_pbn_lock_to_pool(zone->lockPool, &newLock);
    return result;
  }

  if (lock != NULL) {
    // The lock is already held, so we don't need the borrowed lock.
    return_pbn_lock_to_pool(zone->lockPool, &newLock);

    result = ASSERT(lock->holder_count > 0,
                    "physical block %llu lock held", pbn);
    if (result != VDO_SUCCESS) {
      return result;
    }
    *lockPtr = lock;
  } else {
    *lockPtr = newLock;
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
void releasePBNLock(struct physical_zone  *zone,
                    PhysicalBlockNumber    lockedPBN,
                    struct pbn_lock      **lockPtr)
{
  struct pbn_lock *lock = *lockPtr;
  if (lock == NULL) {
    return;
  }
  *lockPtr = NULL;

  ASSERT_LOG_ONLY(lock->holder_count > 0,
                  "should not be releasing a lock that is not held");

  lock->holder_count -= 1;
  if (lock->holder_count > 0) {
    // The lock was shared and is still referenced, so don't release it yet.
    return;
  }

  struct pbn_lock *holder = int_map_remove(zone->pbnOperations, lockedPBN);
  ASSERT_LOG_ONLY((lock == holder),
                  "physical block lock mismatch for block %llu",
                  lockedPBN);

  release_provisional_reference(lock, lockedPBN, zone->allocator);

  return_pbn_lock_to_pool(zone->lockPool, &lock);
}

/**********************************************************************/
void dumpPhysicalZone(const struct physical_zone *zone)
{
  dump_block_allocator(zone->allocator);
}
