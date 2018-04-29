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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/base/pbnLock.h#1 $
 */

#ifndef PBN_LOCK_H
#define PBN_LOCK_H

#include "types.h"
#include "waitQueue.h"

/**
 * The type of a PBN lock.
 **/
typedef enum {
  VIO_READ_LOCK = 0,
  VIO_WRITE_LOCK,
  VIO_COMPRESSED_WRITE_LOCK,
  VIO_BLOCK_MAP_WRITE_LOCK,
} PBNLockType;

typedef struct pbnLockImplementation PBNLockImplementation;

/**
 * A PBN lock.
 **/
struct pbnLock {
  /** The VIO holding this lock; non-NULL when the PBN is locked by a VIO */
  AllocatingVIO *holder;

  /**
   * The HashLock holding this read lock; non-NULL when the PBN is locked by a
   * HashLock.
   **/
  HashLock *hashLockHolder;

  /** The queue of waiters for the lock */
  WaitQueue waiters;

  /** The implementation of the lock */
  const PBNLockImplementation *implementation;

  /**
   * Whether the locked PBN has been provisionally referenced on behalf of the
   * lock holder.
   **/
  bool hasProvisionalReference;

  /**
   * For read locks, the number of references that are guaranteed to still be
   * available on the locked block. Must be decremented for each DataVIO that
   * deduplicates against the block during the lifetime of the lock.
   **/
  uint8_t incrementLimit;
};

/**
 * Initialize a PBNLock.
 *
 * @param lock  The lock to initialize
 * @param type  The type of the lock
 **/
void initializePBNLock(PBNLock *lock, PBNLockType type);

/**
 * Get the holder of a PBNLock, converting it to a pointer to the associated
 * AllocatingVIO.
 *
 * @param lock  The lock to convert
 *
 * @return The lock holder as an AllocatingVIO
 **/
AllocatingVIO *lockHolderAsAllocatingVIO(const PBNLock *lock)
  __attribute__((warn_unused_result));

/**
 * Check whether a PBNLock instance is for a lock that is held.
 *
 * @param lock  The lock to query
 *
 * @return <code>true</code> if there is a lock object referenced and it
 *         is marked as being held by any VIO or HashLock
 **/
static inline bool isPBNLocked(const PBNLock *lock)
{
  return ((lock != NULL)
          && ((lock->holder != NULL) || (lock->hashLockHolder != NULL)));
}

/**
 * Check whether a PBNLock is a read lock.
 *
 * @param lock  The lock to check
 *
 * @return <code>true</code> if the lock is a read lock
 **/
bool isPBNReadLock(const PBNLock *lock)
  __attribute__((warn_unused_result));

/**
 * Notify the waiters for a PBN lock that the lock is being released, and
 * remove them from the lock's wait queue. The lock must have been removed
 * from the lock map already, but the lock's holder field must still be set.
 *
 * @param lock  The PBN lock that is being released
 **/
void notifyPBNLockWaiters(PBNLock *lock);

/**
 * Downgrade a PBN write lock to a PBN read lock. The write lock is expected
 * to have no waiters. The lock holder is cleared and the caller is
 * responsible for setting the new lock holder.
 *
 * @param lock  The PBN write lock to downgrade
 **/
void downgradePBNWriteLock(PBNLock *lock);

/**
 * Check whether a PBN lock has a provisional reference.
 *
 * @param lock  The PBN lock
 **/
static inline bool hasProvisionalReference(PBNLock *lock)
{
  return ((lock != NULL) && lock->hasProvisionalReference);
}

/**
 * Inform a PBN lock that it is responsible for a provisional reference.
 *
 * @param lock  The PBN lock
 **/
void assignProvisionalReference(PBNLock *lock);

/**
 * Inform a PBN lock that it is no longer responsible for a provisional
 * reference.
 *
 * @param lock  The PBN lock
 **/
void unassignProvisionalReference(PBNLock *lock);

/**
 * If the lock is responsible for a provisional reference, release that
 * reference. This method is called when the lock is released.
 *
 * @param lock       The lock
 * @param lockedPBN  The PBN covered by the lock
 * @param allocator  The block allocator from which to release the reference
 **/
void releaseProvisionalReference(PBNLock             *lock,
                                 PhysicalBlockNumber  lockedPBN,
                                 BlockAllocator      *allocator);

#endif /* PBN_LOCK_H */
