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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/refCounts.c#2 $
 */

#include "refCounts.h"
#include "refCountsInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "blockAllocatorInternals.h"
#include "completion.h"
#include "extent.h"
#include "header.h"
#include "journalPoint.h"
#include "numUtils.h"
#include "pbnLock.h"
#include "readOnlyModeContext.h"
#include "referenceBlock.h"
#include "referenceOperation.h"
#include "slab.h"
#include "slabJournal.h"
#include "slabJournalInternals.h"
#include "slabSummary.h"
#include "statusCodes.h"
#include "stringUtils.h"
#include "vdo.h"
#include "vioPool.h"
#include "waitQueue.h"

static const uint64_t BYTES_PER_WORD   = sizeof(uint64_t);
static const bool     NORMAL_OPERATION = true;

/**
 * Return the RefCounts from the RefCounts waiter.
 *
 * @param waiter  The waiter to convert
 *
 * @return  The RefCounts
 **/
__attribute__((warn_unused_result))
static inline RefCounts *refCountsFromWaiter(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (RefCounts *)
    ((uintptr_t) waiter - offsetof(RefCounts, slabSummaryWaiter));
}

/**
 * Convert the index of a reference counter back to the block number of the
 * physical block for which it is counting references. The index is assumed to
 * be valid and in-range.
 *
 * @param refCounts  The reference counts object
 * @param index      The array index of the reference counter
 *
 * @return the physical block number corresponding to the index
 **/
static PhysicalBlockNumber indexToPBN(const RefCounts *refCounts,
                                      uint64_t         index)
{
  return (refCounts->slab->start + index);
}

/**
 * Convert a block number to the index of a reference counter for that block.
 * Out of range values are pinned to the beginning or one past the end of the
 * array.
 *
 * @param refCounts  The reference counts object
 * @param pbn        The physical block number
 *
 * @return the index corresponding to the physical block number
 **/
static uint64_t pbnToIndex(const RefCounts *refCounts, PhysicalBlockNumber pbn)
{
  if (pbn < refCounts->slab->start) {
    return 0;
  }
  uint64_t index = (pbn - refCounts->slab->start);
  return minBlock(index, refCounts->blockCount);
}

/**********************************************************************/
ReferenceStatus referenceCountToStatus(ReferenceCount count)
{
  if (count == EMPTY_REFERENCE_COUNT) {
    return RS_FREE;
  } else if (count == 1) {
    return RS_SINGLE;
  } else if (count == PROVISIONAL_REFERENCE_COUNT) {
    return RS_PROVISIONAL;
  } else {
    return RS_SHARED;
  }
}

/**
 * Reset the free block search back to the first reference counter
 * in the first reference block.
 *
 * @param refCounts  The RefCounts object containing the search cursor
 **/
static void resetSearchCursor(RefCounts *refCounts)
{
  SearchCursor *cursor = &refCounts->searchCursor;

  cursor->block    = cursor->firstBlock;
  cursor->index    = 0;
  // Unit tests have slabs with only one reference block (and it's a runt).
  cursor->endIndex = minBlock(COUNTS_PER_BLOCK, refCounts->blockCount);
}

/**
 * Advance the search cursor to the start of the next reference block,
 * wrapping around to the first reference block if the current block is the
 * last reference block.
 *
 * @param refCounts  The RefCounts object containing the search cursor
 *
 * @return true unless the cursor was at the last reference block
 **/
static bool advanceSearchCursor(RefCounts *refCounts)
{
  SearchCursor *cursor = &refCounts->searchCursor;

  // If we just finished searching the last reference block, then wrap back
  // around to the start of the array.
  if (cursor->block == cursor->lastBlock) {
    resetSearchCursor(refCounts);
    return false;
  }

  // We're not already at the end, so advance to cursor to the next block.
  cursor->block++;
  cursor->index = cursor->endIndex;

  if (cursor->block == cursor->lastBlock) {
    // The last reference block will usually be a runt.
    cursor->endIndex = refCounts->blockCount;
  } else {
    cursor->endIndex += COUNTS_PER_BLOCK;
  }
  return true;
}

/**********************************************************************/
int makeRefCounts(PhysicalLayer        *layer,
                  BlockCount            blockCount,
                  Slab                 *slab,
                  PhysicalBlockNumber   origin,
                  ReadOnlyModeContext  *readOnlyContext,
                  RefCounts           **refCountsPtr)
{
  BlockCount  refBlockCount = getSavedReferenceCountSize(blockCount);
  RefCounts  *refCounts;
  int result = ALLOCATE_EXTENDED(RefCounts, refBlockCount, ReferenceBlock,
                                 "ref counts structure", &refCounts);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Allocate such that the runt slab has a full-length memory array,
  // plus a little padding so we can word-search even at the very end.
  size_t bytes = ((refBlockCount * COUNTS_PER_BLOCK) + (2 * BYTES_PER_WORD));
  result = ALLOCATE(bytes, ReferenceCount, "ref counts array",
                    &refCounts->counters);
  if (result != UDS_SUCCESS) {
    freeRefCounts(&refCounts);
    return result;
  }

  result = initializeEnqueueableCompletion(&refCounts->completion,
                                           REFERENCE_COUNTS_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    freeRefCounts(&refCounts);
    return result;
  }

  refCounts->slab                    = slab;
  refCounts->blockCount              = blockCount;
  refCounts->freeBlocks              = blockCount;
  refCounts->origin                  = origin;
  refCounts->referenceBlockCount     = refBlockCount;
  refCounts->readOnlyContext         = readOnlyContext;
  refCounts->statistics              = &slab->allocator->refCountStatistics;
  refCounts->searchCursor.firstBlock = &refCounts->blocks[0];
  refCounts->searchCursor.lastBlock  = &refCounts->blocks[refBlockCount - 1];
  resetSearchCursor(refCounts);

  for (size_t index = 0; index < refBlockCount; index++) {
    refCounts->blocks[index] = (ReferenceBlock) {
      .refCounts = refCounts,
    };
  }

  *refCountsPtr = refCounts;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeRefCounts(RefCounts **refCountsPtr)
{
  RefCounts *refCounts = *refCountsPtr;
  if (refCounts == NULL) {
    return;
  }

  destroyEnqueueable(&refCounts->completion);
  FREE(refCounts->counters);
  FREE(refCounts);
  *refCountsPtr = NULL;
}

/**********************************************************************/
RefCounts *asRefCounts(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(RefCounts, completion) == 0);
  assertCompletionType(completion->type, REFERENCE_COUNTS_COMPLETION);
  return (RefCounts *) completion;
}

/**
 * Mark a reference count block as dirty, potentially adding it to the dirty
 * queue if it wasn't already dirty.
 *
 * @param block  The reference block to mark as dirty
 *
 * @return  VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int dirtyBlock(ReferenceBlock *block)
{
  if (block->isDirty) {
    return VDO_SUCCESS;
  }

  block->isDirty = true;
  if (block->isWriting) {
    // The conclusion of the current write will enqueue the block again.
    return VDO_SUCCESS;
  }

  return enqueueWaiter(&block->refCounts->dirtyBlocks, &block->waiter);
}

/**********************************************************************/
BlockCount getUnreferencedBlockCount(RefCounts *refCounts)
{
  return refCounts->freeBlocks;
}

/**********************************************************************/
ReferenceBlock *getReferenceBlock(RefCounts *refCounts, SlabBlockNumber index)
{
  return &refCounts->blocks[index / COUNTS_PER_BLOCK];
}

/**
 * Get the reference counter that covers the given physical block number.
 *
 * @param [in]  refCounts       The refcounts object
 * @param [in]  pbn             The physical block number
 * @param [out] counterPtr      A pointer to the reference counter

 **/
static int getReferenceCounter(RefCounts            *refCounts,
                               PhysicalBlockNumber   pbn,
                               ReferenceCount      **counterPtr)
{
  SlabBlockNumber index;
  int result = slabBlockNumberFromPBN(refCounts->slab, pbn, &index);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *counterPtr = &refCounts->counters[index];

  return VDO_SUCCESS;
}

/**********************************************************************/
uint8_t getAvailableReferences(RefCounts *refCounts, PhysicalBlockNumber pbn)
{
  ReferenceCount *counterPtr = NULL;
  int result = getReferenceCounter(refCounts, pbn, &counterPtr);
  if (result != VDO_SUCCESS) {
    return 0;
  }

  if (*counterPtr == PROVISIONAL_REFERENCE_COUNT) {
    return (MAXIMUM_REFERENCE_COUNT - 1);
  }

  return (MAXIMUM_REFERENCE_COUNT - *counterPtr);
}

/**
 * Increment the reference count for a data block.
 *
 * @param [in]     refCounts          The refCounts responsible for the block
 * @param [in]     block              The reference block which contains the
 *                                    block being updated
 * @param [in]     slabBlockNumber    The block to update
 * @param [in]     oldStatus          The reference status of the data block
 *                                    before this increment
 * @param [in]     lock               The PBNLock associated with this
 *                                    increment (may be NULL)
 * @param [in,out] counterPtr         A pointer to the count for the data block
 * @param [out]    freeStatusChanged  A pointer which will be set to true if
 *                                    this update changed the free status of
 *                                    the block
 *
 * @return VDO_SUCCESS or an error
 **/
static int incrementForData(RefCounts       *refCounts,
                            ReferenceBlock  *block,
                            SlabBlockNumber  slabBlockNumber,
                            ReferenceStatus  oldStatus,
                            PBNLock         *lock,
                            ReferenceCount  *counterPtr,
                            bool            *freeStatusChanged)
{
  switch (oldStatus) {
  case RS_FREE:
    *counterPtr = 1;
    block->allocatedCount++;
    refCounts->freeBlocks--;
    *freeStatusChanged = true;
    break;

  case RS_PROVISIONAL:
    *counterPtr        = 1;
    *freeStatusChanged = false;
    break;

  default:
    // Single or shared
    if (*counterPtr >= MAXIMUM_REFERENCE_COUNT) {
      return logErrorWithStringError(VDO_REF_COUNT_INVALID,
                                     "Incrementing a block already having"
                                     " 254 references (slab %u, offset %"
                                     PRIu32 ")",
                                     refCounts->slab->slabNumber,
                                     slabBlockNumber);
    }
    (*counterPtr)++;
    *freeStatusChanged = false;
  }

  if (lock != NULL) {
    unassignProvisionalReference(lock);
  }
  return VDO_SUCCESS;
}

/**
 * Decrement the reference count for a data block.
 *
 * @param [in]     refCounts          The refCounts responsible for the block
 * @param [in]     block              The reference block which contains the
 *                                    block being updated
 * @param [in]     slabBlockNumber    The block to update
 * @param [in]     oldStatus          The reference status of the data block
 *                                    before this decrement
 * @param [in]     lock               The PBNLock associated with the block
 *                                    being decremented (may be NULL)
 * @param [in,out] counterPtr         A pointer to the count for the data block
 * @param [out]    freeStatusChanged  A pointer which will be set to true if
 *                                    this update changed the free status of
 *                                    the block
 *
 * @return VDO_SUCCESS or an error
 **/
static int decrementForData(RefCounts       *refCounts,
                            ReferenceBlock  *block,
                            SlabBlockNumber  slabBlockNumber,
                            ReferenceStatus  oldStatus,
                            PBNLock         *lock,
                            ReferenceCount  *counterPtr,
                            bool            *freeStatusChanged)
{
  switch (oldStatus) {
  case RS_FREE:
    return logErrorWithStringError(VDO_REF_COUNT_INVALID,
                                   "Decrementing free block at offset %"
                                   PRIu32 " in slab %u", slabBlockNumber,
                                   refCounts->slab->slabNumber);

  case RS_PROVISIONAL:
  case RS_SINGLE:
    if (lock != NULL) {
      // There is a read lock on this block, so the block must not become
      // unreferenced.
      *counterPtr        = PROVISIONAL_REFERENCE_COUNT;
      *freeStatusChanged = false;
      assignProvisionalReference(lock);
    } else {
      *counterPtr = EMPTY_REFERENCE_COUNT;
      block->allocatedCount--;
      refCounts->freeBlocks++;
      *freeStatusChanged = true;
    }
    break;

  default:
    // Shared
    (*counterPtr)--;
    *freeStatusChanged = false;
  }

  return VDO_SUCCESS;
}

/**
 * Increment the reference count for a block map page. All block map increments
 * should be from provisional to MAXIMUM_REFERENCE_COUNT. Since block map blocks
 * never dedupe they should never be adjusted from any other state. The
 * adjustment always results in MAXIMUM_REFERENCE_COUNT as this value is used to
 * prevent dedupe against block map blocks.
 *
 * @param [in]     refCounts          The refCounts responsible for the block
 * @param [in]     block              The reference block which contains the
 *                                    block being updated
 * @param [in]     slabBlockNumber    The block to update
 * @param [in]     oldStatus          The reference status of the block
 *                                    before this increment
 * @param [in]     lock               The PBNLock associated with this
 *                                    increment (may be NULL)
 * @param [in]     normalOperation    Whether we are in normal operation vs.
 *                                    recovery or rebuild
 * @param [in,out] counterPtr         A pointer to the count for the block
 * @param [out]    freeStatusChanged  A pointer which will be set to true if
 *                                    this update changed the free status of the
 *                                    block
 *
 * @return VDO_SUCCESS or an error
 **/
static int incrementForBlockMap(RefCounts       *refCounts,
                                ReferenceBlock  *block,
                                SlabBlockNumber  slabBlockNumber,
                                ReferenceStatus  oldStatus,
                                PBNLock         *lock,
                                bool             normalOperation,
                                ReferenceCount  *counterPtr,
                                bool            *freeStatusChanged)
{
  switch (oldStatus) {
  case RS_FREE:
    if (normalOperation) {
      return logErrorWithStringError(VDO_REF_COUNT_INVALID,
                                     "Incrementing unallocated block map block"
                                     " (slab %u, offset %" PRIu32 ")",
                                     refCounts->slab->slabNumber,
                                     slabBlockNumber);
    }

    *counterPtr = MAXIMUM_REFERENCE_COUNT;
    block->allocatedCount++;
    refCounts->freeBlocks--;
    *freeStatusChanged = true;
    return VDO_SUCCESS;

  case RS_PROVISIONAL:
    if (!normalOperation) {
      return logErrorWithStringError(VDO_REF_COUNT_INVALID,
                                     "Block map block had provisional "
                                     "reference during replay"
                                     " (slab %u, offset %" PRIu32 ")",
                                     refCounts->slab->slabNumber,
                                     slabBlockNumber);
    }

    *counterPtr        = MAXIMUM_REFERENCE_COUNT;
    *freeStatusChanged = false;
    if (lock != NULL) {
      unassignProvisionalReference(lock);
    }
    return VDO_SUCCESS;

  default:
    return logErrorWithStringError(VDO_REF_COUNT_INVALID,
                                   "Incrementing a block map block which is "
                                   "already referenced %u times (slab %u, "
                                   "offset %" PRIu32 ")",
                                   *counterPtr,
                                   refCounts->slab->slabNumber,
                                   slabBlockNumber);
  }
}

/**********************************************************************/
static void enterRefCountsReadOnlyMode(RefCounts *refCounts, int result);

/**
 * Update the reference count of a block.
 *
 * @param [in]  refCounts                The refCounts responsible for the
 *                                       block
 * @param [in]  block                    The reference block which contains the
 *                                       block being updated
 * @param [in]  slabBlockNumber          The block to update
 * @param [in]  slabJournalPoint         The slab journal point at which this
 *                                       update is journaled
 * @param [in]  operation                How to update the count
 * @param [in]  normalOperation          Whether we are in normal operation vs.
 *                                       recovery or rebuild
 * @param [out] freeStatusChanged        A pointer which will be set to true if
 *                                       this update changed the free status of
 *                                       the block
 * @param [out] provisionalDecrementPtr  A pointer which will be set to true if
 *                                       this update was a decrement of a
 *                                       provisional reference
 *
 * @return VDO_SUCCESS or an error
 **/
static int updateReferenceCount(RefCounts          *refCounts,
                                ReferenceBlock     *block,
                                SlabBlockNumber     slabBlockNumber,
                                const JournalPoint *slabJournalPoint,
                                ReferenceOperation  operation,
                                bool                normalOperation,
                                bool               *freeStatusChanged,
                                bool               *provisionalDecrementPtr)
{
  ReferenceCount  *counterPtr = &refCounts->counters[slabBlockNumber];
  ReferenceStatus  oldStatus  = referenceCountToStatus(*counterPtr);
  PBNLock         *lock       = getReferenceOperationPBNLock(operation);
  int result;

  switch (operation.type) {
  case DATA_INCREMENT:
    result = incrementForData(refCounts, block, slabBlockNumber, oldStatus,
                              lock, counterPtr, freeStatusChanged);
    break;

  case DATA_DECREMENT:
    result = decrementForData(refCounts, block, slabBlockNumber, oldStatus,
                              lock, counterPtr, freeStatusChanged);
    if ((result == VDO_SUCCESS) && (oldStatus == RS_PROVISIONAL)) {
      if (provisionalDecrementPtr != NULL) {
        *provisionalDecrementPtr = true;
      }
      return VDO_SUCCESS;
    }
    break;

  case BLOCK_MAP_INCREMENT:
    result = incrementForBlockMap(refCounts, block, slabBlockNumber, oldStatus,
                                  lock, normalOperation, counterPtr,
                                  freeStatusChanged);
    break;

  default:
    logError("Unknown reference count operation: %u", operation.type);
    enterRefCountsReadOnlyMode(refCounts, VDO_NOT_IMPLEMENTED);
    result = VDO_NOT_IMPLEMENTED;
  }

  if (result != VDO_SUCCESS) {
    return result;
  }

  if (isValidJournalPoint(slabJournalPoint)) {
    refCounts->slabJournalPoint = *slabJournalPoint;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int adjustReferenceCount(RefCounts          *refCounts,
                         ReferenceOperation  operation,
                         const JournalPoint *slabJournalPoint,
                         bool               *freeStatusChanged)
{
  if (refCounts->closeRequested) {
    return VDO_COMPONENT_BUSY;
  }

  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, operation.pbn,
                                      &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ReferenceBlock *block = getReferenceBlock(refCounts, slabBlockNumber);
  bool provisionalDecrement = false;
  result = updateReferenceCount(refCounts, block, slabBlockNumber,
                                slabJournalPoint, operation,
                                NORMAL_OPERATION, freeStatusChanged,
                                &provisionalDecrement);
  if ((result != VDO_SUCCESS) || provisionalDecrement) {
    return result;
  }

  if (block->isDirty && (block->slabJournalLock > 0)) {
    /*
     * This block is already dirty and a slab journal entry has been made
     * for it since the last time it was clean. We must release the per-entry
     * slab journal lock for the entry associated with the update we are now
     * doing.
     */
    result = ASSERT(isValidJournalPoint(slabJournalPoint),
                    "Reference count adjustments need slab journal points.");
    if (result != VDO_SUCCESS) {
      return result;
    }

    SequenceNumber entryLock = slabJournalPoint->sequenceNumber;
    adjustSlabJournalBlockReference(refCounts->slab->journal, entryLock, -1);
    return VDO_SUCCESS;
  }

  /*
   * This may be the first time we are applying an update for which there
   * is a slab journal entry to this block since the block was
   * cleaned. Therefore, we convert the per-entry slab journal lock to an
   * uncommitted reference block lock, if there is a per-entry lock.
   */
  if (isValidJournalPoint(slabJournalPoint)) {
    block->slabJournalLock = slabJournalPoint->sequenceNumber;
  } else {
    block->slabJournalLock = 0;
  }
  return dirtyBlock(block);
}

/**********************************************************************/
int adjustReferenceCountForRebuild(RefCounts           *refCounts,
                                   PhysicalBlockNumber  pbn,
                                   JournalOperation     operation)
{
  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, pbn, &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ReferenceBlock *block = getReferenceBlock(refCounts, slabBlockNumber);
  bool unusedFreeStatus;
  ReferenceOperation physicalOperation = {
    .type = operation,
  };
  result = updateReferenceCount(refCounts, block, slabBlockNumber, NULL,
                                physicalOperation, !NORMAL_OPERATION,
                                &unusedFreeStatus, NULL);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return dirtyBlock(block);
}

/**********************************************************************/
int replayReferenceCountChange(RefCounts          *refCounts,
                               const JournalPoint *entryPoint,
                               SlabJournalEntry    entry)
{
  ReferenceBlock *block = getReferenceBlock(refCounts, entry.sbn);
  SectorCount sector
    = (entry.sbn % COUNTS_PER_BLOCK) / COUNTS_PER_SECTOR;
  if (!beforeJournalPoint(&block->commitPoints[sector], entryPoint)) {
    // This entry is already reflected in the existing counts, so do nothing.
    return VDO_SUCCESS;
  }

  // This entry is not yet counted in the reference counts.
  bool unusedFreeStatus;
  ReferenceOperation operation = {
    .type = entry.operation
  };
  int result = updateReferenceCount(refCounts, block, entry.sbn,
                                    entryPoint, operation, !NORMAL_OPERATION,
                                    &unusedFreeStatus, NULL);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return dirtyBlock(block);
}

/**********************************************************************/
int getReferenceStatus(RefCounts           *refCounts,
                       PhysicalBlockNumber  pbn,
                       ReferenceStatus     *statusPtr)
{
  ReferenceCount *counterPtr = NULL;
  int result = getReferenceCounter(refCounts, pbn, &counterPtr);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *statusPtr = referenceCountToStatus(*counterPtr);
  return VDO_SUCCESS;
}

/**********************************************************************/
bool areEquivalentReferenceCounters(RefCounts *counterA, RefCounts *counterB)
{
  if ((counterA->blockCount             != counterB->blockCount)
      || (counterA->freeBlocks          != counterB->freeBlocks)
      || (counterA->referenceBlockCount != counterB->referenceBlockCount)) {
    return false;
  }

  for (size_t i = 0; i < counterA->referenceBlockCount; i++) {
    ReferenceBlock *blockA = &counterA->blocks[i];
    ReferenceBlock *blockB = &counterB->blocks[i];
    if (blockA->allocatedCount != blockB->allocatedCount) {
      return false;
    }
  }

  return (memcmp(counterA->counters, counterB->counters,
                 sizeof(ReferenceCount) * counterA->blockCount) == 0);
}

/**
 * Find the array index of the first zero byte in word-sized range of
 * reference counters. The search does no bounds checking; the function relies
 * on the array being sufficiently padded.
 *
 * @param wordPtr     A pointer to the eight counter bytes to check
 * @param startIndex  The array index corresponding to wordPtr[0]
 * @param failIndex   The array index to return if no zero byte is found

 * @return the array index of the first zero byte in the word, or
 *         the value passed as failIndex if no zero byte was found
 **/
static inline SlabBlockNumber findZeroByteInWord(const byte      *wordPtr,
                                                 SlabBlockNumber  startIndex,
                                                 SlabBlockNumber  failIndex)
{
  uint64_t word = getUInt64LE(wordPtr);

  // This looks like a loop, but GCC will unroll the eight iterations for us.
  for (unsigned int offset = 0; offset < BYTES_PER_WORD; offset++) {
    // Assumes little-endian byte order, which we have on X86.
    if ((word & 0xFF) == 0) {
      return (startIndex + offset);
    }
    word >>= 8;
  }

  return failIndex;
}

/**********************************************************************/
bool findFreeBlock(const RefCounts *refCounts,
                   SlabBlockNumber  startIndex,
                   SlabBlockNumber  endIndex,
                   SlabBlockNumber *indexPtr)
{
  SlabBlockNumber  zeroIndex;
  SlabBlockNumber  nextIndex   = startIndex;
  byte            *nextCounter = &refCounts->counters[nextIndex];
  byte            *endCounter  = &refCounts->counters[endIndex];

  // Search every byte of the first unaligned word. (Array is padded so
  // reading past end is safe.)
  zeroIndex = findZeroByteInWord(nextCounter, nextIndex, endIndex);
  if (zeroIndex < endIndex) {
    *indexPtr = zeroIndex;
    return true;
  }

  // On architectures where unaligned word access is expensive, this
  // would be a good place to advance to an alignment boundary.
  nextIndex   += BYTES_PER_WORD;
  nextCounter += BYTES_PER_WORD;

  // Now we're word-aligned; check an word at a time until we find a word
  // containing a zero. (Array is padded so reading past end is safe.)
  while (nextCounter < endCounter) {
    /*
     * The following code is currently an exact copy of the code preceding the
     * loop, but if you try to merge them by using a do loop, it runs slower
     * because a jump instruction gets added at the start of the iteration.
     */
    zeroIndex = findZeroByteInWord(nextCounter, nextIndex, endIndex);
    if (zeroIndex < endIndex) {
      *indexPtr = zeroIndex;
      return true;
    }

    nextIndex   += BYTES_PER_WORD;
    nextCounter += BYTES_PER_WORD;
  }

  return false;
}

/**
 * Search the reference block currently saved in the search cursor for a
 * reference count of zero, starting at the saved counter index.
 *
 * @param [in]  refCounts     The RefCounts object to search
 * @param [out] freeIndexPtr  A pointer to receive the array index of the
 *                            zero reference count
 *
 * @return true if an unreferenced counter was found
 **/
static bool searchCurrentReferenceBlock(const RefCounts *refCounts,
                                        SlabBlockNumber *freeIndexPtr)
{
  // Don't bother searching if the current block is known to be full.
  return ((refCounts->searchCursor.block->allocatedCount < COUNTS_PER_BLOCK)
          && findFreeBlock(refCounts, refCounts->searchCursor.index,
                           refCounts->searchCursor.endIndex, freeIndexPtr));
}

/**
 * Search each reference block for a reference count of zero, starting at the
 * reference block and counter index saved in the search cursor and searching
 * up to the end of the last reference block. The search does not wrap.
 *
 * @param [in]  refCounts     The RefCounts object to search
 * @param [out] freeIndexPtr  A pointer to receive the array index of the
 *                            zero reference count
 *
 * @return true if an unreferenced counter was found
 **/
static bool searchReferenceBlocks(RefCounts       *refCounts,
                                  SlabBlockNumber *freeIndexPtr)
{
  // Start searching at the saved search position in the current block.
  if (searchCurrentReferenceBlock(refCounts, freeIndexPtr)) {
    return true;
  }

  // Search each reference block up to the end of the slab.
  while (advanceSearchCursor(refCounts)) {
    if (searchCurrentReferenceBlock(refCounts, freeIndexPtr)) {
      return true;
    }
  }

  return false;
}

/**
 * Do the bookkeeping for making a provisional reference.
 *
 * @param refCounts        The RefCounts
 * @param slabBlockNumber  The block to reference
 **/
static void makeProvisionalReference(RefCounts       *refCounts,
                                     SlabBlockNumber  slabBlockNumber)
{
  // Make the initial transition from an unreferenced block to a provisionally
  // allocated block.
  refCounts->counters[slabBlockNumber] = PROVISIONAL_REFERENCE_COUNT;

  // Account for the allocation.
  ReferenceBlock *block = getReferenceBlock(refCounts, slabBlockNumber);
  block->allocatedCount++;
  refCounts->freeBlocks--;
}

/**********************************************************************/
int allocateUnreferencedBlock(RefCounts           *refCounts,
                              PhysicalBlockNumber *allocatedPtr)
{
  if (refCounts->closeRequested) {
    return VDO_COMPONENT_BUSY;
  }

  SlabBlockNumber freeIndex;
  if (!searchReferenceBlocks(refCounts, &freeIndex)) {
    return VDO_NO_SPACE;
  }

  ASSERT_LOG_ONLY((refCounts->counters[freeIndex] == EMPTY_REFERENCE_COUNT),
                  "free block must have refCount of zero");
  makeProvisionalReference(refCounts, freeIndex);

  // Update the search hint so the next search will start at the array
  // index just past the free block we just found.
  refCounts->searchCursor.index = (freeIndex + 1);

  *allocatedPtr = indexToPBN(refCounts, freeIndex);
  return VDO_SUCCESS;
}

/**********************************************************************/
int provisionallyReferenceBlock(RefCounts           *refCounts,
                                PhysicalBlockNumber  pbn,
                                PBNLock             *lock)
{
  if (refCounts->closeRequested) {
    return VDO_COMPONENT_BUSY;
  }

  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, pbn, &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (refCounts->counters[slabBlockNumber] == EMPTY_REFERENCE_COUNT) {
    makeProvisionalReference(refCounts, slabBlockNumber);
    if (lock != NULL) {
      assignProvisionalReference(lock);
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount countUnreferencedBlocks(RefCounts           *refCounts,
                                   PhysicalBlockNumber  startPBN,
                                   PhysicalBlockNumber  endPBN)
{
  BlockCount freeBlocks = 0;
  SlabBlockNumber   startIndex = pbnToIndex(refCounts, startPBN);
  SlabBlockNumber   endIndex   = pbnToIndex(refCounts, endPBN);
  for (SlabBlockNumber index = startIndex; index < endIndex; index++) {
    if (refCounts->counters[index] == EMPTY_REFERENCE_COUNT) {
      freeBlocks++;
    }
  }

  return freeBlocks;
}

/**
 * Convert a ReferenceBlock's generic wait queue entry back into the
 * ReferenceBlock.
 *
 * @param waiter        The wait queue entry to convert
 *
 * @return  The wrapping ReferenceBlock
 **/
static inline ReferenceBlock *waiterAsReferenceBlock(Waiter *waiter)
{
  STATIC_ASSERT(offsetof(ReferenceBlock, waiter) == 0);
  return (ReferenceBlock *) waiter;
}

/**
 * WaitCallback to clean dirty reference blocks when resetting.
 *
 * @param blockWaiter  The dirty block
 * @param context      Unused
 **/
static void
clearDirtyReferenceBlocks(Waiter *blockWaiter,
                          void   *context __attribute__((unused)))
{
  waiterAsReferenceBlock(blockWaiter)->isDirty = false;
}

/**********************************************************************/
void resetReferenceCounts(RefCounts *refCounts)
{
  // We can just use memset() since each ReferenceCount is exactly one byte.
  STATIC_ASSERT(sizeof(ReferenceCount) == 1);
  memset(refCounts->counters, 0, refCounts->blockCount);
  refCounts->freeBlocks       = refCounts->blockCount;
  refCounts->slabJournalPoint = (JournalPoint) {
    .sequenceNumber = 0,
    .entryCount     = 0,
  };

  for (size_t i = 0; i < refCounts->referenceBlockCount; i++) {
    refCounts->blocks[i].allocatedCount = 0;
  }

  notifyAllWaiters(&refCounts->dirtyBlocks, clearDirtyReferenceBlocks, NULL);
}

/**********************************************************************/
BlockCount getSavedReferenceCountSize(BlockCount blockCount)
{
  return computeBucketCount(blockCount, COUNTS_PER_BLOCK);
}

/**
 * Return whether a RefCounts has blocks requiring writing.
 *
 * @param refCounts  The RefCounts in question
 *
 * @return <code>true</code> if there are blocks which need writing
 **/
__attribute__((warn_unused_result))
static bool isRefCountsDirty(RefCounts *refCounts)
{
  return ((refCounts->activeCount > 0) || refCounts->updatingSlabSummary
          || hasWaiters(&refCounts->dirtyBlocks));
}

/**********************************************************************/
static void checkForIOComplete(RefCounts *refCounts)
{
  if (!refCounts->hasIOWaiter || isRefCountsDirty(refCounts)) {
    return;
  }

  refCounts->hasIOWaiter = false;
  finishCompletion(&refCounts->completion,
                   isReadOnly(refCounts->readOnlyContext)
                   ? VDO_READ_ONLY : VDO_SUCCESS);
}

/**
 * A waiter callback that resets the writing state of refCounts.
 **/
static void finishSummaryUpdate(Waiter *waiter,
                                void   *context __attribute__((unused)))
{
  RefCounts *refCounts           = refCountsFromWaiter(waiter);
  refCounts->updatingSlabSummary = false;
  checkForIOComplete(refCounts);
}

/**
 * Update slab summary that the RefCounts is clean.
 *
 * @param refCounts    The RefCounts object that is being written
 **/
static void updateSlabSummaryAsClean(RefCounts *refCounts)
{
  SlabSummaryZone *summary = getSlabSummaryZone(refCounts->slab->allocator);
  if (summary == NULL) {
    return;
  }

  // Update the slab summary to indicate this refCounts is clean.
  TailBlockOffset offset
    = getSummarizedTailBlockOffset(summary, refCounts->slab->slabNumber);
  refCounts->updatingSlabSummary        = true;
  refCounts->slabSummaryWaiter.callback = finishSummaryUpdate;
  updateSlabSummaryEntry(summary, &refCounts->slabSummaryWaiter,
                         refCounts->slab->slabNumber, offset, true, true,
                         getSlabFreeBlockCount(refCounts->slab));
}

/**********************************************************************/
static void enterRefCountsReadOnlyMode(RefCounts *refCounts, int result)
{
  enterReadOnlyMode(refCounts->readOnlyContext, result);
  checkForIOComplete(refCounts);
}

/**
 * Handle an I/O error reading or writing a reference count block.
 *
 * @param completion  The VIO doing the I/O as a completion
 **/
static void handleIOError(VDOCompletion *completion)
{
  int           result    = completion->result;
  VIOPoolEntry *entry     = completion->parent;
  RefCounts    *refCounts = ((ReferenceBlock *) entry->parent)->refCounts;
  returnVIO(refCounts->slab->allocator, entry);
  refCounts->activeCount--;
  enterRefCountsReadOnlyMode(refCounts, result);
}

/**
 * After a reference block has written, clean it, release its locks, and return
 * its VIO to the pool.
 *
 * @param completion  The VIO that just finished writing
 **/
static void finishReferenceBlockWrite(VDOCompletion *completion)
{
  VIOPoolEntry   *entry     = completion->parent;
  ReferenceBlock *block     = entry->parent;
  RefCounts      *refCounts = block->refCounts;
  refCounts->activeCount--;

  // Release the slab journal lock.
  adjustSlabJournalBlockReference(refCounts->slab->journal,
                                  block->slabJournalLockToRelease, -1);
  returnVIO(refCounts->slab->allocator, entry);

  /*
   * We can't clear the isWriting flag earlier as releasing the slab journal
   * lock may cause us to be dirtied again, but we don't want to double
   * enqueue.
   */
  block->isWriting = false;

  // Re-queue the block if it was re-dirtied while it was writing.
  if (block->isDirty && !isReadOnly(refCounts->readOnlyContext)) {
    int result = enqueueWaiter(&refCounts->dirtyBlocks, &block->waiter);
    if (result != VDO_SUCCESS) {
      // The enqueue should never fail.
      enterRefCountsReadOnlyMode(refCounts, result);
    }

    if (refCounts->hasIOWaiter) {
      // We must be saving, and this block will otherwise not be relaunched.
      saveDirtyReferenceBlocks(refCounts);
    }

    return;
  }

  if (isReadOnly(refCounts->readOnlyContext)) {
    checkForIOComplete(refCounts);
    return;
  }

  // Mark the RefCounts as clean in the slab summary if there are no dirty
  // or writing blocks and we aren't in read-only mode.
  if (!isRefCountsDirty(refCounts)) {
    updateSlabSummaryAsClean(refCounts);
  }
}

/**********************************************************************/
ReferenceCount *getReferenceCountersForBlock(ReferenceBlock *block)
{
  size_t blockIndex = block - block->refCounts->blocks;
  return &block->refCounts->counters[blockIndex * COUNTS_PER_BLOCK];
}

/**********************************************************************/
void packReferenceBlock(ReferenceBlock *block, void *buffer)
{
  PackedJournalPoint commitPoint;
  packJournalPoint(&block->refCounts->slabJournalPoint, &commitPoint);

  PackedReferenceBlock *packed   = buffer;
  ReferenceCount       *counters = getReferenceCountersForBlock(block);
  for (SectorCount i = 0; i < SECTORS_PER_BLOCK; i++) {
    packed->sectors[i].commitPoint = commitPoint;
    memcpy(packed->sectors[i].counts, counters + (i * COUNTS_PER_SECTOR),
           (sizeof(ReferenceCount) * COUNTS_PER_SECTOR));
  }
}

/**
 * After a dirty block waiter has gotten a VIO from the VIO pool, copy its
 * counters and associated data into the VIO, and launch the write.
 *
 * @param blockWaiter  The waiter of the dirty block
 * @param vioContext   The VIO returned by the pool
 **/
static void writeReferenceBlock(Waiter *blockWaiter, void *vioContext)
{
  VIOPoolEntry   *entry = vioContext;
  ReferenceBlock *block = waiterAsReferenceBlock(blockWaiter);
  packReferenceBlock(block, entry->buffer);

  size_t              blockOffset = (block - block->refCounts->blocks);
  PhysicalBlockNumber pbn         = (block->refCounts->origin + blockOffset);
  block->slabJournalLockToRelease = block->slabJournalLock;
  entry->parent                   = block;

  /*
   * Mark the block as clean, since we won't be committing any updates that
   * happen after this moment. As long as VIO order is preserved, two
   * VIOs updating this block at once will not cause complications.
   */
  block->isDirty = false;

  // Flush before writing to ensure that the recovery journal and slab journal
  // entries which cover this reference update are stable (VDO-2331).
  relaxedAdd64(&block->refCounts->statistics->blocksWritten, 1);
  entry->vio->completion.callbackThreadID
    = block->refCounts->slab->allocator->threadID;
  launchWriteMetadataVIOWithFlush(entry->vio, pbn, finishReferenceBlockWrite,
                                  handleIOError, true, false);
}

/**
 * Launch the write of a dirty reference block by first acquiring a VIO for it
 * from the pool. This can be asynchronous since the writer will have to wait
 * if all VIOs in the pool are currently in use.
 *
 * @param blockWaiter  The waiter of the block which is starting to write
 * @param context      The parent refCounts of the block
 **/
static void launchReferenceBlockWrite(Waiter *blockWaiter, void *context)
{
  RefCounts *refCounts = context;
  if (isReadOnly(refCounts->readOnlyContext)) {
    return;
  }

  refCounts->activeCount++;
  ReferenceBlock *block = waiterAsReferenceBlock(blockWaiter);
  block->isWriting      = true;
  blockWaiter->callback = writeReferenceBlock;
  int result = acquireVIO(refCounts->slab->allocator, blockWaiter);
  if (result != VDO_SUCCESS) {
    // This should never happen.
    refCounts->activeCount--;
    enterRefCountsReadOnlyMode(refCounts, result);
  }
}

/**********************************************************************/
void saveOldestReferenceBlock(RefCounts *refCounts)
{
  notifyNextWaiter(&refCounts->dirtyBlocks, launchReferenceBlockWrite,
                   refCounts);
}

/**********************************************************************/
void saveSeveralReferenceBlocks(RefCounts *refCounts, size_t flushDivisor)
{
  BlockCount dirtyBlockCount = countWaiters(&refCounts->dirtyBlocks);
  if (dirtyBlockCount == 0) {
    return;
  }

  BlockCount blocksToWrite = dirtyBlockCount / flushDivisor;
  // Always save at least one block.
  if (blocksToWrite == 0) {
    blocksToWrite = 1;
  }

  for (BlockCount written = 0; written < blocksToWrite; written++) {
    saveOldestReferenceBlock(refCounts);
  }
}

/**********************************************************************/
void saveDirtyReferenceBlocks(RefCounts *refCounts)
{
  notifyAllWaiters(&refCounts->dirtyBlocks, launchReferenceBlockWrite,
                   refCounts);
  checkForIOComplete(refCounts);
}

/**********************************************************************/
void saveReferenceBlocks(RefCounts     *refCounts,
                         VDOCompletion *parent,
                         VDOAction     *callback,
                         VDOAction     *errorHandler,
                         ThreadID       threadID)
{
  ASSERT_LOG_ONLY(!refCounts->hasIOWaiter,
                  "load or save not in progress on launching refCounts save");
  refCounts->hasIOWaiter = true;
  VDOCompletion *completion = &refCounts->completion;
  prepareCompletion(completion, callback, errorHandler, threadID, parent);
  saveDirtyReferenceBlocks(refCounts);
}

/**********************************************************************/
int dirtyAllReferenceBlocks(RefCounts *refCounts)
{
  for (BlockCount i = 0; i < refCounts->referenceBlockCount; i++) {
    int result = dirtyBlock(&refCounts->blocks[i]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
void saveAllReferenceBlocks(RefCounts     *refCounts,
                            VDOCompletion *parent,
                            VDOAction     *callback,
                            VDOAction     *errorHandler,
                            ThreadID       threadID)
{
  ASSERT_LOG_ONLY(!refCounts->hasIOWaiter,
                  "load or save not in progress on launching refCounts save");
  refCounts->hasIOWaiter = true;
  prepareCompletion(&refCounts->completion, callback, errorHandler, threadID,
                    parent);

  int result = dirtyAllReferenceBlocks(refCounts);
  if (result != VDO_SUCCESS) {
    finishCompletion(&refCounts->completion, result);
    return;
  }

  saveDirtyReferenceBlocks(refCounts);
}

/**********************************************************************/
void closeReferenceCounts(RefCounts     *refCounts,
                          VDOCompletion *parent,
                          VDOAction     *callback,
                          VDOAction     *errorHandler,
                          ThreadID       threadID)
{
  ASSERT_LOG_ONLY(!refCounts->hasIOWaiter,
                  "load or save not in progress on closing refCounts");
  refCounts->closeRequested = true;
  saveReferenceBlocks(refCounts, parent, callback, errorHandler, threadID);
}

/**
 * Clear the provisional reference counts from a reference block.
 *
 * @param block  The block to clear
 **/
static void clearProvisionalReferences(ReferenceBlock *block)
{
  ReferenceCount *counters = getReferenceCountersForBlock(block);
  for (BlockCount j = 0; j < COUNTS_PER_BLOCK; j++) {
    if (counters[j] == PROVISIONAL_REFERENCE_COUNT) {
      counters[j] = EMPTY_REFERENCE_COUNT;
      block->allocatedCount--;
    }
  }
}

/**
 * Unpack reference counts blocks into the internal memory structure.
 *
 * @param packed  The written reference block to be unpacked
 * @param block   The internal reference block to be loaded
 **/
static void unpackReferenceBlock(PackedReferenceBlock *packed,
                                 ReferenceBlock       *block)
{
  RefCounts      *refCounts    = block->refCounts;
  ReferenceCount *counters     = getReferenceCountersForBlock(block);
  for (SectorCount i = 0; i < SECTORS_PER_BLOCK; i++) {
    PackedReferenceSector *sector = &packed->sectors[i];
    unpackJournalPoint(&sector->commitPoint, &block->commitPoints[i]);
    memcpy(counters + (i * COUNTS_PER_SECTOR), sector->counts,
           (sizeof(ReferenceCount) * COUNTS_PER_SECTOR));
    // The slabJournalPoint must be the latest point found in any sector.
    if (beforeJournalPoint(&refCounts->slabJournalPoint,
                           &block->commitPoints[i])) {
      refCounts->slabJournalPoint = block->commitPoints[i];
    }

    if ((i > 0) && !areEquivalentJournalPoints(&block->commitPoints[0],
                                               &block->commitPoints[i])) {
      size_t blockIndex = block - block->refCounts->blocks;
      logWarning("Torn write detected in sector %u of reference block"
                 " %zu of slab %" PRIu16,
                 i, blockIndex, block->refCounts->slab->slabNumber);
    }
  }

  block->allocatedCount = 0;
  for (BlockCount i = 0; i < COUNTS_PER_BLOCK; i++) {
    if (counters[i] != EMPTY_REFERENCE_COUNT) {
      block->allocatedCount++;
    }
  }
}

/**
 * After a reference block has been read, unpack it.
 *
 * @param completion  The VIO that just finished reading
 **/
static void finishReferenceBlockLoad(VDOCompletion *completion)
{
  VIOPoolEntry   *entry = completion->parent;
  ReferenceBlock *block = entry->parent;
  unpackReferenceBlock((PackedReferenceBlock *) entry->buffer, block);

  RefCounts *refCounts = block->refCounts;
  returnVIO(refCounts->slab->allocator, entry);
  refCounts->activeCount--;
  clearProvisionalReferences(block);

  refCounts->freeBlocks -= block->allocatedCount;
  checkForIOComplete(block->refCounts);
}

/**
 * After a block waiter has gotten a VIO from the VIO pool, load the block.
 *
 * @param blockWaiter  The waiter of the block to load
 * @param vioContext   The VIO returned by the pool
 **/
static void loadReferenceBlock(Waiter *blockWaiter, void *vioContext)
{
  VIOPoolEntry        *entry       = vioContext;
  ReferenceBlock      *block       = waiterAsReferenceBlock(blockWaiter);
  size_t               blockOffset = (block - block->refCounts->blocks);
  PhysicalBlockNumber  pbn         = (block->refCounts->origin + blockOffset);
  entry->parent                    = block;

  // This must run on the same thread as the load thread since the slab journal
  // which shares our VIO pool is currently running on that thread too.
  entry->vio->completion.callbackThreadID
    = block->refCounts->completion.callbackThreadID;
  launchReadMetadataVIO(entry->vio, pbn, finishReferenceBlockLoad,
                        handleIOError);
}

/**********************************************************************/
void loadReferenceBlocks(RefCounts     *refCounts,
                         VDOCompletion *parent,
                         VDOAction     *callback,
                         VDOAction     *errorHandler,
                         ThreadID       threadID)
{
  ASSERT_LOG_ONLY(!refCounts->hasIOWaiter,
                  "load or save not in progress on launching refCounts load");

  refCounts->hasIOWaiter = true;
  refCounts->freeBlocks = refCounts->blockCount;
  prepareCompletion(&refCounts->completion, callback, errorHandler, threadID,
                    parent);
  refCounts->activeCount = refCounts->referenceBlockCount;
  for (BlockCount i = 0; i < refCounts->referenceBlockCount; i++) {
    Waiter *blockWaiter = &refCounts->blocks[i].waiter;
    blockWaiter->callback = loadReferenceBlock;
    int result = acquireVIO(refCounts->slab->allocator, blockWaiter);
    if (result != VDO_SUCCESS) {
      // This should never happen.
      refCounts->activeCount -= (refCounts->referenceBlockCount - i);
      enterRefCountsReadOnlyMode(refCounts, result);
      return;
    }
  }
}

/**********************************************************************/
int acquireDirtyBlockLocks(RefCounts *refCounts)
{
  int result = dirtyAllReferenceBlocks(refCounts);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (BlockCount i = 0; i < refCounts->referenceBlockCount; i++) {
    refCounts->blocks[i].slabJournalLock = 1;
  }

  adjustSlabJournalBlockReference(refCounts->slab->journal, 1,
                                  refCounts->referenceBlockCount);
  return VDO_SUCCESS;
}

/**********************************************************************/
void dumpRefCounts(const RefCounts *refCounts)
{
  // Terse because there are a lot of slabs to dump and syslog is lossy.
  logInfo("  refCounts: free=%" PRIu32 "/%" PRIu32 " blocks=%" PRIu32
          " dirty=%zu active=%zu journal@(%" PRIu64 ",%" PRIu16 ")%s",
          refCounts->freeBlocks, refCounts->blockCount,
          refCounts->referenceBlockCount,
          countWaiters(&refCounts->dirtyBlocks),
          refCounts->activeCount,
          refCounts->slabJournalPoint.sequenceNumber,
          refCounts->slabJournalPoint.entryCount,
          (refCounts->updatingSlabSummary ? " updating" : ""));
}
