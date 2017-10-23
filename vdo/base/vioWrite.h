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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/vioWrite.h#1 $
 */

#ifndef VIO_WRITE_H
#define VIO_WRITE_H

#include "types.h"

/**
 * Wait for the holder of a read lock to release it.
 *
 * @param dataVIO  The DataVIO which wants to acquire a read lock
 * @param lock     The current lock
 **/
void waitOnPBNReadLock(DataVIO *dataVIO, PBNLock *lock);

/**
 * Attempt to wait for the holder of a write lock to release it. If we can
 * determine that the verification we want to do will fail once we get the
 * lock, we won't wait.
 *
 * @param dataVIO  The DataVIO which wants to acquire a read lock
 * @param lock     The current lock
 **/
void waitOnPBNWriteLock(DataVIO *dataVIO, PBNLock *lock);

/**
 * Attempt to wait for the holder of a compressed write lock to release it. If
 * we can determine that the verification we want to do will fail once we get
 * the lock, we won't wait.
 *
 * @param dataVIO  The DataVIO which wants to acquire a read lock
 * @param lock     The current lock
 **/
void waitOnPBNCompressedWriteLock(DataVIO *dataVIO, PBNLock *lock);

/**
 * Attempt to wait for the holder of a block map write lock to release it. In
 * actuality, we will just abort deduplication since deduplicating against
 * block map blocks is not allowed.
 *
 * @param dataVIO  The DataVIO which wants to acquire a read lock
 * @param lock     The current lock
 **/
void waitOnPBNBlockMapWriteLock(DataVIO *dataVIO,
                                PBNLock *lock __attribute__((unused)));

/**
 * Lock a DataVIO's duplicate.pbn physical block for reading and then do the
 * read/verify once the lock is acquired. This function is called from
 * verifyAdvice() and uses itself as the callback which is called when the lock
 * is acquired.
 *
 * <p>If we can determine based on the state of the current lock holder that
 * the verification will fail, we will neither acquire the lock nor wait to do
 * so, but send the DataVIO directly to the compression path.
 *
 * @param completion  The DataVIO attempting to acquire the physical block lock
 **/
void acquirePBNReadLock(VDOCompletion *completion);

/**
 * Release the PBN read lock if it is held. The duplicatePBN field will be
 * cleared as well.
 *
 * @param dataVIO  The possible lock holder
 **/
void releasePBNReadLock(DataVIO *dataVIO);

/**
 * Start the asynchronous processing of a DataVIO for a write request which has
 * acquired a lock on its logical block by joining the current flush generation
 * and then attempting to allocate a physical block.
 *
 * @param dataVIO  The DataVIO doing the write
 **/
void launchWriteDataVIO(DataVIO *dataVIO);

/**
 * Clean up a DataVIO which has finished processing a write.
 *
 * @param dataVIO  The DataVIO to clean up
 **/
void cleanupWriteDataVIO(DataVIO *dataVIO);

/**
 * Handle received duplication advice. This is a re-entry point to vioWrite
 * used by hash locks.
 *
 * @param completion The completion of the write in progress
 **/
void verifyAdvice(VDOCompletion *completion);

/**
 * Continue a write by attempting to compress the data. This is a re-entry
 * point to vioWrite used by hash locks.
 *
 * @param dataVIO   The DataVIO to be compressed
 **/
void compressData(DataVIO *dataVIO);

#endif /* VIO_WRITE_H */
