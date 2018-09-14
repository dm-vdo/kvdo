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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/allocatingVIO.c#3 $
 */

#include "allocatingVIO.h"

#include "logger.h"

#include "blockAllocator.h"
#include "dataVIO.h"
#include "pbnLock.h"
#include "slabDepot.h"
#include "vdoInternal.h"
#include "vioWrite.h"

/**
 * Make a single attempt to acquire a write lock on a newly-allocated PBN.
 *
 * @param allocatingVIO  The AllocatingVIO that wants a write lock for its
 *                       newly allocated block
 *
 * @return VDO_SUCCESS or an error code
 **/
static int attemptPBNWriteLock(AllocatingVIO *allocatingVIO)
{
  assertInPhysicalZone(allocatingVIO);

  ASSERT_LOG_ONLY(allocatingVIO->allocationLock == NULL,
                  "must not acquire a lock while already referencing one");

  PBNLock *lock;
  int result = attemptPBNLock(allocatingVIO->zone, allocatingVIO->allocation,
                              allocatingVIO->writeLockType, &lock);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (lock->holderCount > 0) {
    // This block is already locked, which should be impossible.
    return logErrorWithStringError(VDO_LOCK_ERROR,
                                   "Newly allocated block %" PRIu64
                                   " was spuriously locked (holderCount=%u)",
                                   allocatingVIO->allocation,
                                   lock->holderCount);
  }

  // We've successfully acquired a new lock, so mark it as ours.
  lock->holderCount += 1;
  allocatingVIO->allocationLock = lock;
  assignProvisionalReference(lock);
  return VDO_SUCCESS;
}

/**
 * Attempt to allocate and lock a physical block. If successful, continue
 * along the write path.
 *
 * @param allocatingVIO  The AllocatingVIO which needs an allocation
 *
 * @return VDO_SUCCESS or an error if a block could not be allocated
 **/
static int allocateAndLockBlock(AllocatingVIO *allocatingVIO)
{
  BlockAllocator *allocator = getBlockAllocator(allocatingVIO->zone);
  int result = allocateBlock(allocator, &allocatingVIO->allocation);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = attemptPBNWriteLock(allocatingVIO);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // We got a block!
  VIO *vio      = allocatingVIOAsVIO(allocatingVIO);
  vio->physical = allocatingVIO->allocation;
  allocatingVIO->allocationCallback(allocatingVIO);
  return VDO_SUCCESS;
}

static void allocateBlockForWrite(VDOCompletion *completion);

/**
 * Retry allocating a block for write.
 *
 * @param waiter   The AllocatingVIO that was waiting to allocate
 * @param context  The context (unused)
 **/
static void
retryAllocateBlockForWrite(Waiter *waiter,
                           void   *context __attribute__((unused)))
{
  AllocatingVIO *allocatingVIO = waiterAsAllocatingVIO(waiter);
  allocateBlockForWrite(allocatingVIOAsCompletion(allocatingVIO));
}

/**
 * Attempt to enqueue an AllocatingVIO to wait for a slab to be scrubbed in the
 * current allocation zone.
 *
 * @param allocatingVIO  The AllocatingVIO which wants to allocate a block
 *
 * @return VDO_SUCCESS if the AllocatingVIO was queued, VDO_NO_SPACE if there
 *         are no slabs to be scrubbed in the current zone, or some other
 *         error
 **/
static int waitForCleanSlab(AllocatingVIO *allocatingVIO)
{
  Waiter *waiter   = allocatingVIOAsWaiter(allocatingVIO);
  waiter->callback = retryAllocateBlockForWrite;

  BlockAllocator *allocator = getBlockAllocator(allocatingVIO->zone);
  int             result    = enqueueForCleanSlab(allocator, waiter);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // We've successfully enqueued, when we come back, pretend like we've
  // never tried this allocation before.
  allocatingVIO->waitForCleanSlab   = false;
  allocatingVIO->allocationAttempts = 0;
  return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block in an AllocatingVIO's current allocation zone.
 *
 * @param allocatingVIO  The AllocatingVIO
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocateBlockInZone(AllocatingVIO *allocatingVIO)
{
  allocatingVIO->allocationAttempts++;
  int result = allocateAndLockBlock(allocatingVIO);
  if (result != VDO_NO_SPACE) {
    return result;
  }

  if (allocatingVIO->waitForCleanSlab) {
    result = waitForCleanSlab(allocatingVIO);
    if (result != VDO_NO_SPACE) {
      return result;
    }
  }

  VDO                *vdo          = getVDOFromAllocatingVIO(allocatingVIO);
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (allocatingVIO->allocationAttempts >= threadConfig->physicalZoneCount) {
    if (allocatingVIO->waitForCleanSlab) {
      // There were no free blocks in any zone, and no zone had slabs to
      // scrub.
      allocatingVIO->allocationCallback(allocatingVIO);
      return VDO_SUCCESS;
    }

    allocatingVIO->waitForCleanSlab   = true;
    allocatingVIO->allocationAttempts = 0;
  }

  // Try the next zone
  ZoneCount zoneNumber = getPhysicalZoneNumber(allocatingVIO->zone) + 1;
  if (zoneNumber == threadConfig->physicalZoneCount) {
    zoneNumber = 0;
  }
  allocatingVIO->zone = vdo->physicalZones[zoneNumber];
  launchPhysicalZoneCallback(allocatingVIO, allocateBlockForWrite,
                             THIS_LOCATION("$F;cb=allocBlockInZone"));
  return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block. This callback is registered in
 * allocateDataBlock() and allocateBlockInZone().
 *
 * @param completion  The AllocatingVIO needing an allocation
 **/
static void allocateBlockForWrite(VDOCompletion *completion)
{
  AllocatingVIO *allocatingVIO = asAllocatingVIO(completion);
  assertInPhysicalZone(allocatingVIO);
  allocatingVIOAddTraceRecord(allocatingVIO, THIS_LOCATION(NULL));
  int result = allocateBlockInZone(allocatingVIO);
  if (result != VDO_SUCCESS) {
    setCompletionResult(completion, result);
    allocatingVIO->allocationCallback(allocatingVIO);
  }
}

/**********************************************************************/
void allocateDataBlock(AllocatingVIO      *allocatingVIO,
                       PBNLockType         writeLockType,
                       AllocationCallback *callback)
{
  VIO *vio = allocatingVIOAsVIO(allocatingVIO);
  VDOCompletion *completion = vioAsCompletion(vio);

  allocatingVIO->writeLockType      = writeLockType;
  allocatingVIO->allocationCallback = callback;
  allocatingVIO->allocationAttempts = 0;
  allocatingVIO->allocation         = ZERO_BLOCK;
  allocatingVIO->zone
    = getNextAllocationZone(vio->vdo, completion->callbackThreadID);

  launchPhysicalZoneCallback(allocatingVIO, allocateBlockForWrite,
                             THIS_LOCATION("$F;cb=allocDataBlock"));
}

/**********************************************************************/
void releaseAllocationLock(AllocatingVIO *allocatingVIO)
{
  assertInPhysicalZone(allocatingVIO);
  PhysicalBlockNumber lockedPBN = allocatingVIO->allocation;
  if (hasProvisionalReference(allocatingVIO->allocationLock)) {
    allocatingVIO->allocation = ZERO_BLOCK;
  }

  releasePBNLock(allocatingVIO->zone, lockedPBN,
                 &allocatingVIO->allocationLock);
}

/**********************************************************************/
void resetAllocation(AllocatingVIO *allocatingVIO)
{
  ASSERT_LOG_ONLY(allocatingVIO->allocationLock == NULL,
                  "must not reset allocation while holding a PBN lock");

  allocatingVIOAsVIO(allocatingVIO)->physical = ZERO_BLOCK;
  allocatingVIO->zone                         = NULL;
  allocatingVIO->allocation                   = ZERO_BLOCK;
  allocatingVIO->allocationAttempts           = 0;
  allocatingVIO->waitForCleanSlab             = false;
}
