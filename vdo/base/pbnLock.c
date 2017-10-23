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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/pbnLock.c#1 $
 */

#include "pbnLock.h"

#include "logger.h"

#include "allocatingVIO.h"
#include "blockAllocator.h"
#include "dataVIO.h"
#include "intMap.h"
#include "physicalZone.h"
#include "vioWrite.h"
#include "waitQueue.h"

typedef void LockQueuer(DataVIO *dataVIO, PBNLock *lock);

struct pbnLockImplementation {
  PBNLockType     type;
  const char     *name;
  const char     *releaseReason;
  LockQueuer     *waitOnLock;
  WaiterCallback *transferLock;
};

static void getPBNReadLockAfterCompressedWriteLock(Waiter *waiter,
                                                   void   *context);
static void getPBNReadLockAfterWriteLock(Waiter *waiter, void *context);
static void getPBNReadLockAfterReadLock(Waiter *waiter, void *context);

/**
 * This array must have an entry for every PBNLockType value.
 **/
static const PBNLockImplementation LOCK_IMPLEMENTATIONS[] = {
  [VIO_READ_LOCK] = {
    .type          = VIO_READ_LOCK,
    .name          = "read",
    .releaseReason = "candidate duplicate",
    .waitOnLock    = waitOnPBNReadLock,
    .transferLock  = getPBNReadLockAfterReadLock,
  },
  [VIO_WRITE_LOCK] = {
    .type          = VIO_WRITE_LOCK,
    .name          = "write",
    .releaseReason = "newly allocated",
    .waitOnLock    = waitOnPBNWriteLock,
    .transferLock  = getPBNReadLockAfterWriteLock,
  },
  [VIO_COMPRESSED_WRITE_LOCK] = {
    .type          = VIO_COMPRESSED_WRITE_LOCK,
    .name          = "compressed write",
    .releaseReason = "failed compression",
    .waitOnLock    = waitOnPBNCompressedWriteLock,
    .transferLock  = getPBNReadLockAfterCompressedWriteLock,
  },
  [VIO_BLOCK_MAP_WRITE_LOCK] = {
    .type          = VIO_BLOCK_MAP_WRITE_LOCK,
    .name          = "block map write",
    .releaseReason = "block map write",
    .waitOnLock    = waitOnPBNBlockMapWriteLock,
    .transferLock  = NULL,
  },
};

/**********************************************************************/
static inline bool hasLockType(const PBNLock *lock, PBNLockType type)
{
  return (lock->implementation == &LOCK_IMPLEMENTATIONS[type]);
}

/**********************************************************************/
bool isPBNReadLock(const PBNLock *lock)
{
  return hasLockType(lock, VIO_READ_LOCK);
}

/**
 * Check whether a PBNLock is a compressed write lock.
 *
 * @param lock  The lock to check
 *
 * @return <code>true</code> if the lock is a compressed write lock
 **/
__attribute__((warn_unused_result))
static bool isPBNCompressedWriteLock(const PBNLock *lock)
{
  return hasLockType(lock, VIO_COMPRESSED_WRITE_LOCK);
}

/**********************************************************************/
void initializePBNLock(PBNLock *lock, PBNLockType type)
{
  lock->holder         = NULL;
  lock->implementation = &LOCK_IMPLEMENTATIONS[type];
  initializeWaitQueue(&lock->waiters);
}

/**
 * Get a read lock now that the lock holder has released its lock.
 *
 * @param waiter  The waiting DataVIO that will now get the lock
 * @param lock    The lock which is being released
 **/
static void retryPBNReadLock(DataVIO *waiter, PBNLock *lock)
{
  /*
   * When the lock is retried, the PBN this VIO and all the ones in the queue
   * behind it are seeking to lock may not be the same PBN as they were all
   * waiting on before, and can even be in a different zone, or currently
   * locked. We could notify and enqueue every waiter, but it's less expensive
   * to take the other waiters along with this VIO while it is being requeued
   * and transfer them to the new lock's waiter queue when this VIO obtains
   * the lock.
   */
  transferAllWaiters(&lock->waiters, &waiter->lockRetryWaiters);

  dataVIOAsCompletion(waiter)->requeue = true;
  launchDuplicateZoneCallback(waiter, acquirePBNReadLock,
                              THIS_LOCATION("$F;cb=acquirePBNRL"));
}

/**
 * A WaiterCallback to acquire a PBN read lock on a block which was locked
 * for a compressed write.
 *
 * @param waiter   The VIO waiting for the lock
 * @param context  The PBN lock that is being released
 **/
static void getPBNReadLockAfterCompressedWriteLock(Waiter *waiter,
                                                   void   *context)
{
  retryPBNReadLock(waiterAsDataVIO(waiter), (PBNLock *) context);
}

/**
 * A WaiterCallback to acquire a PBN read lock on a block which was write
 * locked.
 *
 * @param waiter   The VIO waiting for the lock
 * @param context  The PBN lock that is being released
 **/
static void getPBNReadLockAfterWriteLock(Waiter *waiter, void *context)
{
  DataVIO *dataVIO    = waiterAsDataVIO(waiter);
  PBNLock *lock       = (PBNLock *) context;
  DataVIO *lockHolder = lockHolderAsDataVIO(lock);
  if (dataVIO->duplicate.pbn != lockHolder->newMapped.pbn) {
    /*
     * The waiter is waiting for a lock on a different PBN than the one which
     * the lock holder ended up mapping to (either due to compression or
     * cancelled compression followed by subsequent dedupe). In either case,
     * the waiter should actually try to dedupe against the PBN which the
     * lock holder ended up using. Not doing so can result either in missing
     * a dedupe opportunity (if the lock holder got compressed), or data
     * corruption (in the subsequent dedupe case if the waiter is already
     * verified against the lock holder, VDO-2711).
     */
    setDuplicateLocation(dataVIO, lockHolder->newMapped);
  }

  retryPBNReadLock(dataVIO, lock);
}

/**
 * A WaiterCallback to acquire a PBN read lock on a block which was read
 * locked.
 *
 * @param waiter   The DataVIO waiting for the lock
 * @param context  The PBN lock that is being released
 **/
static void getPBNReadLockAfterReadLock(Waiter *waiter, void *context)
{
  DataVIO *dataVIO    = waiterAsDataVIO(waiter);
  PBNLock *lock       = (PBNLock *) context;
  DataVIO *lockHolder = lockHolderAsDataVIO(lock);
  if (lockHolder->rolledOver) {
    setDuplicateLocation(dataVIO, lockHolder->newMapped);
  } else if (dataVIO->duplicate.pbn != lockHolder->duplicate.pbn) {
    /*
     * If the waiter is waiting for a lock on a different PBN than the
     * one which the lockHolder is releasing, the lockHolder's dedupe
     * advice must have been either:
     *   1) converted from advice for an uncompressed block to advice
     *   for a compressed block; or
     *   2) rolled over from a previous DataVIO in the queue.
     * We need to pass that conversion on.
     */
    setDuplicateLocation(dataVIO, lockHolder->duplicate);
  }

  retryPBNReadLock(dataVIO, lock);
}

/**********************************************************************/
AllocatingVIO *lockHolderAsAllocatingVIO(const PBNLock *lock)
{
  ASSERT_LOG_ONLY(!isPBNReadLock(lock), "allocation lock is not a read lock");
  return lock->holder;
}

/**********************************************************************/
DataVIO *lockHolderAsDataVIO(const PBNLock *lock)
{
  ASSERT_LOG_ONLY(!isPBNCompressedWriteLock(lock),
                  "lockHolderAsDataVIO() called on DataVIO holder");
  return allocatingVIOAsDataVIO(lock->holder);
}

/**********************************************************************/
void waitOnPBNLock(DataVIO *dataVIO, PBNLock *lock)
{
  lock->implementation->waitOnLock(dataVIO, lock);
}

/**********************************************************************/
void notifyPBNLockWaiters(PBNLock *lock)
{
  notifyNextWaiter(&lock->waiters, lock->implementation->transferLock, lock);

  // Can't be done earlier since the waiters need to access the holder.
  lock->holder = NULL;
}

/**********************************************************************/
void assignProvisionalReference(PBNLock *lock)
{
  ASSERT_LOG_ONLY(!lock->hasProvisionalReference,
                  "lock does not have a provisional reference");
  lock->hasProvisionalReference = true;
}

/**********************************************************************/
void unassignProvisionalReference(PBNLock *lock)
{
  lock->hasProvisionalReference = false;
}

/**********************************************************************/
void releaseProvisionalReference(PBNLock             *lock,
                                 PhysicalBlockNumber  lockedPBN,
                                 BlockAllocator      *allocator)
{
  if (hasProvisionalReference(lock)) {
    releaseBlockReference(allocator, lockedPBN,
                          lock->implementation->releaseReason);
    unassignProvisionalReference(lock);
  }
}
