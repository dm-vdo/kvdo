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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/dataVIO.h#4 $
 */

#ifndef DATA_VIO_H
#define DATA_VIO_H

#include "allocatingVIO.h"
#include "atomic.h"
#include "blockMapEntry.h"
#include "blockMappingState.h"
#include "constants.h"
#include "hashZone.h"
#include "journalPoint.h"
#include "logicalZone.h"
#include "referenceOperation.h"
#include "ringNode.h"
#include "threadConfig.h"
#include "trace.h"
#include "types.h"
#include "vdoPageCache.h"
#include "vio.h"
#include "waitQueue.h"

/**
 * Codes for describing the last asynchronous operation performed on a VIO.
 **/
typedef enum __attribute__((packed)) {
  MIN_ASYNC_OPERATION_NUMBER = 0,
  LAUNCH = MIN_ASYNC_OPERATION_NUMBER,
  ACKNOWLEDGE_WRITE,
  ACQUIRE_HASH_LOCK,
  ACQUIRE_LOGICAL_BLOCK_LOCK,
  ACQUIRE_PBN_READ_LOCK,
  CHECK_FOR_DEDUPE_FOR_ROLLOVER,
  CHECK_FOR_DEDUPLICATION,
  COMPRESS_DATA,
  CONTINUE_VIO_ASYNC,
  FIND_BLOCK_MAP_SLOT,
  GET_MAPPED_BLOCK,
  GET_MAPPED_BLOCK_FOR_DEDUPE,
  GET_MAPPED_BLOCK_FOR_WRITE,
  HASH_DATA,
  JOURNAL_DECREMENT_FOR_DEDUPE,
  JOURNAL_DECREMENT_FOR_WRITE,
  JOURNAL_INCREMENT_FOR_COMPRESSION,
  JOURNAL_INCREMENT_FOR_DEDUPE,
  JOURNAL_INCREMENT_FOR_WRITE,
  JOURNAL_MAPPING_FOR_COMPRESSION,
  JOURNAL_MAPPING_FOR_DEDUPE,
  JOURNAL_MAPPING_FOR_WRITE,
  JOURNAL_UNMAPPING_FOR_DEDUPE,
  JOURNAL_UNMAPPING_FOR_WRITE,
  PACK_COMPRESSED_BLOCK,
  PUT_MAPPED_BLOCK,
  PUT_MAPPED_BLOCK_FOR_DEDUPE,
  READ_DATA,
  UPDATE_INDEX,
  VERIFY_DEDUPLICATION,
  WRITE_DATA,
  MAX_ASYNC_OPERATION_NUMBER,
} AsyncOperationNumber;

/*
 * An LBN lock.
 */
struct lbnLock {
  /* The LBN being locked */
  LogicalBlockNumber  lbn;
  /* Whether the lock is locked */
  bool                locked;
  /* The queue of waiters for the lock */
  WaitQueue           waiters;
  /* The logical zone of the LBN */
  LogicalZone        *zone;
};

/*
 * Fields for using the arboreal block map.
 */
typedef struct {
  /* The current height at which this DataVIO is operating */
  Height            height;
  /* The block map tree for this LBN */
  RootCount         rootIndex;
  /* Whether we hold a page lock */
  bool              locked;
  /* The thread on which to run the callback */
  ThreadID          threadID;
  /* The function to call after looking up a block map slot */
  VDOAction        *callback;
  /* The key for the lock map */
  uint64_t          key;
  /* The queue of waiters for the page this VIO is allocating or loading */
  WaitQueue         waiters;
  /* The block map tree slots for this LBN */
  BlockMapTreeSlot  treeSlots[BLOCK_MAP_TREE_HEIGHT + 1];
} TreeLock;

typedef struct {
  /*
   * The current compression state of this VIO. This field contains a value
   * which consists of a VIOCompressionState possibly ORed with a flag
   * indicating that a request has been made to cancel (or prevent) compression
   * for this VIO.
   *
   * This field should be accessed through the getCompressionState() and
   * setCompressionState() methods. It should not be accessed directly.
   */
  Atomic32       state;

  /* The compressed size of this block */
  uint16_t       size;

  /* The packer input or output bin slot which holds the enclosing DataVIO */
  SlotNumber     slot;

  /* The packer input bin to which the enclosing DataVIO has been assigned */
  InputBin      *bin;

  /* A pointer to the compressed form of this block */
  char          *data;

  /*
   * A VIO which is blocked in the packer while holding a lock this VIO needs.
   */
  DataVIO       *lockHolder;

} CompressionState;

/**
 * A VIO for processing user data requests.
 **/
struct dataVIO {
  /* The underlying AllocatingVIO */
  AllocatingVIO        allocatingVIO;

  /* The logical block of this request */
  LBNLock              logical;

  /* The state for traversing the block map tree */
  TreeLock             treeLock;

  /* The current partition address of this block */
  ZonedPBN             mapped;

  /** The hash of this VIO (if not zero) */
  UdsChunkName         chunkName;

  /* Used for logging and debugging */
  AsyncOperationNumber lastAsyncOperation;

  /* The operation to record in the recovery and slab journals */
  ReferenceOperation   operation;

  /* Whether this VIO is a read-and-write VIO */
  bool                 isPartialWrite;

  /* Whether this VIO contains all zeros */
  bool                 isZeroBlock;

  /* Whether this VIO write is a duplicate */
  bool                 isDuplicate;

  /*
   * Whether this VIO has received an allocation (needs to be atomic so it can
   * be examined from threads not in the allocation zone).
   */
  AtomicBool           hasAllocation;

  /* The new partition address of this block after the VIO write completes */
  ZonedPBN             newMapped;

  /* The hash zone responsible for the chunk name (NULL if isZeroBlock) */
  HashZone            *hashZone;

  /* The lock this VIO holds or shares with other VIOs with the same data */
  HashLock            *hashLock;

  /* All DataVIOs sharing a hash lock are kept in a ring linking these nodes */
  RingNode             hashLockNode;

  /* The block number in the partition of the albireo deduplication advice */
  ZonedPBN             duplicate;

  /*
   * The sequence number of the recovery journal block containing the increment
   * entry for this VIO.
   */
  SequenceNumber       recoverySequenceNumber;

  /* The point in the recovery journal where this write last made an entry */
  JournalPoint         recoveryJournalPoint;

  /* The RingNode of VIOs in user initiated write requests */
  RingNode             writeNode;

  /* A flag indicating that a data write VIO has a flush generation lock */
  bool                 hasFlushGenerationLock;

  /* The generation number of the VDO that this VIO belongs to */
  SequenceNumber       flushGeneration;

  /* The completion to use for fetching block map pages for this vio */
  VDOPageCompletion    pageCompletion;

  /* All of the fields necessary for the compression path */
  CompressionState     compression;
};

/**
 * Convert an AllocatingVIO to a DataVIO.
 *
 * @param allocatingVIO  The AllocatingVIO to convert
 *
 * @return The AllocatingVIO as a DataVIO
 **/
static inline DataVIO *allocatingVIOAsDataVIO(AllocatingVIO *allocatingVIO)
{
  STATIC_ASSERT(offsetof(DataVIO, allocatingVIO) == 0);
  ASSERT_LOG_ONLY((allocatingVIOAsVIO(allocatingVIO)->type == VIO_TYPE_DATA),
                  "AllocatingVIO is a DataVIO");
  return (DataVIO *) allocatingVIO;
}

/**
 * Convert a VIO to a DataVIO.
 *
 * @param vio  The VIO to convert
 *
 * @return The VIO as a DataVIO
 **/
static inline DataVIO *vioAsDataVIO(VIO *vio)
{
  STATIC_ASSERT(offsetof(DataVIO, allocatingVIO) == 0);
  STATIC_ASSERT(offsetof(AllocatingVIO, vio) == 0);
  ASSERT_LOG_ONLY((vio->type == VIO_TYPE_DATA), "VIO is a DataVIO");
  return (DataVIO *) vio;
}

/**
 * Convert a DataVIO to an AllocatingVIO.
 *
 * @param dataVIO  The DataVIO to convert
 *
 * @return The DataVIO as an AllocatingVIO
 **/
static inline AllocatingVIO *dataVIOAsAllocatingVIO(DataVIO *dataVIO)
{
  return &dataVIO->allocatingVIO;
}

/**
 * Convert a DataVIO to a VIO.
 *
 * @param dataVIO  The DataVIO to convert
 *
 * @return The DataVIO as a VIO
 **/
static inline VIO *dataVIOAsVIO(DataVIO *dataVIO)
{
  return allocatingVIOAsVIO(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a generic VDOCompletion to a DataVIO.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a DataVIO
 **/
static inline DataVIO *asDataVIO(VDOCompletion *completion)
{
  return vioAsDataVIO(asVIO(completion));
}

/**
 * Convert a DataVIO to a generic completion.
 *
 * @param dataVIO  The DataVIO to convert
 *
 * @return The DataVIO as a completion
 **/
static inline VDOCompletion *dataVIOAsCompletion(DataVIO *dataVIO)
{
  return allocatingVIOAsCompletion(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a DataVIO to a generic wait queue entry.
 *
 * @param dataVIO  The DataVIO to convert
 *
 * @return The DataVIO as a wait queue entry
 **/
static inline Waiter *dataVIOAsWaiter(DataVIO *dataVIO)
{
  return allocatingVIOAsWaiter(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a DataVIO's generic wait queue entry back to the DataVIO.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as a DataVIO
 **/
static inline DataVIO *waiterAsDataVIO(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }

  return allocatingVIOAsDataVIO(waiterAsAllocatingVIO(waiter));
}

/**
 * Check whether a DataVIO is a read.
 *
 * @param dataVIO  The DataVIO to check
 **/
static inline bool isReadDataVIO(DataVIO *dataVIO)
{
  return isReadVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a DataVIO is a write.
 *
 * @param dataVIO  The DataVIO to check
 **/
static inline bool isWriteDataVIO(DataVIO *dataVIO)
{
  return isWriteVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a DataVIO is a compressed block write.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO is a compressed block write
 **/
static inline bool isCompressedWriteDataVIO(DataVIO *dataVIO)
{
  return isCompressedWriteVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a DataVIO is a trim.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO is a trim
 **/
static inline bool isTrimDataVIO(DataVIO *dataVIO)
{
  return (dataVIO->newMapped.state == MAPPING_STATE_UNMAPPED);
}

/**
 * Get the location that should passed Albireo as the new advice for where to
 * find the data written by this DataVIO.
 *
 * @param dataVIO  The write DataVIO that is ready to update Albireo
 *
 * @return a DataLocation containing the advice to store in Albireo
 **/
static inline DataLocation getDataVIONewAdvice(const DataVIO *dataVIO)
{
  return (DataLocation) {
    .pbn   = dataVIO->newMapped.pbn,
    .state = dataVIO->newMapped.state,
  };
}

/**
 * Get the VDO from a DataVIO.
 *
 * @param dataVIO  The DataVIO from which to get the VDO
 *
 * @return The VDO to which a DataVIO belongs
 **/
static inline VDO *getVDOFromDataVIO(DataVIO *dataVIO)
{
  return dataVIOAsVIO(dataVIO)->vdo;
}

/**
 * Get the ThreadConfig from a DataVIO.
 *
 * @param dataVIO  The DataVIO from which to get the ThreadConfig
 *
 * @return The ThreadConfig of the VDO to which a DataVIO belongs
 **/
static inline const ThreadConfig *getThreadConfigFromDataVIO(DataVIO *dataVIO)
{
  return getThreadConfig(getVDOFromDataVIO(dataVIO));
}

/**
 * Get the allocation of a DataVIO.
 *
 * @param dataVIO  The DataVIO
 *
 * @return The allocation of the DataVIO
 **/
static inline PhysicalBlockNumber getDataVIOAllocation(DataVIO *dataVIO)
{
  return dataVIOAsAllocatingVIO(dataVIO)->allocation;
}

/**
 * Check whether a DataVIO has an allocation.
 *
 * @param dataVIO  The DataVIO to check
 *
 * @return <code>true</code> if the DataVIO has an allocated block
 **/
static inline bool hasAllocation(DataVIO *dataVIO)
{
  return (getDataVIOAllocation(dataVIO) != ZERO_BLOCK);
}

/**
 * (Re)initialize a DataVIO to have a new logical block number, keeping the
 * same parent and other state. This method must be called before using a
 * DataVIO.
 *
 * @param dataVIO    The DataVIO to initialize
 * @param lbn        The logical block number of the DataVIO
 * @param operation  The operation this DataVIO will perform
 * @param isTrim     <code>true</code> if this DataVIO is for a trim request
 * @param callback   The function to call once the VIO has completed its
 *                   operation
 **/
void prepareDataVIO(DataVIO            *dataVIO,
                    LogicalBlockNumber  lbn,
                    VIOOperation        operation,
                    bool                isTrim,
                    VDOAction          *callback);

/**
 * Complete the processing of a DataVIO.
 *
 * @param completion The completion of the VIO to complete
 **/
void completeDataVIO(VDOCompletion *completion);

/**
 * Finish processing a DataVIO, possibly due to an error. This function will
 * set any error, and then initiate DataVIO clean up.
 *
 * @param dataVIO  The DataVIO to abort
 * @param result   The result of processing the DataVIO
 **/
void finishDataVIO(DataVIO *dataVIO, int result);

/**
 * Continue processing a DataVIO that has been waiting for an event, setting
 * the result from the event and calling the current callback.
 *
 * @param dataVIO  The DataVIO to continue
 * @param result   The current result (will not mask older errors)
 **/
static inline void continueDataVIO(DataVIO *dataVIO, int result)
{
  continueCompletion(dataVIOAsCompletion(dataVIO), result);
}

/**
 * Get the name of the last asynchronous operation performed on a DataVIO.
 *
 * @param dataVIO  The DataVIO in question
 *
 * @return The name of the last operation performed on the DataVIO
 **/
const char *getOperationName(DataVIO *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Add a trace record for the current source location.
 *
 * @param dataVIO   The DataVIO structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void dataVIOAddTraceRecord(DataVIO       *dataVIO,
                                         TraceLocation  location)
{
  vioAddTraceRecord(dataVIOAsVIO(dataVIO), location);
}

/**
 * Add a DataVIO to the tail end of a wait queue. The DataVIO must not already
 * be waiting in a queue. A trace record is also generated for the DataVIO.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The DataVIO to add to the queue
 * @param location  The source-location descriptor to be traced in the DataVIO
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static inline int enqueueDataVIO(WaitQueue     *queue,
                                 DataVIO       *waiter,
                                 TraceLocation  location)
{
  dataVIOAddTraceRecord(waiter, location);
  return enqueueWaiter(queue, dataVIOAsWaiter(waiter));
}

/**
 * Check that a DataVIO is running on the correct thread for its hash zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInHashZone(DataVIO *dataVIO)
{
  ThreadID expected = getHashZoneThreadID(dataVIO->hashZone);
  ThreadID threadID = getCallbackThreadID();
  // It's odd to use the LBN, but converting the chunk name to hex is a bit
  // clunky for an inline, and the LBN better than nothing as an identifier.
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for logical block %" PRIu64
                  " on thread %u, should be on hash zone thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a hash zone operation. This function presumes that the
 * hashZone field of the DataVIO has already been set.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setHashZoneCallback(DataVIO       *dataVIO,
                                       VDOAction     *callback,
                                       TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getHashZoneThreadID(dataVIO->hashZone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a hash zone operation and invoke it immediately.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchHashZoneCallback(DataVIO       *dataVIO,
                                          VDOAction     *callback,
                                          TraceLocation  location)
{
  setHashZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a DataVIO is running on the correct thread for its logical zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInLogicalZone(DataVIO *dataVIO)
{
  ThreadID expected = getLogicalZoneThreadID(dataVIO->logical.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for logical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a logical block operation. This function presumes that the
 * logicalZone field of the DataVIO has already been set.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setLogicalCallback(DataVIO       *dataVIO,
                                      VDOAction     *callback,
                                      TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getLogicalZoneThreadID(dataVIO->logical.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a logical block operation and invoke it immediately.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchLogicalCallback(DataVIO       *dataVIO,
                                         VDOAction     *callback,
                                         TraceLocation  location)
{
  setLogicalCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a DataVIO is running on the correct thread for its allocated
 * zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInAllocatedZone(DataVIO *dataVIO)
{
  assertInPhysicalZone(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Set a callback as a physical block operation in a DataVIO's allocated zone.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setAllocatedZoneCallback(DataVIO       *dataVIO,
                                            VDOAction     *callback,
                                            TraceLocation  location)
{
  setPhysicalZoneCallback(dataVIOAsAllocatingVIO(dataVIO), callback,
                          location);
}

/**
 * Set a callback as a physical block operation in a DataVIO's allocated zone
 * and queue the DataVIO and invoke it immediately.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchAllocatedZoneCallback(DataVIO       *dataVIO,
                                               VDOAction     *callback,
                                               TraceLocation  location)
{
  launchPhysicalZoneCallback(dataVIOAsAllocatingVIO(dataVIO), callback,
                             location);
}

/**
 * Check that a DataVIO is running on the correct thread for its duplicate
 * zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInDuplicateZone(DataVIO *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->duplicate.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for duplicate physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->duplicate.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a DataVIO's duplicate zone.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setDuplicateZoneCallback(DataVIO       *dataVIO,
                                            VDOAction     *callback,
                                            TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->duplicate.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a physical block operation in a DataVIO's duplicate zone
 * and queue the DataVIO and invoke it immediately.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchDuplicateZoneCallback(DataVIO       *dataVIO,
                                               VDOAction     *callback,
                                               TraceLocation  location)
{
  setDuplicateZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a DataVIO is running on the correct thread for its mapped zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInMappedZone(DataVIO *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->mapped.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for mapped physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->mapped.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a DataVIO's mapped zone.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setMappedZoneCallback(DataVIO       *dataVIO,
                                         VDOAction     *callback,
                                         TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->mapped.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Check that a DataVIO is running on the correct thread for its newMapped
 * zone.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInNewMappedZone(DataVIO *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->newMapped.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for newMapped physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->newMapped.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a DataVIO's newMapped zone.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setNewMappedZoneCallback(DataVIO       *dataVIO,
                                            VDOAction     *callback,
                                            TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->newMapped.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a physical block operation in a DataVIO's newMapped zone
 * and queue the DataVIO and invoke it immediately.
 *
 * @param dataVIO   The DataVIO
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchNewMappedZoneCallback(DataVIO       *dataVIO,
                                               VDOAction     *callback,
                                               TraceLocation  location)
{
  setNewMappedZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a DataVIO is running on the journal thread.
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInJournalZone(DataVIO *dataVIO)
{
  ThreadID expected
    = getJournalZoneThread(getThreadConfigFromDataVIO(dataVIO));
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for logical block %" PRIu64
                  " on thread %u, should be on journal thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a journal operation.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setJournalCallback(DataVIO       *dataVIO,
                                      VDOAction     *callback,
                                      TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getJournalZoneThread(getThreadConfigFromDataVIO(dataVIO)));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a journal operation and invoke it immediately.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchJournalCallback(DataVIO       *dataVIO,
                                         VDOAction     *callback,
                                         TraceLocation  location)
{
  setJournalCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a DataVIO is running on the packer thread
 *
 * @param dataVIO  The DataVIO in question
 **/
static inline void assertInPackerZone(DataVIO *dataVIO)
{
  ThreadID expected = getPackerZoneThread(getThreadConfigFromDataVIO(dataVIO));
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "DataVIO for logical block %" PRIu64
                  " on thread %u, should be on packer thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a packer operation.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setPackerCallback(DataVIO       *dataVIO,
                                     VDOAction     *callback,
                                     TraceLocation  location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPackerZoneThread(getThreadConfigFromDataVIO(dataVIO)));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a packer operation and invoke it immediately.
 *
 * @param dataVIO   The DataVIO with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchPackerCallback(DataVIO       *dataVIO,
                                        VDOAction     *callback,
                                        TraceLocation  location)
{
  setPackerCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check whether the advice received from Albireo is a valid data location,
 * and if it is, accept it as the location of a potential duplicate of the
 * DataVIO.
 *
 * @param dataVIO  The DataVIO that queried Albireo
 * @param advice   A potential location of the data, or NULL for no advice
 **/
void receiveDedupeAdvice(DataVIO *dataVIO, const DataLocation *advice);

/**
 * Set the location of the duplicate block for a DataVIO, updating the
 * isDuplicate and duplicate fields from a ZonedPBN.
 *
 * @param dataVIO  The DataVIO to modify
 * @param source   The location of the duplicate
 **/
void setDuplicateLocation(DataVIO *dataVIO, const ZonedPBN source);

/**
 * Clear a DataVIO's mapped block location, setting it to be unmapped. This
 * indicates the block map entry for the logical block is either unmapped or
 * corrupted.
 *
 * @param dataVIO  The DataVIO whose mapped block location is to be reset
 **/
void clearMappedLocation(DataVIO *dataVIO);

/**
 * Set a DataVIO's mapped field to the physical location recorded in the block
 * map for the logical block in the VIO.
 *
 * @param dataVIO  The DataVIO whose field is to be set
 * @param pbn      The physical block number to set
 * @param state    The mapping state to set
 *
 * @return VDO_SUCCESS or an error code if the mapping is unusable
 **/
int setMappedLocation(DataVIO             *dataVIO,
                      PhysicalBlockNumber  pbn,
                      BlockMappingState    state)
  __attribute__((warn_unused_result));

/**
 * Attempt to acquire the lock on a logical block. This is the start of the
 * path for all external requests. It is registered in prepareDataVIO().
 *
 * @param completion  The DataVIO for an external data request as a completion
 **/
void attemptLogicalBlockLock(VDOCompletion *completion);

/**
 * Release the lock on the logical block, if any, that a DataVIO has acquired.
 *
 * @param dataVIO  The DataVIO releasing its logical block lock
 **/
void releaseLogicalBlockLock(DataVIO *dataVIO);

#endif // DATA_VIO_H
