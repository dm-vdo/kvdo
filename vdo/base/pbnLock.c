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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/pbnLock.c#3 $
 */

#include "pbnLock.h"

#include "logger.h"

#include "blockAllocator.h"
#include "referenceBlock.h"

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
  lock->holderCount = 0;
  setPBNLockType(lock, type);
}

/**********************************************************************/
void downgradePBNWriteLock(PBNLock *lock)
{
  ASSERT_LOG_ONLY(!isPBNReadLock(lock),
                  "PBN lock must not already have been downgraded");
  ASSERT_LOG_ONLY(!hasLockType(lock, VIO_BLOCK_MAP_WRITE_LOCK),
                  "must not downgrade block map write locks");
  ASSERT_LOG_ONLY(lock->holderCount == 1,
                  "PBN write lock should have one holder but has %u",
                  lock->holderCount);
  if (hasLockType(lock, VIO_WRITE_LOCK)) {
    // DataVIO write locks are downgraded in place--the writer retains the
    // hold on the lock. They've already had a single incRef journaled.
    lock->incrementLimit = MAXIMUM_REFERENCE_COUNT - 1;
  } else {
    // Compressed block write locks are downgraded when they are shared with
    // all their hash locks. The writer is releasing its hold on the lock.
    lock->holderCount = 0;
    lock->incrementLimit = MAXIMUM_REFERENCE_COUNT;
  }
  setPBNLockType(lock, VIO_READ_LOCK);
}

/**********************************************************************/
bool claimPBNLockIncrement(PBNLock *lock)
{
  /*
   * Claim the next free reference atomically since hash locks from multiple
   * hash zone threads might be concurrently deduplicating against a single
   * PBN lock on compressed block. As long as hitting the increment limit will
   * lead to the PBN lock being released in a sane time-frame, we won't
   * overflow a 32-bit claim counter, allowing a simple add instead of a
   * compare-and-swap.
   */
  uint32_t claimNumber = atomicAdd32(&lock->incrementsClaimed, 1);
  return (claimNumber <= lock->incrementLimit);
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
