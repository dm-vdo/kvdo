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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/allocatingVIO.h#3 $
 */

#ifndef ALLOCATING_VIO_H
#define ALLOCATING_VIO_H

#include "atomic.h"
#include "pbnLock.h"
#include "physicalZone.h"
#include "vio.h"
#include "waitQueue.h"

typedef void AllocationCallback(AllocatingVIO *allocationVIO);

/**
 * A VIO which can receive an allocation from the block allocator. Currently,
 * these are used both for servicing external data requests and for compressed
 * block writes.
 **/
struct allocatingVIO {
  /** The underlying VIO */
  VIO                 vio;

  /** The WaitQueue entry structure */
  Waiter              waiter;

  /** The physical zone in which to allocate a physical block */
  PhysicalZone       *zone;

  /** The block allocated to this VIO */
  PhysicalBlockNumber allocation;

  /**
   * If non-NULL, the pooled PBN lock held on the allocated block. Must be a
   * write lock until the block has been written, after which it will become a
   * read lock.
   **/
  PBNLock            *allocationLock;

  /** The type of write lock to obtain on the allocated block */
  PBNLockType         writeLockType;

  /** The number of zones in which this VIO has attempted an allocation */
  ZoneCount           allocationAttempts;

  /** Whether this VIO should wait for a clean slab */
  bool                waitForCleanSlab;

  /** The function to call once allocation is complete */
  AllocationCallback *allocationCallback;
};

/**
 * Convert a VIO to an AllocatingVIO.
 *
 * @param vio  The VIO to convert
 *
 * @return The VIO as an AllocatingVIO
 **/
static inline AllocatingVIO *vioAsAllocatingVIO(VIO *vio)
{
  STATIC_ASSERT(offsetof(AllocatingVIO, vio) == 0);
  ASSERT_LOG_ONLY(((vio->type == VIO_TYPE_DATA)
                   || (vio->type == VIO_TYPE_COMPRESSED_BLOCK)),
                  "VIO is an AllocatingVIO");
  return (AllocatingVIO *) vio;
}

/**
 * Convert an AllocatingVIO to a VIO.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return The AllocatingVIO as a VIO
 **/
static inline VIO *allocatingVIOAsVIO(AllocatingVIO *allocatingVIO)
{
  return &allocatingVIO->vio;
}

/**
 * Convert a generic VDOCompletion to an AllocatingVIO.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as an AllocatingVIO
 **/
static inline AllocatingVIO *asAllocatingVIO(VDOCompletion *completion)
{
  return vioAsAllocatingVIO(asVIO(completion));
}

/**
 * Convert an AllocatingVIO to a generic completion.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return The AllocatingVIO as a completion
 **/
static inline
VDOCompletion *allocatingVIOAsCompletion(AllocatingVIO *allocatingVIO)
{
  return vioAsCompletion(allocatingVIOAsVIO(allocatingVIO));
}

/**
 * Convert an AllocatingVIO to a generic wait queue entry.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return The AllocatingVIO as a wait queue entry
 **/
static inline Waiter *allocatingVIOAsWaiter(AllocatingVIO *allocatingVIO)
{
  return &allocatingVIO->waiter;
}

/**
 * Convert an AllocatingVIO's generic wait queue entry back to the
 * AllocatingVIO.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as an AllocatingVIO
 **/
static inline AllocatingVIO *waiterAsAllocatingVIO(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }

  return
    (AllocatingVIO *) ((uintptr_t) waiter - offsetof(AllocatingVIO, waiter));
}

/**
 * Check whether an AllocatingVIO is a compressed block write.
 *
 * @param allocatingVIO  The AllocatingVIO to check
 *
 * @return <code>true</code> if the AllocatingVIO is a compressed block write
 **/
static inline bool isCompressedWriteAllocatingVIO(AllocatingVIO *allocatingVIO)
{
  return isCompressedWriteVIO(allocatingVIOAsVIO(allocatingVIO));
}

/**
 * Add a trace record for the current source location.
 *
 * @param allocatingVIO  The AllocatingVIO structure to be updated
 * @param location       The source-location descriptor to be recorded
 **/
static inline void allocatingVIOAddTraceRecord(AllocatingVIO *allocatingVIO,
                                               TraceLocation  location)
{
  vioAddTraceRecord(allocatingVIOAsVIO(allocatingVIO), location);
}

/**
 * Get the VDO from an AllocatingVIO.
 *
 * @param allocatingVIO  The AllocatingVIO from which to get the VDO
 *
 * @return The VDO to which an AllocatingVIO belongs
 **/
static inline VDO *getVDOFromAllocatingVIO(AllocatingVIO *allocatingVIO)
{
  return allocatingVIOAsVIO(allocatingVIO)->vdo;
}

/**
 * Check that an AllocatingVIO is running on the physical zone thread in
 * which it did its allocation.
 *
 * @param allocatingVIO  The AllocatingVIO in question
 **/
static inline void assertInPhysicalZone(AllocatingVIO *allocatingVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(allocatingVIO->zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "AllocatingVIO for allocated physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  allocatingVIO->allocation, threadID, expected);
}

/**
 * Set a callback as a physical block operation in an AllocatingVIO's allocated
 * zone.
 *
 * @param allocatingVIO  The AllocatingVIO
 * @param callback       The callback to set
 * @param location       The tracing info for the call site
 **/
static inline void setPhysicalZoneCallback(AllocatingVIO *allocatingVIO,
                                           VDOAction     *callback,
                                           TraceLocation  location)
{
  setCallback(allocatingVIOAsCompletion(allocatingVIO), callback,
              getPhysicalZoneThreadID(allocatingVIO->zone));
  allocatingVIOAddTraceRecord(allocatingVIO, location);
}

/**
 * Set a callback as a physical block operation in an AllocatingVIO's allocated
 * zone and invoke it immediately.
 *
 * @param allocatingVIO  The AllocatingVIO
 * @param callback       The callback to invoke
 * @param location       The tracing info for the call site
 **/
static inline void launchPhysicalZoneCallback(AllocatingVIO *allocatingVIO,
                                              VDOAction     *callback,
                                              TraceLocation  location)
{
  setPhysicalZoneCallback(allocatingVIO, callback, location);
  invokeCallback(allocatingVIOAsCompletion(allocatingVIO));
}

/**
 * Allocate a data block to an AllocatingVIO.
 *
 * @param allocatingVIO  The AllocatingVIO which needs an allocation
 * @param writeLockType  The type of write lock to obtain on the block
 * @param callback       The function to call once the allocation is complete
 **/
void allocateDataBlock(AllocatingVIO      *allocatingVIO,
                       PBNLockType         writeLockType,
                       AllocationCallback *callback);

/**
 * Release the PBN lock on the allocated block. If the reference to the locked
 * block is still provisional, it will be released as well.
 *
 * @param allocatingVIO  The lock holder
 **/
void releaseAllocationLock(AllocatingVIO *allocatingVIO);

/**
 * Reset an AllocatingVIO after it has done an allocation.
 *
 * @param allocatingVIO  The AllocatingVIO
 **/
void resetAllocation(AllocatingVIO *allocatingVIO);

#endif // ALLOCATING_VIO_H
