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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dataVIO.c#16 $
 */

#include "dataVIO.h"

#include "logger.h"

#include "atomic.h"
#include "blockMap.h"
#include "compressionState.h"
#include "extent.h"
#include "logicalZone.h"
#include "threadConfig.h"
#include "vdoInternal.h"
#include "vioRead.h"
#include "vioWrite.h"

static const char *ASYNC_OPERATION_NAMES[] = {
  "launch",
  "acknowledgeWrite",
  "acquire_hash_lock",
  "acquireLogicalBlockLock",
  "acquirePBNReadLock",
  "checkForDedupeForRollover",
  "checkForDeduplication",
  "compressData",
  "continueVIOAsync",
  "findBlockMapSlot",
  "getMappedBlock",
  "getMappedBlockForDedupe",
  "getMappedBlockForWrite",
  "hashData",
  "journalDecrementForDedupe",
  "journalDecrementForWrite",
  "journalIncrementForCompression",
  "journalIncrementForDedupe",
  "journalIncrementForWrite",
  "journalMappingForCompression",
  "journalMappingForDedupe",
  "journalMappingForWrite",
  "journalUnmappingForDedupe",
  "journalUnmappingForWrite",
  "attemptPacking",
  "putMappedBlock",
  "putMappedBlockForDedupe",
  "readData",
  "updateIndex",
  "verifyDeduplication",
  "writeData",
};

/**
 * Initialize the LBN lock of a data_vio. In addition to recording the LBN on
 * which the data_vio will operate, it will also find the logical zone
 * associated with the LBN.
 *
 * @param dataVIO  The dataVIO to initialize
 * @param lbn      The lbn on which the dataVIO will operate
 **/
static void initializeLBNLock(struct data_vio *dataVIO, LogicalBlockNumber lbn)
{
  struct lbn_lock *lock = &dataVIO->logical;
  lock->lbn     = lbn;
  lock->locked  = false;
  initializeWaitQueue(&lock->waiters);

  struct vdo *vdo = getVDOFromDataVIO(dataVIO);
  lock->zone      = getLogicalZone(vdo->logicalZones,
                                   computeLogicalZone(dataVIO));
}

/**********************************************************************/
void prepareDataVIO(struct data_vio    *dataVIO,
                    LogicalBlockNumber  lbn,
                    VIOOperation        operation,
                    bool                isTrim,
                    VDOAction          *callback)
{
  // Clearing the tree lock must happen before initializing the LBN lock,
  // which also adds information to the tree lock.
  memset(&dataVIO->treeLock,  0, sizeof(dataVIO->treeLock));
  initializeLBNLock(dataVIO, lbn);
  initializeRing(&dataVIO->hashLockNode);
  initializeRing(&dataVIO->writeNode);

  reset_allocation(dataVIOAsAllocatingVIO(dataVIO));

  dataVIO->isDuplicate = false;

  memset(&dataVIO->chunkName, 0, sizeof(dataVIO->chunkName));
  memset(&dataVIO->duplicate, 0, sizeof(dataVIO->duplicate));

  struct vio *vio = dataVIOAsVIO(dataVIO);
  vio->operation  = operation;
  vio->callback   = callback;
  dataVIO->pageCompletion.completion.enqueueable
    = vioAsCompletion(vio)->enqueueable;

  dataVIO->mapped.state = MAPPING_STATE_UNCOMPRESSED;
  dataVIO->newMapped.state
    = (isTrim ? MAPPING_STATE_UNMAPPED : MAPPING_STATE_UNCOMPRESSED);
  resetCompletion(vioAsCompletion(vio));
  setLogicalCallback(dataVIO, attemptLogicalBlockLock,
                     THIS_LOCATION("$F;cb=acquireLogicalBlockLock"));
}

/**********************************************************************/
void completeDataVIO(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  if (completion->result != VDO_SUCCESS) {
    struct vio *vio = dataVIOAsVIO(dataVIO);
    updateVIOErrorStats(vio,
                        "Completing %s vio for LBN %" PRIu64
                        " with error after %s",
                        getVIOReadWriteFlavor(vio), dataVIO->logical.lbn,
                        getOperationName(dataVIO));
  }

  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F($io)"));
  if (isReadDataVIO(dataVIO)) {
    cleanupReadDataVIO(dataVIO);
  } else {
    cleanupWriteDataVIO(dataVIO);
  }
}

/**********************************************************************/
void finishDataVIO(struct data_vio *dataVIO, int result)
{
  struct vdo_completion *completion = dataVIOAsCompletion(dataVIO);
  setCompletionResult(completion, result);
  completeDataVIO(completion);
}

/**********************************************************************/
const char *getOperationName(struct data_vio *dataVIO)
{
  STATIC_ASSERT((MAX_ASYNC_OPERATION_NUMBER - MIN_ASYNC_OPERATION_NUMBER)
                == COUNT_OF(ASYNC_OPERATION_NAMES));

  return ((dataVIO->lastAsyncOperation < MAX_ASYNC_OPERATION_NUMBER)
          ? ASYNC_OPERATION_NAMES[dataVIO->lastAsyncOperation]
          : "unknown async operation");
}

/**********************************************************************/
void receiveDedupeAdvice(struct data_vio            *dataVIO,
                         const struct data_location *advice)
{
  /*
   * NOTE: this is called on non-base-code threads. Be very careful to not do
   * anything here that needs a base code thread-local variable, such as
   * trying to get the current thread ID, or that does a lot of work.
   */

  struct vdo *vdo = getVDOFromDataVIO(dataVIO);
  struct zoned_pbn duplicate = validateDedupeAdvice(vdo, advice,
                                                    dataVIO->logical.lbn);
  setDuplicateLocation(dataVIO, duplicate);
}

/**********************************************************************/
void setDuplicateLocation(struct data_vio        *dataVIO,
                          const struct zoned_pbn  source)
{
  dataVIO->isDuplicate = (source.pbn != ZERO_BLOCK);
  dataVIO->duplicate   = source;
}

/**********************************************************************/
void clearMappedLocation(struct data_vio *dataVIO)
{
  dataVIO->mapped = (struct zoned_pbn) { .state = MAPPING_STATE_UNMAPPED };
}

/**********************************************************************/
int setMappedLocation(struct data_vio     *dataVIO,
                      PhysicalBlockNumber  pbn,
                      BlockMappingState    state)
{
  struct physical_zone *zone;
  int result = getPhysicalZone(getVDOFromDataVIO(dataVIO), pbn, &zone);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dataVIO->mapped = (struct zoned_pbn) {
    .pbn   = pbn,
    .state = state,
    .zone  = zone,
  };
  return VDO_SUCCESS;
}

/**
 * Launch a request which has acquired an LBN lock.
 *
 * @param dataVIO  The data_vio which has just acquired a lock
 **/
static void launchLockedRequest(struct data_vio *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  dataVIO->logical.locked = true;

  if (isWriteDataVIO(dataVIO)) {
    launchWriteDataVIO(dataVIO);
  } else {
    launchReadDataVIO(dataVIO);
  }
}

/**********************************************************************/
void attemptLogicalBlockLock(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);

  if (dataVIO->logical.lbn
      >= getVDOFromDataVIO(dataVIO)->config.logicalBlocks) {
    finishDataVIO(dataVIO, VDO_OUT_OF_RANGE);
    return;
  }

  struct data_vio *lockHolder;
  struct lbn_lock *lock = &dataVIO->logical;
  int result = int_map_put(getLBNLockMap(lock->zone), lock->lbn, dataVIO, false,
                           (void **) &lockHolder);
  if (result != VDO_SUCCESS) {
    finishDataVIO(dataVIO, result);
    return;
  }

  if (lockHolder == NULL) {
    // We got the lock
    launchLockedRequest(dataVIO);
    return;
  }

  result = ASSERT(lockHolder->logical.locked, "logical block lock held");
  if (result != VDO_SUCCESS) {
    finishDataVIO(dataVIO, result);
    return;
  }

  /*
   * If the new request is a pure read request (not read-modify-write) and
   * the lockHolder is writing and has received an allocation (VDO-2683),
   * service the read request immediately by copying data from the lockHolder
   * to avoid having to flush the write out of the packer just to prevent the
   * read from waiting indefinitely. If the lockHolder does not yet have an
   * allocation, prevent it from blocking in the packer and wait on it.
   */
  if (isReadDataVIO(dataVIO) && atomicLoadBool(&lockHolder->hasAllocation)) {
    copyData(lockHolder, dataVIO);
    finishDataVIO(dataVIO, VDO_SUCCESS);
    return;
  }

  dataVIO->lastAsyncOperation = ACQUIRE_LOGICAL_BLOCK_LOCK;
  result = enqueueDataVIO(&lockHolder->logical.waiters, dataVIO,
                          THIS_LOCATION("$F;cb=logicalBlockLock"));
  if (result != VDO_SUCCESS) {
    finishDataVIO(dataVIO, result);
    return;
  }

  // Prevent writes and read-modify-writes from blocking indefinitely on
  // lock holders in the packer.
  if (!isReadDataVIO(lockHolder) && cancel_compression(lockHolder)) {
    dataVIO->compression.lockHolder = lockHolder;
    launchPackerCallback(dataVIO, removeLockHolderFromPacker,
                         THIS_LOCATION("$F;cb=removeLockHolderFromPacker"));
  }
}

/**
 * Release an uncontended LBN lock.
 *
 * @param dataVIO  The data_vio holding the lock
 **/
static void releaseLock(struct data_vio *dataVIO)
{
  struct lbn_lock *lock    = &dataVIO->logical;
  struct int_map  *lockMap = getLBNLockMap(lock->zone);
  if (!lock->locked) {
    // The lock is not locked, so it had better not be registered in the lock
    // map.
    struct data_vio *lockHolder = int_map_get(lockMap, lock->lbn);
    ASSERT_LOG_ONLY((dataVIO != lockHolder),
                    "no logical block lock held for block %llu",
                    lock->lbn);
    return;
  }

  // Remove the lock from the logical block lock map, releasing the lock.
  struct data_vio *lockHolder = int_map_remove(lockMap, lock->lbn);
  ASSERT_LOG_ONLY((dataVIO == lockHolder),
                  "logical block lock mismatch for block %llu", lock->lbn);
  lock->locked = false;
  return;
}

/**********************************************************************/
void releaseLogicalBlockLock(struct data_vio *dataVIO)
{
  assertInLogicalZone(dataVIO);
  if (!hasWaiters(&dataVIO->logical.waiters)) {
    releaseLock(dataVIO);
    return;
  }

  struct lbn_lock *lock = &dataVIO->logical;
  ASSERT_LOG_ONLY(lock->locked, "lbn_lock with waiters is not locked");

  // Another data_vio is waiting for the lock, so just transfer it in a single
  // lock map operation
  struct data_vio *nextLockHolder = waiterAsDataVIO(dequeueNextWaiter(&lock->waiters));

  // Transfer the remaining lock waiters to the next lock holder.
  transferAllWaiters(&lock->waiters, &nextLockHolder->logical.waiters);

  struct data_vio *lockHolder;
  int result = int_map_put(getLBNLockMap(lock->zone), lock->lbn, nextLockHolder,
                           true, (void **) &lockHolder);
  if (result != VDO_SUCCESS) {
    finishDataVIO(nextLockHolder, result);
    return;
  }

  ASSERT_LOG_ONLY((lockHolder == dataVIO),
                  "logical block lock mismatch for block %llu", lock->lbn);
  lock->locked = false;

  /*
   * If there are still waiters, other DataVIOs must be trying to get the lock
   * we just transferred. We must ensure that the new lock holder doesn't block
   * in the packer.
   */
  if (hasWaiters(&nextLockHolder->logical.waiters)) {
    cancel_compression(nextLockHolder);
  }

  // Avoid stack overflow on lock transfer.
  // XXX: this is only an issue in the 1 thread config.
  dataVIOAsCompletion(nextLockHolder)->requeue = true;
  launchLockedRequest(nextLockHolder);
}
