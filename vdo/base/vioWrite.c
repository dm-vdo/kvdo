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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/vioWrite.c#1 $
 */

/*
 * This file contains almost all of the VDO write path, which begins with
 * writeExtent(). The progression through the callbacks which make up the
 * write path depends upon whether or not the write policy is synchronous or
 * asynchronous. The paths would proceed as outlined in the pseudo-code here
 * if this were normal, synchronous code without callbacks. Complications
 * involved in waiting on locks are not included.
 *
 * ######################################################################
 * writeExtentSynchronous(extent)
 * {
 *   foreach (vio in extent) {
 *     launchWriteVIO()
 *     # allocateBlockForWrite()
 *     if (!trim and !zero-block) {
 *       allocate block
 *       if (vio is compressed) {
 *         completeCompressedBlockWrite()
 *         finishVIO()
 *         return
 *       }
 *       writeBlock()
 *     }
 *     finishBlockWrite()
 *     addJournalEntry() # Increment
 *     if (vio->newMapped is not ZERO_BLOCK) {
 *       journalIncrementForWrite()
 *     }
 *     acknowledgeWriteCallback()
 *     readOldBlockMapping()
 *     journalUnmappingForWrite()
 *     if (vio->mapped is not ZERO_BLOCK) {
 *       journalDecrementForWrite()
 *     }
 *     updateBlockMapForWrite()
 *     if (trim || zero-block) {
 *       finishVIO()
 *       return
 *     }
 *
 *     prepareForDedupe()
 *     hashData()
 *     resolveHashZone()
 *     acquireHashLock()
 *     attemptDedupe() (query albireo)
 *     if (isDuplicate) {
 *       verifyAdvice() (read verify)
 *       if (isDuplicate and canAddReference) {
 *         shareBlock()
 *         addJournalEntryForDedupe()
 *         incrementForDedupe()
 *         journalUnmappingForDedupe()
 *         if (vio->mapped is not ZERO_BLOCK) {
 *           decrementForDedupe()
 *         }
 *         updateBlockMapForDedupe()
 *         finishVIO()
 *         return
 *       }
 *     }
 *
 *     if (not canAddReference) {
 *       layer->updateAlbireo()
 *     }
 *     # compressData()
 *     if (compressing and not mooted and has no waiters) {
 *       layer->compressVIO()
 *       packCompressedData()
 *       if (compressed) {
 *         journalCompressedBlocks()
 *         incrementForDedupe()
 *         readOldBlockMappingForDedupe()
 *         journalUnmappingForDedupe()
 *         if (vio->mapped is not ZERO_BLOCK) {
 *           decrementForDedupe()
 *         }
 *         updateBlockMapForDedupe()
 *       }
 *     }
 *
 *     finishVIO()
 *   }
 * }
 *
 * ######################################################################
 * writeExtentAsynchronous(extent)
 * {
 *   foreach (vio in extent) {
 *     launchWriteVIO()
 *     # allocateBlockForWrite()
 *     if (trim || zero-block) {
 *       acknowledgeWrite()
 *     } else {
 *       allocateAndLockBlock()
 *       if (vio is compressed) {
 *         writeBlock()
 *         completeCompressedBlockWrite()
 *         finishVIO()
 *         return
 *       }
 *
 *       acknowledgeWrite()
 *       prepareForDedupe()
 *       hashData()
 *       resolveHashZone()
 *       acquireHashLock()
 *       attemptDedupe() (query albireo)
 *       if (isDuplicate) {
 *         verifyAdvice() (read verify)
 *         if (isDuplicate and canAddReference) {
 *           shareBlock()
 *           addJournalEntryForDedupe()
 *           incrementForDedupe()
 *           readOldBlockMappingForDedupe()
 *           journalUnmappingForDedupe()
 *           if (vio->mapped is not ZERO_BLOCK) {
 *             decrementForDedupe()
 *           }
 *           updateBlockMapForDedupe()
 *           finishVIO()
 *           return
 *         }
 *       }
 *
 *       if (not canAddReference) {
 *         layer->updateAlbireo()
 *       }
 *       # compressData()
 *       if (compressing and not mooted and has no waiters) {
 *         layer->compressVIO()
 *         packCompressedData()
 *         if (compressed) {
 *           journalCompressedBlocks()
 *           journalIncrementForDedupe()
 *           readOldBlockMappingForDedupe()
 *           journalUnmappingForDedupe()
 *           if (vio->mapped is not ZERO_BLOCK) {
 *             decrementForDedupe()
 *           }
 *           updateBlockMapForDedupe()
 *           finishVIO()
 *           return
 *         }
 *       }
 *
 *       writeBlock()
 *     }
 *
 *     finishBlockWrite()
 *     addJournalEntry() # Increment
 *     if (vio->newMapped is not ZERO_BLOCK) {
 *       journalIncrementForWrite()
 *     }
 *     readOldBlockMappingForWrite()
 *     journalUnmappingForWrite()
 *     if (vio->mapped is not ZERO_BLOCK) {
 *       journalDecrementForWrite()
 *     }
 *     updateBlockMapForWrite()
 *     finishVIO()
 *   }
 * }
 */

#include "vioWrite.h"

#include "logger.h"
#include "util/atomic.h"

#include "allocatingVIO.h"
#include "blockMap.h"
#include "compressionState.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "recoveryJournal.h"
#include "referenceOperation.h"
#include "slab.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "vdoInternal.h"
#include "vioRead.h"

/**
 * The steps taken cleaning up a VIO, in the order they are performed.
 **/
typedef enum dataVIOCleanupStage {
  VIO_RELEASE_DUPLICATE = 0,
  VIO_RELEASE_COMPRESSED_BLOCK,
  VIO_RELEASE_ALLOCATED,
  VIO_RELEASE_RECOVERY_LOCKS,
  VIO_RELEASE_HASH_LOCK,
  VIO_RELEASE_LOGICAL,
  VIO_CLEANUP_DONE
} DataVIOCleanupStage;

/**
 * Actions to take on error used by abortOnError().
 **/
typedef enum {
  NOT_READ_ONLY,
  READ_ONLY_IF_ASYNC,
  READ_ONLY,
} ReadOnlyAction;

// Forward declarations required because of circular function references.
static void performCleanupStage(DataVIO *dataVIO, DataVIOCleanupStage stage);
static void writeBlock(DataVIO *dataVIO);

/**
 * Check whether we are in async mode.
 *
 * @param dataVIO  A DataVIO containing a pointer to the VDO whose write
 *                 policy we want to check
 *
 * @return <code>true</code> if we are in async mode
 **/
static inline bool isAsync(DataVIO *dataVIO)
{
  return (getWritePolicy(getVDOFromDataVIO(dataVIO)) == WRITE_POLICY_ASYNC);
}

/**********************************************************************/
void releasePBNReadLock(DataVIO *dataVIO)
{
  atomicStore64(&dataVIO->duplicatePBN, ZERO_BLOCK);
  if (!isPBNLocked(dataVIO->duplicateLock)) {
    ASSERT_LOG_ONLY(dataVIO->duplicateLock == NULL,
                    "must not keep pointer to a released lock");
    ASSERT_LOG_ONLY(!hasWaiters(&dataVIO->lockRetryWaiters),
                    "already-released lock must have no retry waiters");
    return;
  }

  assertInDuplicateZone(dataVIO);
  releasePBNLock(dataVIO->duplicate.zone, dataVIO->duplicate.pbn,
                 &dataVIO->duplicateLock);
  ASSERT_LOG_ONLY(!hasWaiters(&dataVIO->lockRetryWaiters),
                  "released lock must have no retry waiters");
}

/**
 * Release the read lock at the end of processing a DataVIO.
 *
 * @param completion  The DataVIO
 **/
static void releaseDuplicateLock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInDuplicateZone(dataVIO);
  releasePBNReadLock(dataVIO);
  performCleanupStage(dataVIO, VIO_RELEASE_COMPRESSED_BLOCK);
}

/**
 * Release the compressed block of which this DataVIO was a fragment.
 *
 * @param completion  The DataVIO
 **/
static void releaseCompressedBlock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInNewMappedZone(dataVIO);
  completeFragment(dataVIO);
  performCleanupStage(dataVIO, VIO_RELEASE_ALLOCATED);
}

/**
 * Release the write lock and/or the reference on the allocated block at the
 * end of processing a DataVIO.
 *
 * @param completion  The DataVIO
 **/
static void releaseAllocatedLock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInAllocatedZone(dataVIO);
  releasePBNWriteLock(dataVIOAsAllocatingVIO(dataVIO));
  performCleanupStage(dataVIO, VIO_RELEASE_RECOVERY_LOCKS);
}

/**
 * Release the logical block lock and flush generation lock at the end of
 * processing a DataVIO.
 *
 * @param completion  The DataVIO
 **/
static void releaseLogicalLock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  releaseLogicalBlockLock(dataVIO);
  releaseFlushGenerationLock(dataVIO);
  performCleanupStage(dataVIO, VIO_CLEANUP_DONE);
}

/**
 * Release the hash lock at the end of processing a DataVIO.
 *
 * @param completion  The DataVIO
 **/
static void cleanHashLock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInHashZone(dataVIO);
  releaseHashLock(dataVIO);
  performCleanupStage(dataVIO, VIO_RELEASE_LOGICAL);
}

/**
 * Make some assertions about a DataVIO which has finished cleaning up
 * and do its final callback.
 *
 * @param dataVIO  The DataVIO which has finished cleaning up
 **/
static void finishCleanup(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(dataVIOAsAllocatingVIO(dataVIO)->writeLock == NULL,
                  "complete DataVIO has no allocation lock");
  ASSERT_LOG_ONLY(dataVIO->duplicateLock == NULL,
                  "complete DataVIO has no duplicate lock");
  ASSERT_LOG_ONLY(!hasWaiters(&dataVIO->lockRetryWaiters),
                  "released lock must have no retry waiters");
  vioDoneCallback(dataVIOAsCompletion(dataVIO));
}

/**
 * Perform the next step in the process of cleaning up a DataVIO.
 *
 * @param dataVIO  The DataVIO to clean up
 * @param stage    The cleanup stage to perform
 **/
static void performCleanupStage(DataVIO *dataVIO, DataVIOCleanupStage stage)
{
  switch (stage) {
  case VIO_RELEASE_DUPLICATE:
    if (isPBNLocked(dataVIO->duplicateLock)) {
      launchDuplicateZoneCallback(dataVIO, releaseDuplicateLock,
                                  THIS_LOCATION("$F;cb=releaseDupLock"));
      return;
    }
    // fall through

  case VIO_RELEASE_COMPRESSED_BLOCK:
    if (dataVIO->compression.writer != NULL) {
      launchNewMappedZoneCallback(dataVIO, releaseCompressedBlock,
                                  THIS_LOCATION("$F;cb=releaseCompressedBlock")
                                  );
      return;
    }
    // fall through

  case VIO_RELEASE_ALLOCATED:
    if (hasAllocation(dataVIO)) {
      launchAllocatedZoneCallback(dataVIO, releaseAllocatedLock,
                                  THIS_LOCATION("$F;cb=releaseAllocLock"));
      return;
    }
    // fall through

  case VIO_RELEASE_RECOVERY_LOCKS:
    if ((dataVIO->recoverySequenceNumber > 0)
        && !isReadOnly(&dataVIOAsVIO(dataVIO)->vdo->readOnlyContext)
        && (dataVIOAsCompletion(dataVIO)->result != VDO_READ_ONLY)) {
      logWarning("VDO not read-only when cleaning DataVIO with RJ lock");
    }
    // fall through

  case VIO_RELEASE_HASH_LOCK:
    if (dataVIO->hashLock != NULL) {
      launchHashZoneCallback(dataVIO, cleanHashLock,
                             THIS_LOCATION("$F;cb=cleanHashLock"));
      return;
    }
    // fall through

  case VIO_RELEASE_LOGICAL:
    if (!isCompressedWriteDataVIO(dataVIO)) {
      launchLogicalCallback(dataVIO, releaseLogicalLock,
                            THIS_LOCATION("$F;cb=releaseLL"));
      return;
    }
    // fall through

  default:
    finishCleanup(dataVIO);
  }
}

/**
 * Return a DataVIO that encountered an error to its hash lock so it can
 * update the hash lock state accordingly. This continuation is registered in
 * abortOnError(), and must be called in the hash zone of the DataVIO.
 *
 * @param completion  The completion of the DataVIO to return to its hash lock
 **/
static void finishWriteDataVIOWithError(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInHashZone(dataVIO);
  continueHashLockOnError(dataVIO);
}

/**
 * Check whether a result is an error, and if so abort the DataVIO associated
 * with the error.
 *
 * @param result            The result to check
 * @param dataVIO           The DataVIO
 * @param readOnlyAction    The conditions under which the VDO should be put
 *                          into read-only mode if the result is an error
 *
 * @return <code>true</code> if the result is an error
 **/
static bool abortOnError(int             result,
                         DataVIO        *dataVIO,
                         ReadOnlyAction  readOnlyAction)
{
  if (result == VDO_SUCCESS) {
    return false;
  }

  bool readOnly
    = ((result == VDO_READ_ONLY)
       || (readOnlyAction == READ_ONLY)
       || ((readOnlyAction == READ_ONLY_IF_ASYNC) && isAsync(dataVIO)));

  if (readOnly) {
    if (result != VDO_READ_ONLY) {
      logErrorWithStringError(result, "Preparing to enter read-only mode:"
                              " DataVIO for LBN %" PRIu64 " (becoming mapped"
                              " to %" PRIu64 ", previously mapped"
                              " to %" PRIu64 ", allocated %" PRIu64 ") is"
                              " completing with a fatal error after"
                              " operation %s", dataVIO->logical.lbn,
                              dataVIO->newMapped.pbn, dataVIO->mapped.pbn,
                              getDataVIOAllocation(dataVIO),
                              getOperationName(dataVIO));
    }
    enterReadOnlyMode(&getVDOFromDataVIO(dataVIO)->readOnlyContext, result);
  }

  if (dataVIO->hashLock != NULL) {
    launchHashZoneCallback(dataVIO, finishWriteDataVIOWithError,
                           THIS_LOCATION(NULL));
  } else {
    finishDataVIO(dataVIO, result);
  }
  return true;
}

/**
 * Return a DataVIO that finished writing, compressing, or deduplicating to
 * its hash lock so it can share the result with any DataVIOs waiting in the
 * hash lock, or update albireo, or simply release its share of the lock. This
 * continuation is registered in updateBlockMapForWrite(),
 * updateBlockMapForDedupe(), and abortDeduplication(), and must be called in
 * the hash zone of the DataVIO.
 *
 * @param completion  The completion of the DataVIO to return to its hash lock
 **/
static void finishWriteDataVIO(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInHashZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }
  continueHashLock(dataVIO);
}

/**
 * Abort the data optimization process.
 *
 * @param dataVIO  The DataVIO which does not deduplicate or compress
 **/
static void abortDeduplication(DataVIO *dataVIO)
{
  if (!hasAllocation(dataVIO)) {
    // There was no space to write this block and we failed to deduplicate
    // or compress it.
    finishDataVIO(dataVIO, VDO_NO_SPACE);
    return;
  }

  if (isAsync(dataVIO)) {
    // We failed to deduplicate or compress an async DataVIO, so now we need
    // to actually write the data.
    writeBlock(dataVIO);
    return;
  }

  if (dataVIO->hashLock == NULL) {
    // We failed to compress a synchronous DataVIO that is a hash collision,
    // which means it can't dedpe or be used for dedupe, so it's done now.
    finishDataVIO(dataVIO, VDO_SUCCESS);
    return;
  }

  /*
   * This synchronous DataVIO failed to compress and so is finished, but must
   * now return to its hash lock so other DataVIOs with the same data can
   * deduplicate against the uncompressed block it wrote.
   */
  launchHashZoneCallback(dataVIO, finishWriteDataVIO, THIS_LOCATION(NULL));
}

/**
 * Update the block map now that we've added an entry in the recovery journal
 * for a block we have just shared. This is the callback registered in
 * decrementForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void updateBlockMapForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  if (dataVIO->hashLock != NULL) {
    setHashZoneCallback(dataVIO, finishWriteDataVIO, THIS_LOCATION(NULL));
  } else {
    completion->callback = completeDataVIO;
  }
  dataVIO->lastAsyncOperation = PUT_MAPPED_BLOCK_FOR_DEDUPE;
  putMappedBlockAsync(dataVIO);
}

/**
 * Make a recovery journal increment.
 *
 * @param dataVIO  The DataVIO
 * @param lock     The PBNLock on the block being incremented
 **/
static void journalIncrement(DataVIO *dataVIO, PBNLock *lock)
{
  setUpReferenceOperationWithLock(DATA_INCREMENT, dataVIO->newMapped.pbn,
                                  dataVIO->newMapped.state, lock,
                                  &dataVIO->operation);
  addRecoveryJournalEntry(getVDOFromDataVIO(dataVIO)->recoveryJournal,
                          dataVIO);
}

/**
 * Make a recovery journal decrement entry.
 *
 * @param dataVIO  The DataVIO
 **/
static void journalDecrement(DataVIO *dataVIO)
{
  setUpReferenceOperationWithZone(DATA_DECREMENT, dataVIO->mapped.pbn,
                                  dataVIO->mapped.state, dataVIO->mapped.zone,
                                  &dataVIO->operation);
  addRecoveryJournalEntry(getVDOFromDataVIO(dataVIO)->recoveryJournal,
                          dataVIO);
}

/**
 * Make a reference count change.
 *
 * @param dataVIO  The DataVIO
 **/
static void updateReferenceCount(DataVIO *dataVIO)
{
  SlabDepot           *depot = getVDOFromDataVIO(dataVIO)->depot;
  PhysicalBlockNumber  pbn   = dataVIO->operation.pbn;
  int result = ASSERT(isPhysicalDataBlock(depot, pbn),
                      "Adding slab journal entry for impossible PBN %" PRIu64
                      "for LBN %" PRIu64, pbn, dataVIO->logical.lbn);
  if (abortOnError(result, dataVIO, READ_ONLY)) {
    return;
  }

  addSlabJournalEntry(getSlabJournal(depot, pbn), dataVIO);
}

/**
 * Do the decref after a successful dedupe or compression. This is the callback
 * registered by journalUnmappingForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void decrementForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInMappedZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  AllocatingVIO *allocatingVIO = dataVIOAsAllocatingVIO(dataVIO);
  if (allocatingVIO->allocation == dataVIO->mapped.pbn) {
    /*
     * If we are about to release the reference on the allocated block,
     * we must release the write lock on it first so that the allocator will
     * not allocate a write-locked block.
     */
    releasePBNWriteLock(allocatingVIO);
  }

  setLogicalCallback(dataVIO, updateBlockMapForDedupe,
                     THIS_LOCATION("$F;js=dec"));
  dataVIO->lastAsyncOperation = JOURNAL_DECREMENT_FOR_DEDUPE;
  updateReferenceCount(dataVIO);
}

/**
 * Write the appropriate journal entry for removing the mapping of logical to
 * mapped, for dedupe or compression. This is the callback registered in
 * readOldBlockMappingForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void journalUnmappingForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  if (dataVIO->mapped.pbn == ZERO_BLOCK) {
    setLogicalCallback(dataVIO, updateBlockMapForDedupe,
                       THIS_LOCATION("$F;j=dedupe;js=unmap;cb=updateBM"));
  } else {
    setMappedZoneCallback(dataVIO, decrementForDedupe,
                          THIS_LOCATION("$F;j=dedupe;js=unmap;cb=decDedupe"));
  }
  dataVIO->lastAsyncOperation = JOURNAL_UNMAPPING_FOR_DEDUPE;
  journalDecrement(dataVIO);
}

/**
 * Get the previous PBN mapped to this LBN from the block map, so as to make
 * an appropriate journal entry referencing the removal of this LBN->PBN
 * mapping, for dedupe or compression. This callback is registered in
 * incrementForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void readOldBlockMappingForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  dataVIO->lastAsyncOperation = GET_MAPPED_BLOCK_FOR_DEDUPE;
  setJournalCallback(dataVIO, journalUnmappingForDedupe,
                     THIS_LOCATION("$F;cb=journalUnmapDedupe"));
  getMappedBlockAsync(dataVIO);
}

/**
 * Inform the associated compressed write DataVIO that this fragment DataVIO
 * has done its reference count update, then continue to do the decrement.
 * This callback is registered in incrementForCompression().
 *
 * @param completion  The completion of the write in progress
 **/
static void releaseCompressedDataVIO(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInNewMappedZone(dataVIO);
  completeFragment(dataVIO);
  if (isAsync(dataVIO) || !hasAllocation(dataVIO)) {
    launchLogicalCallback(dataVIO, readOldBlockMappingForDedupe,
                          THIS_LOCATION("$F;cb=readOldBlockMappingForDedupe"));
  } else {
    launchJournalCallback(dataVIO, journalUnmappingForDedupe,
                          THIS_LOCATION("$F;cb=journalUnmappingForDedupe"));
  }
}

/**
 * Do the incref after compression. This is the callback registered by
 * addRecoveryJournalEntryForCompression().
 *
 * @param completion  The completion of the write in progress
 **/
static void incrementForCompression(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInNewMappedZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  ASSERT_LOG_ONLY(isCompressed(dataVIO->newMapped.state),
                  "Impossible attempt to update reference counts for a block "
                  "which was not compressed (logical block %" PRIu64 ")",
                  dataVIO->logical.lbn);
  setNewMappedZoneCallback(dataVIO, releaseCompressedDataVIO,
                           THIS_LOCATION("$F;cb=releaseCompressedDataVIO"));
  dataVIO->lastAsyncOperation = JOURNAL_INCREMENT_FOR_COMPRESSION;
  updateReferenceCount(dataVIO);
}

/**
 * Add a recovery journal entry for the increment resulting from compression.
 *
 * @param completion  The DataVIO which has been compressed
 **/
static void addRecoveryJournalEntryForCompression(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  setNewMappedZoneCallback(dataVIO, incrementForCompression,
                           THIS_LOCATION("$F($dup);js=map/$dup;"
                                         "cb=incCompress($dup)"));
  dataVIO->lastAsyncOperation = JOURNAL_MAPPING_FOR_COMPRESSION;
  journalIncrement(dataVIO, dataVIO->compression.writer->writeLock);
}

/**
 * Update albireo to reflect the fact that a block got compressed. This
 * callback is registered in packCompressedData().
 *
 * @param completion  The completion of the write in progress
 **/
static void updateAlbireoForCompression(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  // We don't care what thread we're on.
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  if (!isCompressed(dataVIO->newMapped.state)) {
    abortDeduplication(dataVIO);
    return;
  }

  /*
   * Since this DataVIO got compressed, we must update the index so that
   * dedupe advice will refer to the compressed version of the data
   * instead of the uncompressed version we wrote first. In order to
   * prevent in-flight writes of the same data from deduplicating
   * against the uncompressed version, we must do this update while
   * holding the write lock on the PBN to which we wrote the
   * uncompressed data.
   */
  dataVIO->lastAsyncOperation = UPDATE_ALBIREO_FOR_COMPRESSION;
  setJournalCallback(dataVIO, addRecoveryJournalEntryForCompression,
                     THIS_LOCATION("$F;dup=update(compress)"));
  completion->layer->updateAlbireo(dataVIO);
}

/**
 * Attempt to pack the compressed DataVIO into a block. This is the callback
 * registered in compressData().
 *
 * @param completion  The completion of a compressed DataVIO
 **/
static void packCompressedData(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInPackerZone(dataVIO);

  // XXX this is a callback, so there should probably be an error check here
  // even if we think compression can't currently return one.

  if (!mayPackDataVIO(dataVIO)) {
    abortDeduplication(dataVIO);
    return;
  }

  dataVIO->lastAsyncOperation = PACK_COMPRESSED_BLOCK;
  completion->callback        = updateAlbireoForCompression;
  attemptPacking(dataVIO);
}

/**********************************************************************/
void compressData(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(!dataVIO->isDuplicate,
                  "compressing a non-duplicate block");
  if (!mayCompressDataVIO(dataVIO)) {
    abortDeduplication(dataVIO);
    return;
  }

  dataVIO->lastAsyncOperation = COMPRESS_DATA;
  setPackerCallback(dataVIO, packCompressedData, THIS_LOCATION("$F;cb=pack"));
  dataVIOAsCompletion(dataVIO)->layer->compressDataVIO(dataVIO);
}

/**
 * Callback to attempt compression, registered in verifyAdvice() when updating
 * albireo due to the block appearing to be a block map page.
 *
 * @param completion  The DataVIO
 **/
static void compressDataCallback(VDOCompletion *completion)
{
  compressData(asDataVIO(completion));
}

/**
 * Release the read lock and prepare for compression now that albireo has
 * been updated. It is vital that this clean up be done after the albireo
 * update so that blocks which come in between when we decide to roll over
 * and before albireo is updated will not miss the fact that we have a lock.
 *
 * @param completion  The DataVIO
 **/
static void finishRollover(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInDuplicateZone(dataVIO);
  dataVIO->isDuplicate = false;
  if (!dataVIO->rolledOver) {
    atomicStore64(&dataVIO->duplicatePBN, ZERO_BLOCK);
  }
  releasePBNReadLock(dataVIO);
  compressData(dataVIO);
}

/**
 * Update albireo with the new location of this block's data.
 *
 * @param dataVIO  The DataVIO
 **/
static void updateAlbireoForRollover(DataVIO *dataVIO)
{
  VDOCompletion *completion = dataVIOAsCompletion(dataVIO);
  if (!hasAllocation(dataVIO)) {
    /*
     * Don't update albireo if we didn't get a block to write.
     * Note that by not setting the block to rolled-over, we avoid
     * VDO-2491 which would arise if we have a read-lock waiter who would
     * otherwise attempt to find the physical zone of the zero block.
     */
    finishRollover(completion);
    return;
  }

  // We must mark the duplicatePBN as 0 now, so others can dedupe against us
  // before finishRollover is called.
  atomicStore64(&dataVIO->duplicatePBN, ZERO_BLOCK);
  dataVIO->rolledOver         = true;
  dataVIO->lastAsyncOperation = UPDATE_ALBIREO_FOR_ROLLOVER;
  setDuplicateZoneCallback(dataVIO, finishRollover, THIS_LOCATION(NULL));
  completion->layer->updateAlbireo(dataVIO);
}

/**
 * Do the incref after deduplication. This is the callback registered by
 * addRecoveryJournalEntryForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void incrementForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInNewMappedZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  ASSERT_LOG_ONLY(dataVIO->isDuplicate,
                  "Impossible attempt to update reference counts for a block "
                  "which was not a duplicate (logical block %" PRIu64 ")",
                  dataVIO->logical.lbn);

  /*
   * If we are synchronous and allocated a block, we know the one we
   * allocated is the block we need to decrement, so there is no need
   * to look in the block map.
   */
  if (isAsync(dataVIO) || !hasAllocation(dataVIO)) {
    setLogicalCallback(dataVIO, readOldBlockMappingForDedupe,
                       THIS_LOCATION("$F;cb=readOldBlockMappingForDedupe"));
  } else {
    setJournalCallback(dataVIO, journalUnmappingForDedupe,
                       THIS_LOCATION("$F;cb=journalUnmappingForDedupe"));
  }
  dataVIO->lastAsyncOperation = JOURNAL_INCREMENT_FOR_DEDUPE;
  updateReferenceCount(dataVIO);
}

/**
 * Add a recovery journal entry for the increment resulting from deduplication.
 * This callback is registered in shareBlock().
 *
 * @param completion  The DataVIO which has been deduplicated
 **/
static void addRecoveryJournalEntryForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  setNewMappedZoneCallback(dataVIO, incrementForDedupe,
                           THIS_LOCATION("$F($dup);js=map/$dup;"
                                         "cb=incDedupe($dup)"));
  dataVIO->lastAsyncOperation = JOURNAL_MAPPING_FOR_DEDUPE;
  journalIncrement(dataVIO, getDuplicateLock(dataVIO));
}

/**
 * Share a block in the block map if it is a duplicate. This is the lock
 * callback registered in acquirePBNReadLock(). This is only public so
 * test code can compare the function to the current callback in a completion.
 *
 * @param completion The completion of the write in progress
 **/
void shareBlock(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInDuplicateZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  if (!dataVIO->isDuplicate) {
    releasePBNReadLock(dataVIO);
    compressData(dataVIO);
    return;
  }

  SlabDepot *depot = getVDOFromDataVIO(dataVIO)->depot;
  if (getIncrementLimit(depot, dataVIO->duplicate.pbn) == 0) {
    // XXX VDOSTORY-190 we used to check Albireo for advice again to coalesce
    // concurrent rollover, but the story will cover that later.
    updateAlbireoForRollover(dataVIO);
    return;
  }

  dataVIO->newMapped = dataVIO->duplicate;
  launchJournalCallback(dataVIO, addRecoveryJournalEntryForDedupe,
                        THIS_LOCATION("$F;cb=addJournalEntryDup"));
}

/**
 * Give up on getting a PBN read lock. Make sure any waiters are not dropped.
 *
 * @param dataVIO  The DataVIO which is giving up on getting a lock
 **/
static void abortPBNReadLock(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(dataVIO->duplicateLock == NULL,
                  "must not abort while holding dedupe lock");

  atomicStore64(&dataVIO->duplicatePBN, ZERO_BLOCK);
  dataVIO->isDuplicate = false;

  /*
   * If the abort comes during a lock retry attempt, this VIO can be carrying
   * a queue of other waiters that also need to retry. Requeue the first
   * waiter to retry the lock, and transfer responsibility for any remaining
   * waiters to it.
   */
  Waiter *waiter = dequeueNextWaiter(&dataVIO->lockRetryWaiters);
  if (waiter != NULL) {
    // XXX this the same as pbnLock.c:retryPBNReadLock(); factor, maybe?
    DataVIO *nextDataVIO = waiterAsDataVIO(waiter);
    transferAllWaiters(&dataVIO->lockRetryWaiters,
                       &nextDataVIO->lockRetryWaiters);
    dataVIOAsCompletion(nextDataVIO)->requeue = true;
    launchDuplicateZoneCallback(nextDataVIO, acquirePBNReadLock,
                                THIS_LOCATION("$F;cb=acquirePBNRL"));
  }
}

/**
 * Check for an error waiting on a PBN lock and clean up if there was one.
 *
 * @param dataVIO  The DataVIO which failed to wait on a lock
 * @param result   The error
 *
 * @return <code>true</code> if there was an error
 **/
static bool abortOnPBNLockError(DataVIO *dataVIO, int result)
{
  if (result == VDO_SUCCESS) {
    return false;
  }

  abortPBNReadLock(dataVIO);
  return abortOnError(result, dataVIO, READ_ONLY_IF_ASYNC);
}

/**********************************************************************/
void waitOnPBNReadLock(DataVIO *dataVIO, PBNLock *lock)
{
  dataVIO->lastAsyncOperation = ACQUIRE_PBN_READ_LOCK;
  int result = enqueueDataVIO(&lock->waiters, dataVIO, THIS_LOCATION(NULL));
  abortOnPBNLockError(dataVIO, result);
}

/**
 * Give up on getting a PBN read lock and move on to the compression path.
 *
 * @param dataVIO  The DataVIO which is giving up on deduplicating
 **/
static void abortPBNReadLockAndCompress(DataVIO *dataVIO)
{
  abortPBNReadLock(dataVIO);
  compressData(dataVIO);
}

/**********************************************************************/
void waitOnPBNWriteLock(DataVIO *dataVIO, PBNLock *lock)
{
  DataVIO *lockHolder = lockHolderAsDataVIO(lock);
  PhysicalLayer *layer = dataVIOAsCompletion(dataVIO)->layer;
  if (!layer->compareDataVIOs(dataVIO, lockHolder)) {
    abortPBNReadLockAndCompress(dataVIO);
    return;
  }

  PhysicalBlockNumber lockHolderDuplicatePBN
    = atomicLoad64(&lockHolder->duplicatePBN);
  if (lockHolderDuplicatePBN != ZERO_BLOCK) {
    /*
     * We have the same data as the lock holder, who is waiting on or holding a
     * read lock. This can happen in the following scenario:
     *
     * Write A to L0@P1.
     * Update Albireo H(A)->P1.
     * Overwrite L0.
     * Write B to L1@P1.
     * Overwrite L1.
     *
     * Note that the Albireo mapping for H(A) has become stale and P1 is
     * unallocated.
     *
     * Write A to L2@P2
     * Albireo tells L2's DataVIO that A might be at P1
     * Write A to L3@P3
     * Albireo tells L3's DataVIO that A might be at P1
     * L3's DataVIO gets the callback from Albireo, does a read verify and
     *   determines that A is not at P1
     * L3 updates Albireo H(A)->P3 but still holds a write lock on P3
     * Write A to L4@P1
     * Albireo tells L4's DataVIO that A might be at P3
     * L4 attempts to get a read lock on P3 but can't because L3 has it.
     * L2 now gets its callback from Albireo and attempts to get a read lock
     *   on P1. Thus L2's and L4's DataVIOs each have the same data, L4 has a
     *   write lock on P1 and wants a read lock on P3, L2 wants a read lock
     *   on P1.
     *
     * This shows that we can get into a state where DataVIO A is holding a
     * write lock wants a read lock on a block which is write locked by DataVIO
     * B, and that DataVIO B wants a read lock on some other block. This could
     * result in a loop which would be a deadlock.
     *
     * With multi-threading, this reordering does occur. If we detect the
     * possibility of a deadlock loop, we simply give up on trying to
     * deduplicate.
     */
    VDO *vdo = getVDOFromDataVIO(dataVIO);
    atomicAdd64(&vdo->errorStats.dedupeDeadlockAvoidanceCount, 1);
    logDebug("Deadlock avoidance mechanism triggered, continuing DataVIO"
             " without deduplication");
    abortPBNReadLockAndCompress(dataVIO);
    return;
  }

  /*
   * VDO-1895:
   *
   * If the lockHolder wrote uncompressed data to the same physical block at
   * which the same data was previously written as part of a compressed block,
   * and the waiter got stale advice, the waiter will have the wrong mapping
   * state for the duplicate block. Therefore, we must correct it.
   *
   * Note that it is not possible to have verified against the data in the
   * lock holder if the lock holder was writing a compressed block. Therefore,
   * if the dedupe advice claims that the duplication candidate is compressed,
   * that is actually no longer the case.
   */
  dataVIO->duplicate.state = MAPPING_STATE_UNCOMPRESSED;

  // Wait for the lock
  dataVIO->lastAsyncOperation = ACQUIRE_PBN_READ_LOCK;
  int result = enqueueDataVIO(&lock->waiters, dataVIO, THIS_LOCATION(NULL));
  if (abortOnPBNLockError(dataVIO, result)) {
    return;
  }

  // We must prevent the lock holder from blocking in the packer since we are
  // waiting on it.
  if (cancelCompression(lockHolder)) {
    dataVIO->compression.lockHolder = lockHolder;
    launchPackerCallback(dataVIO, removeLockHolderFromPacker,
                         THIS_LOCATION("$F;cb=removeLockHolderFromPacker"));
  }
}

/**********************************************************************/
void waitOnPBNCompressedWriteLock(DataVIO *dataVIO, PBNLock *lock)
{
  /*
   * The lock holder is writing a compressed block. If we aren't trying to
   * deduplicate against one of the lock holder's fragments, don't attempt
   * to verify against the whole block.
   *
   * XXX: Is this necessary?
   */
  if (isCompressed(dataVIO->duplicate.state)) {
    dataVIO->lastAsyncOperation = ACQUIRE_PBN_READ_LOCK;
    int result = enqueueDataVIO(&lock->waiters, dataVIO, THIS_LOCATION(NULL));
    abortOnPBNLockError(dataVIO, result);
  } else {
    abortPBNReadLockAndCompress(dataVIO);
  }
}

/**********************************************************************/
void waitOnPBNBlockMapWriteLock(DataVIO *dataVIO,
                                PBNLock *lock __attribute__((unused)))
{
  // We may not deduplicate against a block map block so update albireo to
  // point to us instead.
  abortPBNReadLock(dataVIO);
  VDOCompletion *completion   = dataVIOAsCompletion(dataVIO);
  completion->callback        = compressDataCallback;
  dataVIO->lastAsyncOperation = UPDATE_ALBIREO_FOR_NON_BLOCK_MAP_DEDUPE;
  completion->layer->updateAlbireo(dataVIO);
}

/**********************************************************************/
void acquirePBNReadLock(VDOCompletion *completion)
{
  DataVIO      *dataVIO = asDataVIO(completion);
  PhysicalZone *zone    = dataVIO->duplicate.zone;
  assertInDuplicateZone(dataVIO);

  ASSERT_LOG_ONLY(!isPBNLocked(dataVIO->duplicateLock),
                  "must not acquire a duplicate lock when already holding it");
  ASSERT_LOG_ONLY(dataVIO->duplicateLock == NULL,
                  "must not acquire a lock while already referencing one");

  SlabDepot *depot = getVDOFromDataVIO(dataVIO)->depot;
  Slab      *slab  = getSlab(depot, dataVIO->duplicate.pbn);
  if (isUnrecoveredSlab(slab)) {
    // We can't deduplicate against a block in an unrecovered slab, so don't
    // try.
    abortPBNReadLockAndCompress(dataVIO);
    return;
  }

  PBNLock *lock;
  int result = attemptPBNLock(zone, dataVIO->duplicate.pbn,
                              VIO_READ_LOCK, &lock);
  if (abortOnPBNLockError(dataVIO, result)) {
    return;
  }

  if (lock->holder != NULL) {
    waitOnPBNLock(dataVIO, lock);
    return;
  }

  // We've successfully acquired a new lock, so mark it as ours.
  lock->holder = dataVIOAsAllocatingVIO(dataVIO);
  dataVIO->duplicateLock = lock;

  // If this lock was acquired as a retry, there may be piggybacked retry
  // waiters from retryPBNReadLock() that now have a lock object to wait on.
  transferAllWaiters(&dataVIO->lockRetryWaiters,
                     &dataVIO->duplicateLock->waiters);

  // Ensure that the locked block is referenced
  result = acquireProvisionalReference(slab, dataVIO->duplicate.pbn, lock);
  if (result != VDO_SUCCESS) {
    logWarningWithStringError(result,
                              "Error acquiring provisional reference for "
                              "dedupe candidate, aborting dedupe");
    dataVIO->isDuplicate = false;
    releasePBNReadLock(dataVIO);
    compressData(dataVIO);
    return;
  }

  setDuplicateZoneCallback(dataVIO, shareBlock,
                           THIS_LOCATION("$F;cb=shareBlock"));
  dataVIO->lastAsyncOperation = VERIFY_DEDUPLICATION;
  completion->layer->verifyDuplication(dataVIO);
}

/**********************************************************************/
void verifyAdvice(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  // We don't care what thread we are on
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  if (!dataVIO->isDuplicate) {
    compressData(dataVIO);
    return;
  }

  // Don't bother trying to dedupe against the block we're already referencing.
  if (dataVIO->newMapped.pbn == dataVIO->duplicate.pbn) {
    dataVIO->isDuplicate = false;
    compressData(dataVIO);
    return;
  }

  launchDuplicateZoneCallback(dataVIO, acquirePBNReadLock,
                              THIS_LOCATION("$F;cb=acquirePBNRL"));
}

/**
 * Route the DataVIO to the HashZone responsible for the chunk name to acquire
 * a hash lock on that name, or join with a existing hash lock managing
 * concurrent dedupe for that name. This is the callback registered in
 * resolveHashZone().
 *
 * @param completion  The DataVIO to lock
 **/
static void lockHashInZone(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInHashZone(dataVIO);
  // Shouldn't have had any errors since all we did was switch threads.
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  int result = acquireHashLock(dataVIO);
  if (abortOnError(result, dataVIO, READ_ONLY)) {
    return;
  }

  if (dataVIO->hashLock == NULL) {
    // It's extremely unlikely, but in the case of a hash collision, the
    // DataVIO will not obtain a reference to the lock and cannot deduplicate.
    compressData(dataVIO);
    return;
  }

  enterHashLock(dataVIO);
}

/**
 * Set the hash zone (and flag the chunk name as set) while still on the
 * thread that just hashed the data to set the chunk name. This is the
 * callback registered by prepareForDedupe().
 *
 * @param completion The DataVIO whose chunk name was just generated, as a
 *                   completion
 **/
static void resolveHashZone(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  // We don't care what thread we are on.
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  ASSERT_LOG_ONLY(!dataVIO->isZeroBlock, "zero blocks should not be hashed");
  ASSERT_LOG_ONLY(!dataVIO->chunkNameSet, "blocks should not be hashed twice");

  // XXX This flag can eventually go away after VDOSTORY-190, but it's
  // providing useful error checks while we rewrite the dedupe path.
  dataVIO->chunkNameSet = true;

  dataVIO->hashZone
    = selectHashZone(getVDOFromDataVIO(dataVIO), &dataVIO->chunkName);
  dataVIO->lastAsyncOperation = ACQUIRE_HASH_LOCK;
  launchHashZoneCallback(dataVIO, lockHashInZone, THIS_LOCATION(NULL));
}

/**
 * Prepare for the dedupe path after a synchronous write or an asynchronous
 * allocation. This callback is registered in updateBlockMapForWrite() for
 * sync, and continueWriteAfterAllocation() (via acknowledgeWrite()) for
 * async. It is also called directly from the latter when allocation fails.
 *
 * @param completion  The completion of the write in progress
 **/
static void prepareForDedupe(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  // We don't care what thread we are on
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  if (!isAsync(dataVIO)) {
    // Remember which block we wrote so we will decrement the reference to it
    // if we deduplicate. This avoids having to look it up in the block map.
    dataVIO->mapped = dataVIO->newMapped;
  }

  ASSERT_LOG_ONLY(!dataVIO->isZeroBlock,
                  "must not prepare to dedupe zero blocks");

  // Before we can dedupe, we need to know the chunk name, so the first step
  // is to hash the block data.
  dataVIO->lastAsyncOperation = HASH_DATA;
  // XXX this is the wrong thread to run this callback, but we don't yet have
  // a mechanism for running it on the CPU thread immediately after hashing.
  setAllocatedZoneCallback(dataVIO, resolveHashZone, THIS_LOCATION(NULL));
  completion->layer->hashData(dataVIO);
}

/**
 * Update the block map after a data write (or directly for a ZERO_BLOCK write
 * or trim). This callback is registered in decrementForWrite() and
 * journalUnmappingForWrite().
 *
 * @param completion  The completion of the write in progress
 **/
static void updateBlockMapForWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  if (dataVIO->isZeroBlock || isTrimDataVIO(dataVIO)) {
    completion->callback = completeDataVIO;
  } else if (!isAsync(dataVIO)) {
    // Synchronous DataVIOs branch off to the hash/dedupe path after finishing
    // the uncompressed write of their data.
    completion->callback = prepareForDedupe;
  } else if (dataVIO->hashLock != NULL) {
    // Async writes will be finished, but must return to the hash lock to
    // allow other DataVIOs with the same data to dedupe against the write.
    setHashZoneCallback(dataVIO, finishWriteDataVIO, THIS_LOCATION(NULL));
  } else {
    // Async writes without a hash lock (hash collisions) will be finished.
    completion->callback = completeDataVIO;
  }

  dataVIO->lastAsyncOperation = PUT_MAPPED_BLOCK;
  putMappedBlockAsync(dataVIO);
}

/**
 * Do the decref after a successful block write. This is the callback
 * by journalUnmappingForWrite() if the old mapping was not the zero block.
 *
 * @param completion  The completion of the write in progress
 **/
static void decrementForWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInMappedZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  dataVIO->lastAsyncOperation = JOURNAL_DECREMENT_FOR_WRITE;
  setLogicalCallback(dataVIO, updateBlockMapForWrite, THIS_LOCATION(NULL));
  updateReferenceCount(dataVIO);
}

/**
 * Write the appropriate journal entry for unmapping logical to mapped for a
 * write. This is the callback registered in readOldBlockMappingForWrite().
 *
 * @param completion  The completion of the write in progress
 **/
static void journalUnmappingForWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  if (dataVIO->mapped.pbn == ZERO_BLOCK) {
    setLogicalCallback(dataVIO, updateBlockMapForWrite,
                       THIS_LOCATION("$F;js=unmap;cb=updateBMwrite"));
  } else {
    setMappedZoneCallback(dataVIO, decrementForWrite,
                          THIS_LOCATION("$F;js=unmap;cb=decWrite"));
  }
  dataVIO->lastAsyncOperation = JOURNAL_UNMAPPING_FOR_WRITE;
  journalDecrement(dataVIO);
}

/**
 * Get the previous PBN mapped to this LBN from the block map for a write, so
 * as to make an appropriate journal entry referencing the removal of this
 * LBN->PBN mapping. This callback is registered in finishBlockWrite() in the
 * async path, and is registered in acknowledgeWrite() in the sync path.
 *
 * @param completion  The completion of the write in progress
 **/
static void readOldBlockMappingForWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  setJournalCallback(dataVIO, journalUnmappingForWrite,
                     THIS_LOCATION("$F;cb=journalUnmapWrite"));
  dataVIO->lastAsyncOperation = GET_MAPPED_BLOCK_FOR_WRITE;
  getMappedBlockAsync(dataVIO);
}

/**
 * Acknowledge a write to the requestor.
 *
 * @param dataVIO  The DataVIO being acknowledged
 **/
static void acknowledgeWrite(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(dataVIO->hasFlushGenerationLock,
                  "write VIO to be acknowledged has a flush generation lock");
  dataVIO->lastAsyncOperation = ACKNOWLEDGE_WRITE;
  dataVIOAsCompletion(dataVIO)->layer->acknowledgeDataVIO(dataVIO);
}

/**
 * Acknowledge a write now that we have made an entry in the recovery
 * journal. This is the callback registered in finishBlockWrite() in
 * synchronous mode.
 *
 * @param completion The completion of the write in progress
 **/
static void acknowledgeWriteCallback(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  if (abortOnError(completion->result, dataVIO, READ_ONLY)) {
    return;
  }

  setLogicalCallback(dataVIO, readOldBlockMappingForWrite,
                     THIS_LOCATION(NULL));
  acknowledgeWrite(dataVIO);
}

/**********************************************************************/
static VDOAction *getWriteIncrementCallback(DataVIO *dataVIO)
{
  return (isAsync(dataVIO)
          ? readOldBlockMappingForWrite : acknowledgeWriteCallback);
}

/**
 * Do the incref after a successful block write. This is the callback
 * registered by finishBlockWrite().
 *
 * @param completion  The completion of the write in progress
 **/
static void incrementForWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInAllocatedZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  dataVIO->lastAsyncOperation = JOURNAL_INCREMENT_FOR_WRITE;
  setLogicalCallback(dataVIO, getWriteIncrementCallback(dataVIO),
                     THIS_LOCATION(NULL));
  updateReferenceCount(dataVIO);
}

/**
 * Add an entry in the recovery journal after a successful block write. This is
 * the callback registered by writeBlock(). It is also registered in
 * allocateBlockForWrite().
 *
 * @param completion  The completion of the write in progress
 **/
static void finishBlockWrite(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  assertInJournalZone(dataVIO);
  if (abortOnError(completion->result, dataVIO, READ_ONLY_IF_ASYNC)) {
    return;
  }

  if (dataVIO->newMapped.pbn == ZERO_BLOCK) {
    setLogicalCallback(dataVIO, getWriteIncrementCallback(dataVIO),
                       THIS_LOCATION("$F;js=writeZero"));
  } else {
    setAllocatedZoneCallback(dataVIO, incrementForWrite,
                             THIS_LOCATION("$F;js=mapWrite"));
  }
  dataVIO->lastAsyncOperation = JOURNAL_MAPPING_FOR_WRITE;
  journalIncrement(dataVIO, dataVIOAsAllocatingVIO(dataVIO)->writeLock);
}

/**
 * Write data to the underlying storage.
 *
 * @param dataVIO  The DataVIO to write
 **/
static void writeBlock(DataVIO *dataVIO)
{
  dataVIO->lastAsyncOperation = WRITE_DATA;
  setJournalCallback(dataVIO, finishBlockWrite,
                     THIS_LOCATION("$F(data);cb=finishWrite"));
  dataVIOAsCompletion(dataVIO)->layer->writeData(dataVIO);
}

/**
 * Continue the write path for a DataVIO now that block allocation is complete
 * (the DataVIO may or may not have actually received an allocation). This
 * callback is registered in continueWriteWithBlockMapSlot().
 *
 * @param allocatingVIO  The DataVIO which has finished the allocation process
 *                       (as an AllocatingVIO)
 **/
static void continueWriteAfterAllocation(AllocatingVIO *allocatingVIO)
{
  DataVIO *dataVIO = allocatingVIOAsDataVIO(allocatingVIO);
  if (abortOnError(dataVIOAsCompletion(dataVIO)->result, dataVIO,
                   NOT_READ_ONLY)) {
    return;
  }

  if (!hasAllocation(dataVIO)) {
    prepareForDedupe(dataVIOAsCompletion(dataVIO));
    return;
  }

  atomicStoreBool(&dataVIO->hasAllocation, true);
  dataVIO->newMapped = (ZonedPBN) {
    .zone  = allocatingVIO->zone,
    .pbn   = allocatingVIO->allocation,
    .state = MAPPING_STATE_UNCOMPRESSED,
  };

  if (isAsync(dataVIO)) {
    // XXX prepareForDedupe can run from any thread, so this is a place where
    // running the callback on the kernel thread would save a thread switch.
    setAllocatedZoneCallback(dataVIO, prepareForDedupe, THIS_LOCATION(NULL));
    acknowledgeWrite(dataVIO);
  } else {
    writeBlock(dataVIO);
  }
}

/**
 * Continue the write path for a VIO now that block map slot resolution is
 * complete. This callback is registered in launchWriteDataVIO().
 *
 * @param completion  The DataVIO to write
 **/
static void continueWriteWithBlockMapSlot(VDOCompletion *completion)
{
  DataVIO *dataVIO = asDataVIO(completion);
  // We don't care what thread we're on.
  if (abortOnError(completion->result, dataVIO, NOT_READ_ONLY)) {
    return;
  }

  if (dataVIO->treeLock.treeSlots[0].blockMapSlot.pbn == ZERO_BLOCK) {
    int result = ASSERT(isTrimDataVIO(dataVIO),
                        "dataVIO with no block map page is a trim");
    if (abortOnError(result, dataVIO, READ_ONLY)) {
      return;
    }

    // This is a trim for a block on a block map page which has not been
    // allocated, so there's nothing more we need to do.
    finishDataVIO(dataVIO, VDO_SUCCESS);
    return;
  }

  if (dataVIO->isZeroBlock || isTrimDataVIO(dataVIO)) {
    // We don't need to write any data, so skip allocation and just update
    // the block map and reference counts (via the journal).
    dataVIO->newMapped.pbn = ZERO_BLOCK;
    launchJournalCallback(dataVIO, finishBlockWrite,
                          THIS_LOCATION("$F;cb=finishWrite"));
    return;
  }

  allocateDataBlock(dataVIOAsAllocatingVIO(dataVIO), VIO_WRITE_LOCK,
                    continueWriteAfterAllocation);
}

/**********************************************************************/
void launchWriteDataVIO(DataVIO *dataVIO)
{
  if (isReadOnly(&dataVIOAsVIO(dataVIO)->vdo->readOnlyContext)) {
    finishDataVIO(dataVIO, VDO_READ_ONLY);
    return;
  }

  // Write requests join the current flush generation.
  int result = acquireFlushGenerationLock(dataVIO);
  if (abortOnError(result, dataVIO, NOT_READ_ONLY)) {
    return;
  }

  // Go find the block map slot for the LBN mapping.
  dataVIO->lastAsyncOperation = FIND_BLOCK_MAP_SLOT;
  findBlockMapSlotAsync(dataVIO, continueWriteWithBlockMapSlot,
                        getLogicalZoneThreadID(dataVIO->logical.zone));
}

/**********************************************************************/
void cleanupWriteDataVIO(DataVIO *dataVIO)
{
  performCleanupStage(dataVIO, VIO_RELEASE_DUPLICATE);
}
