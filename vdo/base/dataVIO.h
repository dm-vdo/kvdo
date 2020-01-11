/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dataVIO.h#18 $
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
 * Codes for describing the last asynchronous operation performed on a vio.
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
struct lbn_lock {
  /* The LBN being locked */
  LogicalBlockNumber   lbn;
  /* Whether the lock is locked */
  bool                 locked;
  /* The queue of waiters for the lock */
  struct wait_queue    waiters;
  /* The logical zone of the LBN */
  struct logical_zone *zone;
};

/*
 * Fields for using the arboreal block map.
 */
struct tree_lock {
  /* The current height at which this data_vio is operating */
  Height                      height;
  /* The block map tree for this LBN */
  RootCount                   rootIndex;
  /* Whether we hold a page lock */
  bool                        locked;
  /* The thread on which to run the callback */
  ThreadID                    threadID;
  /* The function to call after looking up a block map slot */
  VDOAction                  *callback;
  /* The key for the lock map */
  uint64_t                    key;
  /* The queue of waiters for the page this vio is allocating or loading */
  struct wait_queue           waiters;
  /* The block map tree slots for this LBN */
  struct block_map_tree_slot  treeSlots[BLOCK_MAP_TREE_HEIGHT + 1];
};

struct compression_state {
  /*
   * The current compression state of this vio. This field contains a value
   * which consists of a VIOCompressionState possibly ORed with a flag
   * indicating that a request has been made to cancel (or prevent) compression
   * for this vio.
   *
   * This field should be accessed through the getCompressionState() and
   * setCompressionState() methods. It should not be accessed directly.
   */
  Atomic32          state;

  /* The compressed size of this block */
  uint16_t          size;

  /* The packer input or output bin slot which holds the enclosing data_vio */
  SlotNumber        slot;

  /* The packer input bin to which the enclosing data_vio has been assigned */
  struct input_bin *bin;

  /* A pointer to the compressed form of this block */
  char             *data;

  /*
   * A vio which is blocked in the packer while holding a lock this vio needs.
   */
  struct data_vio  *lockHolder;

};

/**
 * A vio for processing user data requests.
 **/
struct data_vio {
  /* The underlying struct allocating_vio */
  struct allocating_vio       allocatingVIO;

  /* The logical block of this request */
  struct lbn_lock             logical;

  /* The state for traversing the block map tree */
  struct tree_lock            treeLock;

  /* The current partition address of this block */
  struct zoned_pbn            mapped;

  /** The hash of this vio (if not zero) */
  UdsChunkName                chunkName;

  /* Used for logging and debugging */
  AsyncOperationNumber        lastAsyncOperation;

  /* The operation to record in the recovery and slab journals */
  struct reference_operation  operation;

  /* Whether this vio is a read-and-write vio */
  bool                        isPartialWrite;

  /* Whether this vio contains all zeros */
  bool                        isZeroBlock;

  /* Whether this vio write is a duplicate */
  bool                        isDuplicate;

  /*
   * Whether this vio has received an allocation (needs to be atomic so it can
   * be examined from threads not in the allocation zone).
   */
  AtomicBool                  hasAllocation;

  /* The new partition address of this block after the vio write completes */
  struct zoned_pbn            newMapped;

  /* The hash zone responsible for the chunk name (NULL if isZeroBlock) */
  struct hash_zone           *hashZone;

  /* The lock this vio holds or shares with other vios with the same data */
  struct hash_lock           *hashLock;

  /* All DataVIOs sharing a hash lock are kept in a ring linking these nodes */
  RingNode                    hashLockNode;

  /* The block number in the partition of the albireo deduplication advice */
  struct zoned_pbn            duplicate;

  /*
   * The sequence number of the recovery journal block containing the increment
   * entry for this vio.
   */
  SequenceNumber              recoverySequenceNumber;

  /* The point in the recovery journal where this write last made an entry */
  struct journal_point        recoveryJournalPoint;

  /* The RingNode of vios in user initiated write requests */
  RingNode                    writeNode;

  /* A flag indicating that a data write vio has a flush generation lock */
  bool                        hasFlushGenerationLock;

  /* The generation number of the VDO that this vio belongs to */
  SequenceNumber              flushGeneration;

  /* The completion to use for fetching block map pages for this vio */
  struct vdo_page_completion  pageCompletion;

  /* All of the fields necessary for the compression path */
  struct compression_state    compression;
};

/**
 * Convert an allocating_vio to a data_vio.
 *
 * @param allocatingVIO  The allocating_vio to convert
 *
 * @return The allocating_vio as a data_vio
 **/
static inline struct data_vio *
allocatingVIOAsDataVIO(struct allocating_vio *allocatingVIO)
{
  STATIC_ASSERT(offsetof(struct data_vio, allocatingVIO) == 0);
  ASSERT_LOG_ONLY((allocating_vio_as_vio(allocatingVIO)->type == VIO_TYPE_DATA),
                  "allocating_vio is a struct data_vio");
  return (struct data_vio *) allocatingVIO;
}

/**
 * Convert a vio to a data_vio.
 *
 * @param vio  The vio to convert
 *
 * @return The vio as a data_vio
 **/
static inline struct data_vio *vioAsDataVIO(struct vio *vio)
{
  STATIC_ASSERT(offsetof(struct data_vio, allocatingVIO) == 0);
  STATIC_ASSERT(offsetof(struct allocating_vio, vio) == 0);
  ASSERT_LOG_ONLY((vio->type == VIO_TYPE_DATA), "vio is a data_vio");
  return (struct data_vio *) vio;
}

/**
 * Convert a data_vio to an allocating_vio.
 *
 * @param dataVIO  The data_vio to convert
 *
 * @return The data_vio as an allocating_vio
 **/
static inline struct allocating_vio *dataVIOAsAllocatingVIO(struct data_vio *dataVIO)
{
  return &dataVIO->allocatingVIO;
}

/**
 * Convert a data_vio to a vio.
 *
 * @param dataVIO  The data_vio to convert
 *
 * @return The data_vio as a vio
 **/
static inline struct vio *dataVIOAsVIO(struct data_vio *dataVIO)
{
  return allocating_vio_as_vio(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a generic vdo_completion to a data_vio.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a data_vio
 **/
static inline struct data_vio *asDataVIO(struct vdo_completion *completion)
{
  return vioAsDataVIO(asVIO(completion));
}

/**
 * Convert a data_vio to a generic completion.
 *
 * @param dataVIO  The data_vio to convert
 *
 * @return The data_vio as a completion
 **/
static inline struct vdo_completion *
dataVIOAsCompletion(struct data_vio *dataVIO)
{
  return allocating_vio_as_completion(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a data_vio to a generic wait queue entry.
 *
 * @param dataVIO  The data_vio to convert
 *
 * @return The data_vio as a wait queue entry
 **/
static inline struct waiter *dataVIOAsWaiter(struct data_vio *dataVIO)
{
  return allocating_vio_as_waiter(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Convert a data_vio's generic wait queue entry back to the data_vio.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as a data_vio
 **/
static inline struct data_vio *waiterAsDataVIO(struct waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }

  return allocatingVIOAsDataVIO(waiter_as_allocating_vio(waiter));
}

/**
 * Check whether a data_vio is a read.
 *
 * @param dataVIO  The data_vio to check
 **/
static inline bool isReadDataVIO(struct data_vio *dataVIO)
{
  return isReadVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a data_vio is a write.
 *
 * @param dataVIO  The data_vio to check
 **/
static inline bool isWriteDataVIO(struct data_vio *dataVIO)
{
  return isWriteVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a data_vio is a compressed block write.
 *
 * @param dataVIO  The data_vio to check
 *
 * @return <code>true</code> if the data_vio is a compressed block write
 **/
static inline bool isCompressedWriteDataVIO(struct data_vio *dataVIO)
{
  return isCompressedWriteVIO(dataVIOAsVIO(dataVIO));
}

/**
 * Check whether a data_vio is a trim.
 *
 * @param dataVIO  The data_vio to check
 *
 * @return <code>true</code> if the data_vio is a trim
 **/
static inline bool isTrimDataVIO(struct data_vio *dataVIO)
{
  return (dataVIO->newMapped.state == MAPPING_STATE_UNMAPPED);
}

/**
 * Get the location that should passed Albireo as the new advice for where to
 * find the data written by this data_vio.
 *
 * @param dataVIO  The write data_vio that is ready to update Albireo
 *
 * @return a data_location containing the advice to store in Albireo
 **/
static inline struct data_location
getDataVIONewAdvice(const struct data_vio *dataVIO)
{
  return (struct data_location) {
    .pbn   = dataVIO->newMapped.pbn,
    .state = dataVIO->newMapped.state,
  };
}

/**
 * Get the vdo from a data_vio.
 *
 * @param dataVIO  The data_vio from which to get the vdo
 *
 * @return The vdo to which a data_vio belongs
 **/
static inline struct vdo *getVDOFromDataVIO(struct data_vio *dataVIO)
{
  return dataVIOAsVIO(dataVIO)->vdo;
}

/**
 * Get the ThreadConfig from a data_vio.
 *
 * @param dataVIO  The data_vio from which to get the ThreadConfig
 *
 * @return The ThreadConfig of the vdo to which a data_vio belongs
 **/
static inline const ThreadConfig *
getThreadConfigFromDataVIO(struct data_vio *dataVIO)
{
  return getThreadConfig(getVDOFromDataVIO(dataVIO));
}

/**
 * Get the allocation of a data_vio.
 *
 * @param dataVIO  The data_vio
 *
 * @return The allocation of the data_vio
 **/
static inline PhysicalBlockNumber getDataVIOAllocation(struct data_vio *dataVIO)
{
  return dataVIOAsAllocatingVIO(dataVIO)->allocation;
}

/**
 * Check whether a data_vio has an allocation.
 *
 * @param dataVIO  The data_vio to check
 *
 * @return <code>true</code> if the data_vio has an allocated block
 **/
static inline bool hasAllocation(struct data_vio *dataVIO)
{
  return (getDataVIOAllocation(dataVIO) != ZERO_BLOCK);
}

/**
 * (Re)initialize a data_vio to have a new logical block number, keeping the
 * same parent and other state. This method must be called before using a
 * data_vio.
 *
 * @param dataVIO    The data_vio to initialize
 * @param lbn        The logical block number of the data_vio
 * @param operation  The operation this data_vio will perform
 * @param isTrim     <code>true</code> if this data_vio is for a trim request
 * @param callback   The function to call once the vio has completed its
 *                   operation
 **/
void prepareDataVIO(struct data_vio    *dataVIO,
                    LogicalBlockNumber  lbn,
                    VIOOperation        operation,
                    bool                isTrim,
                    VDOAction          *callback);

/**
 * Complete the processing of a data_vio.
 *
 * @param completion The completion of the vio to complete
 **/
void completeDataVIO(struct vdo_completion *completion);

/**
 * Finish processing a data_vio, possibly due to an error. This function will
 * set any error, and then initiate data_vio clean up.
 *
 * @param dataVIO  The data_vio to abort
 * @param result   The result of processing the data_vio
 **/
void finishDataVIO(struct data_vio *dataVIO, int result);

/**
 * Continue processing a data_vio that has been waiting for an event, setting
 * the result from the event and calling the current callback.
 *
 * @param dataVIO  The data_vio to continue
 * @param result   The current result (will not mask older errors)
 **/
static inline void continueDataVIO(struct data_vio *dataVIO, int result)
{
  continueCompletion(dataVIOAsCompletion(dataVIO), result);
}

/**
 * Get the name of the last asynchronous operation performed on a data_vio.
 *
 * @param dataVIO  The data_vio in question
 *
 * @return The name of the last operation performed on the data_vio
 **/
const char *getOperationName(struct data_vio *dataVIO)
  __attribute__((warn_unused_result));

/**
 * Add a trace record for the current source location.
 *
 * @param dataVIO   The data_vio structure to be updated
 * @param location  The source-location descriptor to be recorded
 **/
static inline void dataVIOAddTraceRecord(struct data_vio *dataVIO,
                                         TraceLocation    location)
{
  vioAddTraceRecord(dataVIOAsVIO(dataVIO), location);
}

/**
 * Add a data_vio to the tail end of a wait queue. The data_vio must not already
 * be waiting in a queue. A trace record is also generated for the data_vio.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The data_vio to add to the queue
 * @param location  The source-location descriptor to be traced in the data_vio
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static inline int enqueueDataVIO(struct wait_queue *queue,
                                 struct data_vio   *waiter,
                                 TraceLocation      location)
{
  dataVIOAddTraceRecord(waiter, location);
  return enqueueWaiter(queue, dataVIOAsWaiter(waiter));
}

/**
 * Check that a data_vio is running on the correct thread for its hash zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInHashZone(struct data_vio *dataVIO)
{
  ThreadID expected = getHashZoneThreadID(dataVIO->hashZone);
  ThreadID threadID = getCallbackThreadID();
  // It's odd to use the LBN, but converting the chunk name to hex is a bit
  // clunky for an inline, and the LBN better than nothing as an identifier.
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for logical block %" PRIu64
                  " on thread %u, should be on hash zone thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a hash zone operation. This function presumes that the
 * hashZone field of the data_vio has already been set.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setHashZoneCallback(struct data_vio *dataVIO,
                                       VDOAction       *callback,
                                       TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getHashZoneThreadID(dataVIO->hashZone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a hash zone operation and invoke it immediately.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchHashZoneCallback(struct data_vio *dataVIO,
                                          VDOAction       *callback,
                                          TraceLocation    location)
{
  setHashZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a data_vio is running on the correct thread for its logical zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInLogicalZone(struct data_vio *dataVIO)
{
  ThreadID expected = getLogicalZoneThreadID(dataVIO->logical.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for logical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a logical block operation. This function presumes that the
 * logicalZone field of the data_vio has already been set.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setLogicalCallback(struct data_vio *dataVIO,
                                      VDOAction       *callback,
                                      TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getLogicalZoneThreadID(dataVIO->logical.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a logical block operation and invoke it immediately.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchLogicalCallback(struct data_vio *dataVIO,
                                         VDOAction       *callback,
                                         TraceLocation    location)
{
  setLogicalCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a data_vio is running on the correct thread for its allocated
 * zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInAllocatedZone(struct data_vio *dataVIO)
{
  assertInPhysicalZone(dataVIOAsAllocatingVIO(dataVIO));
}

/**
 * Set a callback as a physical block operation in a data_vio's allocated zone.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setAllocatedZoneCallback(struct data_vio *dataVIO,
                                            VDOAction       *callback,
                                            TraceLocation    location)
{
  set_physical_zone_callback(dataVIOAsAllocatingVIO(dataVIO), callback,
                             location);
}

/**
 * Set a callback as a physical block operation in a data_vio's allocated zone
 * and queue the data_vio and invoke it immediately.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchAllocatedZoneCallback(struct data_vio *dataVIO,
                                               VDOAction       *callback,
                                               TraceLocation    location)
{
  launch_physical_zone_callback(dataVIOAsAllocatingVIO(dataVIO), callback,
                                location);
}

/**
 * Check that a data_vio is running on the correct thread for its duplicate
 * zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInDuplicateZone(struct data_vio *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->duplicate.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for duplicate physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->duplicate.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's duplicate zone.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setDuplicateZoneCallback(struct data_vio *dataVIO,
                                            VDOAction       *callback,
                                            TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->duplicate.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a physical block operation in a data_vio's duplicate zone
 * and queue the data_vio and invoke it immediately.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchDuplicateZoneCallback(struct data_vio *dataVIO,
                                               VDOAction       *callback,
                                               TraceLocation    location)
{
  setDuplicateZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a data_vio is running on the correct thread for its mapped zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInMappedZone(struct data_vio *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->mapped.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for mapped physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->mapped.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's mapped zone.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setMappedZoneCallback(struct data_vio *dataVIO,
                                         VDOAction       *callback,
                                         TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->mapped.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Check that a data_vio is running on the correct thread for its newMapped
 * zone.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInNewMappedZone(struct data_vio *dataVIO)
{
  ThreadID expected = getPhysicalZoneThreadID(dataVIO->newMapped.zone);
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for newMapped physical block %" PRIu64
                  " on thread %u, should be on thread %u",
                  dataVIO->newMapped.pbn, threadID, expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's newMapped zone.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setNewMappedZoneCallback(struct data_vio *dataVIO,
                                            VDOAction       *callback,
                                            TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPhysicalZoneThreadID(dataVIO->newMapped.zone));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a physical block operation in a data_vio's newMapped zone
 * and queue the data_vio and invoke it immediately.
 *
 * @param dataVIO   The data_vio
 * @param callback  The callback to invoke
 * @param location  The tracing info for the call site
 **/
static inline void launchNewMappedZoneCallback(struct data_vio *dataVIO,
                                               VDOAction       *callback,
                                               TraceLocation    location)
{
  setNewMappedZoneCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a data_vio is running on the journal thread.
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInJournalZone(struct data_vio *dataVIO)
{
  ThreadID expected
    = getJournalZoneThread(getThreadConfigFromDataVIO(dataVIO));
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for logical block %" PRIu64
                  " on thread %u, should be on journal thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a journal operation.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setJournalCallback(struct data_vio *dataVIO,
                                      VDOAction       *callback,
                                      TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getJournalZoneThread(getThreadConfigFromDataVIO(dataVIO)));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a journal operation and invoke it immediately.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchJournalCallback(struct data_vio *dataVIO,
                                         VDOAction       *callback,
                                         TraceLocation    location)
{
  setJournalCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check that a data_vio is running on the packer thread
 *
 * @param dataVIO  The data_vio in question
 **/
static inline void assertInPackerZone(struct data_vio *dataVIO)
{
  ThreadID expected = getPackerZoneThread(getThreadConfigFromDataVIO(dataVIO));
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((expected == threadID),
                  "data_vio for logical block %" PRIu64
                  " on thread %u, should be on packer thread %u",
                  dataVIO->logical.lbn, threadID, expected);
}

/**
 * Set a callback as a packer operation.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void setPackerCallback(struct data_vio *dataVIO,
                                     VDOAction       *callback,
                                     TraceLocation    location)
{
  setCallback(dataVIOAsCompletion(dataVIO), callback,
              getPackerZoneThread(getThreadConfigFromDataVIO(dataVIO)));
  dataVIOAddTraceRecord(dataVIO, location);
}

/**
 * Set a callback as a packer operation and invoke it immediately.
 *
 * @param dataVIO   The data_vio with which to set the callback
 * @param callback  The callback to set
 * @param location  The tracing info for the call site
 **/
static inline void launchPackerCallback(struct data_vio *dataVIO,
                                        VDOAction       *callback,
                                        TraceLocation    location)
{
  setPackerCallback(dataVIO, callback, location);
  invokeCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Check whether the advice received from Albireo is a valid data location,
 * and if it is, accept it as the location of a potential duplicate of the
 * data_vio.
 *
 * @param dataVIO  The data_vio that queried Albireo
 * @param advice   A potential location of the data, or NULL for no advice
 **/
void receiveDedupeAdvice(struct data_vio            *dataVIO,
                         const struct data_location *advice);

/**
 * Set the location of the duplicate block for a data_vio, updating the
 * isDuplicate and duplicate fields from a zoned_pbn.
 *
 * @param dataVIO  The data_vio to modify
 * @param source   The location of the duplicate
 **/
void setDuplicateLocation(struct data_vio        *dataVIO,
                          const struct zoned_pbn  source);

/**
 * Clear a data_vio's mapped block location, setting it to be unmapped. This
 * indicates the block map entry for the logical block is either unmapped or
 * corrupted.
 *
 * @param dataVIO  The data_vio whose mapped block location is to be reset
 **/
void clearMappedLocation(struct data_vio *dataVIO);

/**
 * Set a data_vio's mapped field to the physical location recorded in the block
 * map for the logical block in the vio.
 *
 * @param dataVIO  The data_vio whose field is to be set
 * @param pbn      The physical block number to set
 * @param state    The mapping state to set
 *
 * @return VDO_SUCCESS or an error code if the mapping is unusable
 **/
int setMappedLocation(struct data_vio     *dataVIO,
                      PhysicalBlockNumber  pbn,
                      BlockMappingState    state)
  __attribute__((warn_unused_result));

/**
 * Attempt to acquire the lock on a logical block. This is the start of the
 * path for all external requests. It is registered in prepareDataVIO().
 *
 * @param completion  The data_vio for an external data request as a completion
 **/
void attemptLogicalBlockLock(struct vdo_completion *completion);

/**
 * Release the lock on the logical block, if any, that a data_vio has acquired.
 *
 * @param dataVIO  The data_vio releasing its logical block lock
 **/
void releaseLogicalBlockLock(struct data_vio *dataVIO);

#endif // DATA_VIO_H
