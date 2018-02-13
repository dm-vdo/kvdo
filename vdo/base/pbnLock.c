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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/pbnLock.c#2 $
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
  PBNLockType  type;
  const char  *name;
  const char  *releaseReason;
};

/**
 * This array must have an entry for every PBNLockType value.
 **/
static const PBNLockImplementation LOCK_IMPLEMENTATIONS[] = {
  [VIO_READ_LOCK] = {
    .type          = VIO_READ_LOCK,
    .name          = "read",
    .releaseReason = "candidate duplicate",
  },
  [VIO_WRITE_LOCK] = {
    .type          = VIO_WRITE_LOCK,
    .name          = "write",
    .releaseReason = "newly allocated",
  },
  [VIO_COMPRESSED_WRITE_LOCK] = {
    .type          = VIO_COMPRESSED_WRITE_LOCK,
    .name          = "compressed write",
    .releaseReason = "failed compression",
  },
  [VIO_BLOCK_MAP_WRITE_LOCK] = {
    .type          = VIO_BLOCK_MAP_WRITE_LOCK,
    .name          = "block map write",
    .releaseReason = "block map write",
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

/**********************************************************************/
static inline void setPBNLockType(PBNLock *lock, PBNLockType type)
{
  lock->implementation = &LOCK_IMPLEMENTATIONS[type];
}

/**********************************************************************/
void initializePBNLock(PBNLock *lock, PBNLockType type)
{
  lock->holder         = NULL;
  lock->hashLockHolder = NULL;
  setPBNLockType(lock, type);
  initializeWaitQueue(&lock->waiters);
}

/**********************************************************************/
AllocatingVIO *lockHolderAsAllocatingVIO(const PBNLock *lock)
{
  ASSERT_LOG_ONLY(!isPBNReadLock(lock), "allocation lock is not a read lock");
  return lock->holder;
}

/**********************************************************************/
void downgradePBNWriteLock(PBNLock *lock)
{
  ASSERT_LOG_ONLY(!isPBNReadLock(lock),
                  "PBN lock must not already have been downgraded");
  ASSERT_LOG_ONLY(!hasLockType(lock, VIO_BLOCK_MAP_WRITE_LOCK),
                  "must not downgrade block map write locks");
  ASSERT_LOG_ONLY(!hasWaiters(&lock->waiters),
                  "write locks must have no waiters");

  setPBNLockType(lock, VIO_READ_LOCK);
  lock->holder = NULL;
  lock->hashLockHolder = NULL;
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
