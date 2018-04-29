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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/base/hashLock.h#1 $
 */

#ifndef HASH_LOCK_H
#define HASH_LOCK_H

#include "types.h"

/**
 * Get the PBN lock on the duplicate data location for a DataVIO from the
 * HashLock the DataVIO holds (if there is one).
 *
 * @param dataVIO  The DataVIO to query
 *
 * @return The PBN lock on the DataVIO's duplicate location
 **/
PBNLock *getDuplicateLock(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Acquire or share a lock on the hash (chunk name) of the data in a DataVIO,
 * updating the DataVIO to reference the lock. This must only be called in the
 * correct thread for the zone. In the unlikely case of a hash collision, this
 * function will succeed, but the DataVIO will not get a lock reference.
 *
 * @param dataVIO  The DataVIO acquiring a lock on its chunk name
 **/
int acquireHashLock(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Asynchronously process a DataVIO that has just acquired its reference to a
 * hash lock. This may place the DataVIO on a wait queue, or it may use the
 * DataVIO to perform operations on the lock's behalf.
 *
 * @param dataVIO  The DataVIO that has just acquired a lock on its chunk name
 **/
void enterHashLock(DataVIO *dataVIO);

/**
 * Asynchronously continue processing a DataVIO in its hash lock after it has
 * finished writing, compressing, or deduplicating, so it can share the result
 * with any DataVIOs waiting in the hash lock, or update Albireo, or simply
 * release its share of the lock. This must only be called in the correct
 * thread for the hash zone.
 *
 * @param dataVIO  The DataVIO to continue processing in its hash lock
 **/
void continueHashLock(DataVIO *dataVIO);

/**
 * Re-enter the hash lock after encountering an error, to clean up the hash
 * lock.
 *
 * @param dataVIO  The DataVIO with an error
 **/
void continueHashLockOnError(DataVIO *dataVIO);

/**
 * Release a DataVIO's share of a hash lock, if held, and null out the
 * DataVIO's reference to it. This must only be called in the correct thread
 * for the hash zone.
 *
 * If the DataVIO is the only one holding the lock, this also releases any
 * resources or locks used by the hash lock (such as a PBN read lock on a
 * block containing data with the same hash) and returns the lock to the hash
 * zone's lock pool.
 *
 * @param dataVIO  The DataVIO releasing its hash lock
 **/
void releaseHashLock(DataVIO *dataVIO);

/**
 * Transfer a duplicate PBN read lock to a hash lock that has been waiting to
 * acquire it, then asynchronously continue processing the hash lock using the
 * provided DataVIO. The lock transfer may be rejected if the duplicate lock
 * would no longer be of use to the waiter.
 *
 * @param dataVIO        The DataVIO that was queued to wait on the PBN lock
 * @param duplicateLock  The duplicate PBN read lock that is being acquired
 *
 * @return <code>true</code> only if the transfer was accepted
 **/
bool inheritDuplicatePBNLock(DataVIO *dataVIO, PBNLock *duplicateLock)
  __attribute__((warn_unused_result));

/**
 * Downgrade a DataVIO's allocation lock from a PBN write lock to a read lock,
 * and transfer it to the DataVIO's hash lock, converting it to a duplicate
 * PBN lock.
 *
 * @param dataVIO  The DataVIO holding the allocation lock to transfer
 **/
void transferPBNWriteLock(DataVIO *dataVIO);

#endif // HASH_LOCK_H
