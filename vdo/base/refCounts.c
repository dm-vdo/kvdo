/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/refCounts.c#26 $
 */

#include "refCounts.h"
#include "refCountsInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "completion.h"
#include "extent.h"
#include "header.h"
#include "journalPoint.h"
#include "numUtils.h"
#include "pbnLock.h"
#include "readOnlyNotifier.h"
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
 * Return the ref_counts from the ref_counts waiter.
 *
 * @param waiter  The waiter to convert
 *
 * @return  The ref_counts
 **/
__attribute__((warn_unused_result))
static inline struct ref_counts *refCountsFromWaiter(struct waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (struct ref_counts *)
    ((uintptr_t) waiter - offsetof(struct ref_counts, slabSummaryWaiter));
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
static PhysicalBlockNumber indexToPBN(const struct ref_counts *refCounts,
                                      uint64_t                 index)
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
static uint64_t pbnToIndex(const struct ref_counts *refCounts,
                           PhysicalBlockNumber      pbn)
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
 * @param refCounts  The ref_counts object containing the search cursor
 **/
static void resetSearchCursor(struct ref_counts *refCounts)
{
  struct search_cursor *cursor = &refCounts->searchCursor;

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
 * @param refCounts  The ref_counts object containing the search cursor
 *
 * @return true unless the cursor was at the last reference block
 **/
static bool advanceSearchCursor(struct ref_counts *refCounts)
{
  struct search_cursor *cursor = &refCounts->searchCursor;

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
int makeRefCounts(BlockCount                  blockCount,
                  struct vdo_slab            *slab,
                  PhysicalBlockNumber         origin,
                  struct read_only_notifier  *readOnlyNotifier,
                  struct ref_counts         **refCountsPtr)
{
  BlockCount  refBlockCount = getSavedReferenceCountSize(blockCount);
  struct ref_counts  *refCounts;
  int result = ALLOCATE_EXTENDED(struct ref_counts, refBlockCount,
                                 struct reference_block,
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

  refCounts->slab                    = slab;
  refCounts->blockCount              = blockCount;
  refCounts->freeBlocks              = blockCount;
  refCounts->origin                  = origin;
  refCounts->referenceBlockCount     = refBlockCount;
  refCounts->readOnlyNotifier        = readOnlyNotifier;
  refCounts->statistics              = &slab->allocator->ref_count_statistics;
  refCounts->searchCursor.firstBlock = &refCounts->blocks[0];
  refCounts->searchCursor.lastBlock  = &refCounts->blocks[refBlockCount - 1];
  resetSearchCursor(refCounts);

  size_t index;
  for (index = 0; index < refBlockCount; index++) {
    refCounts->blocks[index] = (struct reference_block) {
      .refCounts = refCounts,
    };
  }

  *refCountsPtr = refCounts;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeRefCounts(struct ref_counts **refCountsPtr)
{
  struct ref_counts *refCounts = *refCountsPtr;
  if (refCounts == NULL) {
    return;
  }

  FREE(refCounts->counters);
  FREE(refCounts);
  *refCountsPtr = NULL;
}

/**
 * Check whether a ref_counts object has active I/O.
 *
 * @param refCounts  The ref_counts to check
 *
 * @return <code>true</code> if there is reference block I/O or a summary
 *         update in progress
 **/
__attribute__((warn_unused_result))
static bool hasActiveIO(struct ref_counts *refCounts)
{
  return ((refCounts->activeCount > 0) || refCounts->updatingSlabSummary);
}

/**********************************************************************/
bool areRefCountsActive(struct ref_counts *refCounts)
{
  if (hasActiveIO(refCounts)) {
    return true;
  }

    // When not suspending or recovering, the refCounts must be clean.
  AdminStateCode code = refCounts->slab->state.state;
  return (hasWaiters(&refCounts->dirtyBlocks)
          && (code != ADMIN_STATE_SUSPENDING)
          && (code != ADMIN_STATE_RECOVERING));
}

/**********************************************************************/
static void enterRefCountsReadOnlyMode(struct ref_counts *refCounts, int result)
{
  enter_read_only_mode(refCounts->readOnlyNotifier, result);
  checkIfSlabDrained(refCounts->slab);
}

/**
 * Enqueue a block on the dirty queue.
 *
 * @param block  The block to enqueue
 **/
static void enqueueDirtyBlock(struct reference_block *block)
{
  int result = enqueueWaiter(&block->refCounts->dirtyBlocks, &block->waiter);
  if (result != VDO_SUCCESS) {
    // This should never happen.
    enterRefCountsReadOnlyMode(block->refCounts, result);
  }
}

/**
 * Mark a reference count block as dirty, potentially adding it to the dirty
 * queue if it wasn't already dirty.
 *
 * @param block  The reference block to mark as dirty
 **/
static void dirtyBlock(struct reference_block *block)
{
  if (block->isDirty) {
    return;
  }

  block->isDirty = true;
  if (block->isWriting) {
    // The conclusion of the current write will enqueue the block again.
    return;
  }

  enqueueDirtyBlock(block);
}

/**********************************************************************/
BlockCount getUnreferencedBlockCount(struct ref_counts *refCounts)
{
  return refCounts->freeBlocks;
}

/**********************************************************************/
struct reference_block *getReferenceBlock(struct ref_counts *refCounts,
                                          SlabBlockNumber    index)
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
static int getReferenceCounter(struct ref_counts    *refCounts,
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
uint8_t getAvailableReferences(struct ref_counts   *refCounts,
                               PhysicalBlockNumber  pbn)
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
 * @param [in]     lock               The pbn_lock associated with this
 *                                    increment (may be NULL)
 * @param [in,out] counterPtr         A pointer to the count for the data block
 * @param [out]    freeStatusChanged  A pointer which will be set to true if
 *                                    this update changed the free status of
 *                                    the block
 *
 * @return VDO_SUCCESS or an error
 **/
static int incrementForData(struct ref_counts      *refCounts,
                            struct reference_block *block,
                            SlabBlockNumber         slabBlockNumber,
                            ReferenceStatus         oldStatus,
                            struct pbn_lock        *lock,
                            ReferenceCount         *counterPtr,
                            bool                   *freeStatusChanged)
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
    unassign_provisional_reference(lock);
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
 * @param [in]     lock               The pbn_lock associated with the block
 *                                    being decremented (may be NULL)
 * @param [in,out] counterPtr         A pointer to the count for the data block
 * @param [out]    freeStatusChanged  A pointer which will be set to true if
 *                                    this update changed the free status of
 *                                    the block
 *
 * @return VDO_SUCCESS or an error
 **/
static int decrementForData(struct ref_counts      *refCounts,
                            struct reference_block *block,
                            SlabBlockNumber         slabBlockNumber,
                            ReferenceStatus         oldStatus,
                            struct pbn_lock        *lock,
                            ReferenceCount         *counterPtr,
                            bool                   *freeStatusChanged)
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
      assign_provisional_reference(lock);
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
 * @param [in]     lock               The pbn_lock associated with this
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
static int incrementForBlockMap(struct ref_counts      *refCounts,
                                struct reference_block *block,
                                SlabBlockNumber         slabBlockNumber,
                                ReferenceStatus         oldStatus,
                                struct pbn_lock        *lock,
                                bool                    normalOperation,
                                ReferenceCount         *counterPtr,
                                bool                   *freeStatusChanged)
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
      unassign_provisional_reference(lock);
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
static int
updateReferenceCount(struct ref_counts          *refCounts,
                     struct reference_block     *block,
                     SlabBlockNumber             slabBlockNumber,
                     const struct journal_point *slabJournalPoint,
                     struct reference_operation  operation,
                     bool                        normalOperation,
                     bool                       *freeStatusChanged,
                     bool                       *provisionalDecrementPtr)
{
  ReferenceCount  *counterPtr = &refCounts->counters[slabBlockNumber];
  ReferenceStatus  oldStatus  = referenceCountToStatus(*counterPtr);
  struct pbn_lock *lock       = getReferenceOperationPBNLock(operation);
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

  if (is_valid_journal_point(slabJournalPoint)) {
    refCounts->slabJournalPoint = *slabJournalPoint;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int adjustReferenceCount(struct ref_counts          *refCounts,
                         struct reference_operation  operation,
                         const struct journal_point *slabJournalPoint,
                         bool                       *freeStatusChanged)
{
  if (!isSlabOpen(refCounts->slab)) {
    return VDO_INVALID_ADMIN_STATE;
  }

  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, operation.pbn,
                                      &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct reference_block *block = getReferenceBlock(refCounts, slabBlockNumber);
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
    result = ASSERT(is_valid_journal_point(slabJournalPoint),
                    "Reference count adjustments need slab journal points.");
    if (result != VDO_SUCCESS) {
      return result;
    }

    SequenceNumber entryLock = slabJournalPoint->sequence_number;
    adjustSlabJournalBlockReference(refCounts->slab->journal, entryLock, -1);
    return VDO_SUCCESS;
  }

  /*
   * This may be the first time we are applying an update for which there
   * is a slab journal entry to this block since the block was
   * cleaned. Therefore, we convert the per-entry slab journal lock to an
   * uncommitted reference block lock, if there is a per-entry lock.
   */
  if (is_valid_journal_point(slabJournalPoint)) {
    block->slabJournalLock = slabJournalPoint->sequence_number;
  } else {
    block->slabJournalLock = 0;
  }

  dirtyBlock(block);
  return VDO_SUCCESS;
}

/**********************************************************************/
int adjustReferenceCountForRebuild(struct ref_counts   *refCounts,
                                   PhysicalBlockNumber  pbn,
                                   JournalOperation     operation)
{
  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, pbn, &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct reference_block *block = getReferenceBlock(refCounts, slabBlockNumber);
  bool unusedFreeStatus;
  struct reference_operation physicalOperation = {
    .type = operation,
  };
  result = updateReferenceCount(refCounts, block, slabBlockNumber, NULL,
                                physicalOperation, !NORMAL_OPERATION,
                                &unusedFreeStatus, NULL);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dirtyBlock(block);
  return VDO_SUCCESS;
}

/**********************************************************************/
int replayReferenceCountChange(struct ref_counts          *refCounts,
                               const struct journal_point *entryPoint,
                               struct slab_journal_entry   entry)
{
  struct reference_block *block = getReferenceBlock(refCounts, entry.sbn);
  SectorCount sector
    = (entry.sbn % COUNTS_PER_BLOCK) / COUNTS_PER_SECTOR;
  if (!before_journal_point(&block->commitPoints[sector], entryPoint)) {
    // This entry is already reflected in the existing counts, so do nothing.
    return VDO_SUCCESS;
  }

  // This entry is not yet counted in the reference counts.
  bool unusedFreeStatus;
  struct reference_operation operation = {
    .type = entry.operation
  };
  int result = updateReferenceCount(refCounts, block, entry.sbn,
                                    entryPoint, operation, !NORMAL_OPERATION,
                                    &unusedFreeStatus, NULL);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dirtyBlock(block);
  return VDO_SUCCESS;
}

/**********************************************************************/
int getReferenceStatus(struct ref_counts   *refCounts,
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
bool areEquivalentReferenceCounters(struct ref_counts *counterA,
                                    struct ref_counts *counterB)
{
  if ((counterA->blockCount             != counterB->blockCount)
      || (counterA->freeBlocks          != counterB->freeBlocks)
      || (counterA->referenceBlockCount != counterB->referenceBlockCount)) {
    return false;
  }

  size_t i;
  for (i = 0; i < counterA->referenceBlockCount; i++) {
    struct reference_block *blockA = &counterA->blocks[i];
    struct reference_block *blockB = &counterB->blocks[i];
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
  unsigned int offset;
  for (offset = 0; offset < BYTES_PER_WORD; offset++) {
    // Assumes little-endian byte order, which we have on X86.
    if ((word & 0xFF) == 0) {
      return (startIndex + offset);
    }
    word >>= 8;
  }

  return failIndex;
}

/**********************************************************************/
bool findFreeBlock(const struct ref_counts *refCounts,
                   SlabBlockNumber          startIndex,
                   SlabBlockNumber          endIndex,
                   SlabBlockNumber         *indexPtr)
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
 * @param [in]  refCounts     The ref_counts object to search
 * @param [out] freeIndexPtr  A pointer to receive the array index of the
 *                            zero reference count
 *
 * @return true if an unreferenced counter was found
 **/
static bool searchCurrentReferenceBlock(const struct ref_counts *refCounts,
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
 * @param [in]  refCounts     The ref_counts object to search
 * @param [out] freeIndexPtr  A pointer to receive the array index of the
 *                            zero reference count
 *
 * @return true if an unreferenced counter was found
 **/
static bool searchReferenceBlocks(struct ref_counts *refCounts,
                                  SlabBlockNumber   *freeIndexPtr)
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
 * @param refCounts        The ref_counts
 * @param slabBlockNumber  The block to reference
 **/
static void makeProvisionalReference(struct ref_counts *refCounts,
                                     SlabBlockNumber    slabBlockNumber)
{
  // Make the initial transition from an unreferenced block to a provisionally
  // allocated block.
  refCounts->counters[slabBlockNumber] = PROVISIONAL_REFERENCE_COUNT;

  // Account for the allocation.
  struct reference_block *block = getReferenceBlock(refCounts, slabBlockNumber);
  block->allocatedCount++;
  refCounts->freeBlocks--;
}

/**********************************************************************/
int allocateUnreferencedBlock(struct ref_counts   *refCounts,
                              PhysicalBlockNumber *allocatedPtr)
{
  if (!isSlabOpen(refCounts->slab)) {
    return VDO_INVALID_ADMIN_STATE;
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
int provisionallyReferenceBlock(struct ref_counts   *refCounts,
                                PhysicalBlockNumber  pbn,
                                struct pbn_lock     *lock)
{
  if (!isSlabOpen(refCounts->slab)) {
    return VDO_INVALID_ADMIN_STATE;
  }

  SlabBlockNumber slabBlockNumber;
  int result = slabBlockNumberFromPBN(refCounts->slab, pbn, &slabBlockNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (refCounts->counters[slabBlockNumber] == EMPTY_REFERENCE_COUNT) {
    makeProvisionalReference(refCounts, slabBlockNumber);
    if (lock != NULL) {
      assign_provisional_reference(lock);
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount countUnreferencedBlocks(struct ref_counts   *refCounts,
                                   PhysicalBlockNumber  startPBN,
                                   PhysicalBlockNumber  endPBN)
{
  BlockCount freeBlocks = 0;
  SlabBlockNumber   startIndex = pbnToIndex(refCounts, startPBN);
  SlabBlockNumber   endIndex   = pbnToIndex(refCounts, endPBN);
  SlabBlockNumber   index;
  for (index = startIndex; index < endIndex; index++) {
    if (refCounts->counters[index] == EMPTY_REFERENCE_COUNT) {
      freeBlocks++;
    }
  }

  return freeBlocks;
}

/**
 * Convert a reference_block's generic wait queue entry back into the
 * reference_block.
 *
 * @param waiter        The wait queue entry to convert
 *
 * @return  The wrapping reference_block
 **/
static inline struct reference_block *
waiterAsReferenceBlock(struct waiter *waiter)
{
  STATIC_ASSERT(offsetof(struct reference_block, waiter) == 0);
  return (struct reference_block *) waiter;
}

/**
 * WaitCallback to clean dirty reference blocks when resetting.
 *
 * @param blockWaiter  The dirty block
 * @param context      Unused
 **/
static void
clearDirtyReferenceBlocks(struct waiter *blockWaiter,
                          void          *context __attribute__((unused)))
{
  waiterAsReferenceBlock(blockWaiter)->isDirty = false;
}

/**********************************************************************/
void resetReferenceCounts(struct ref_counts *refCounts)
{
  // We can just use memset() since each ReferenceCount is exactly one byte.
  STATIC_ASSERT(sizeof(ReferenceCount) == 1);
  memset(refCounts->counters, 0, refCounts->blockCount);
  refCounts->freeBlocks       = refCounts->blockCount;
  refCounts->slabJournalPoint = (struct journal_point) {
    .sequence_number = 0,
    .entry_count     = 0,
  };

  size_t i;
  for (i = 0; i < refCounts->referenceBlockCount; i++) {
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
 * A waiter callback that resets the writing state of refCounts.
 **/
static void finishSummaryUpdate(struct waiter *waiter, void *context)
{
  struct ref_counts *refCounts   = refCountsFromWaiter(waiter);
  refCounts->updatingSlabSummary = false;

  int result = *((int *) context);
  if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
    checkIfSlabDrained(refCounts->slab);
    return;
  }

  logErrorWithStringError(result, "failed to update slab summary");
  enterRefCountsReadOnlyMode(refCounts, result);
}

/**
 * Update slab summary that the ref_counts object is clean.
 *
 * @param refCounts    The ref_counts object that is being written
 **/
static void updateSlabSummaryAsClean(struct ref_counts *refCounts)
{
  struct slab_summary_zone *summary
    = get_slab_summary_zone(refCounts->slab->allocator);
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

/**
 * Handle an I/O error reading or writing a reference count block.
 *
 * @param completion  The VIO doing the I/O as a completion
 **/
static void handleIOError(struct vdo_completion *completion)
{
  int                    result    = completion->result;
  struct vio_pool_entry *entry     = completion->parent;
  struct ref_counts     *refCounts
    = ((struct reference_block *) entry->parent)->refCounts;
  return_vio(refCounts->slab->allocator, entry);
  refCounts->activeCount--;
  enterRefCountsReadOnlyMode(refCounts, result);
}

/**
 * After a reference block has written, clean it, release its locks, and return
 * its VIO to the pool.
 *
 * @param completion  The VIO that just finished writing
 **/
static void finishReferenceBlockWrite(struct vdo_completion *completion)
{
  struct vio_pool_entry           *entry     = completion->parent;
  struct reference_block          *block     = entry->parent;
  struct ref_counts               *refCounts = block->refCounts;
  refCounts->activeCount--;

  // Release the slab journal lock.
  adjustSlabJournalBlockReference(refCounts->slab->journal,
                                  block->slabJournalLockToRelease, -1);
  return_vio(refCounts->slab->allocator, entry);

  /*
   * We can't clear the isWriting flag earlier as releasing the slab journal
   * lock may cause us to be dirtied again, but we don't want to double
   * enqueue.
   */
  block->isWriting = false;

  if (is_read_only(refCounts->readOnlyNotifier)) {
    checkIfSlabDrained(refCounts->slab);
    return;
  }

  // Re-queue the block if it was re-dirtied while it was writing.
  if (block->isDirty) {
    enqueueDirtyBlock(block);
    if (isSlabDraining(refCounts->slab)) {
      // We must be saving, and this block will otherwise not be relaunched.
      saveDirtyReferenceBlocks(refCounts);
    }

    return;
  }

  // Mark the ref_counts as clean in the slab summary if there are no dirty
  // or writing blocks and no summary update in progress.
  if (!hasActiveIO(refCounts) && !hasWaiters(&refCounts->dirtyBlocks)) {
    updateSlabSummaryAsClean(refCounts);
  }
}

/**********************************************************************/
ReferenceCount *getReferenceCountersForBlock(struct reference_block *block)
{
  size_t blockIndex = block - block->refCounts->blocks;
  return &block->refCounts->counters[blockIndex * COUNTS_PER_BLOCK];
}

/**********************************************************************/
void packReferenceBlock(struct reference_block *block, void *buffer)
{
  struct packed_journal_point commitPoint;
  pack_journal_point(&block->refCounts->slabJournalPoint, &commitPoint);

  struct packed_reference_block *packed = buffer;
  ReferenceCount *counters = getReferenceCountersForBlock(block);
  SectorCount i;
  for (i = 0; i < SECTORS_PER_BLOCK; i++) {
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
static void writeReferenceBlock(struct waiter *blockWaiter, void *vioContext)
{
  struct vio_pool_entry           *entry = vioContext;
  struct reference_block          *block = waiterAsReferenceBlock(blockWaiter);
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
    = block->refCounts->slab->allocator->thread_id;
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
static void launchReferenceBlockWrite(struct waiter *blockWaiter, void *context)
{
  struct ref_counts *refCounts = context;
  if (is_read_only(refCounts->readOnlyNotifier)) {
    return;
  }

  refCounts->activeCount++;
  struct reference_block *block = waiterAsReferenceBlock(blockWaiter);
  block->isWriting              = true;
  blockWaiter->callback         = writeReferenceBlock;
  int result = acquire_vio(refCounts->slab->allocator, blockWaiter);
  if (result != VDO_SUCCESS) {
    // This should never happen.
    refCounts->activeCount--;
    enterRefCountsReadOnlyMode(refCounts, result);
  }
}

/**********************************************************************/
void saveOldestReferenceBlock(struct ref_counts *refCounts)
{
  notifyNextWaiter(&refCounts->dirtyBlocks, launchReferenceBlockWrite,
                   refCounts);
}

/**********************************************************************/
void saveSeveralReferenceBlocks(struct ref_counts *refCounts,
                                size_t             flushDivisor)
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

  BlockCount written;
  for (written = 0; written < blocksToWrite; written++) {
    saveOldestReferenceBlock(refCounts);
  }
}

/**********************************************************************/
void saveDirtyReferenceBlocks(struct ref_counts *refCounts)
{
  notifyAllWaiters(&refCounts->dirtyBlocks, launchReferenceBlockWrite,
                   refCounts);
  checkIfSlabDrained(refCounts->slab);
}

/**********************************************************************/
void dirtyAllReferenceBlocks(struct ref_counts *refCounts)
{
  BlockCount i;
  for (i = 0; i < refCounts->referenceBlockCount; i++) {
    dirtyBlock(&refCounts->blocks[i]);
  }
}

/**
 * Clear the provisional reference counts from a reference block.
 *
 * @param block  The block to clear
 **/
static void clearProvisionalReferences(struct reference_block *block)
{
  ReferenceCount *counters = getReferenceCountersForBlock(block);
  BlockCount j;
  for (j = 0; j < COUNTS_PER_BLOCK; j++) {
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
static void unpackReferenceBlock(struct packed_reference_block *packed,
                                 struct reference_block        *block)
{
  struct ref_counts *refCounts    = block->refCounts;
  ReferenceCount    *counters     = getReferenceCountersForBlock(block);
  SectorCount i;
  for (i = 0; i < SECTORS_PER_BLOCK; i++) {
    struct packed_reference_sector *sector = &packed->sectors[i];
    unpack_journal_point(&sector->commitPoint, &block->commitPoints[i]);
    memcpy(counters + (i * COUNTS_PER_SECTOR), sector->counts,
           (sizeof(ReferenceCount) * COUNTS_PER_SECTOR));
    // The slabJournalPoint must be the latest point found in any sector.
    if (before_journal_point(&refCounts->slabJournalPoint,
                             &block->commitPoints[i])) {
      refCounts->slabJournalPoint = block->commitPoints[i];
    }

    if ((i > 0) && !are_equivalent_journal_points(&block->commitPoints[0],
                                                  &block->commitPoints[i])) {
      size_t blockIndex = block - block->refCounts->blocks;
      logWarning("Torn write detected in sector %u of reference block"
                 " %zu of slab %" PRIu16,
                 i, blockIndex, block->refCounts->slab->slabNumber);
    }
  }

  block->allocatedCount = 0;
  BlockCount index;
  for (index = 0; index < COUNTS_PER_BLOCK; index++) {
    if (counters[index] != EMPTY_REFERENCE_COUNT) {
      block->allocatedCount++;
    }
  }
}

/**
 * After a reference block has been read, unpack it.
 *
 * @param completion  The VIO that just finished reading
 **/
static void finishReferenceBlockLoad(struct vdo_completion *completion)
{
  struct vio_pool_entry  *entry = completion->parent;
  struct reference_block *block = entry->parent;
  unpackReferenceBlock((struct packed_reference_block *) entry->buffer, block);

  struct ref_counts *refCounts = block->refCounts;
  return_vio(refCounts->slab->allocator, entry);
  refCounts->activeCount--;
  clearProvisionalReferences(block);

  refCounts->freeBlocks -= block->allocatedCount;
  checkIfSlabDrained(block->refCounts->slab);
}

/**
 * After a block waiter has gotten a VIO from the VIO pool, load the block.
 *
 * @param blockWaiter  The waiter of the block to load
 * @param vioContext   The VIO returned by the pool
 **/
static void loadReferenceBlock(struct waiter *blockWaiter, void *vioContext)
{
  struct vio_pool_entry  *entry       = vioContext;
  struct reference_block *block       = waiterAsReferenceBlock(blockWaiter);
  size_t                  blockOffset = (block - block->refCounts->blocks);
  PhysicalBlockNumber     pbn         = (block->refCounts->origin + blockOffset);
  entry->parent                       = block;

  entry->vio->completion.callbackThreadID
    = block->refCounts->slab->allocator->thread_id;
  launchReadMetadataVIO(entry->vio, pbn, finishReferenceBlockLoad,
                        handleIOError);
}

/**
 * Load reference blocks from the underlying storage into a pre-allocated
 * reference counter.
 *
 * @param refCounts  The reference counter to be loaded
 **/
static void loadReferenceBlocks(struct ref_counts *refCounts)
{
  refCounts->freeBlocks  = refCounts->blockCount;
  refCounts->activeCount = refCounts->referenceBlockCount;
  BlockCount i;
  for (i = 0; i < refCounts->referenceBlockCount; i++) {
    struct waiter *blockWaiter = &refCounts->blocks[i].waiter;
    blockWaiter->callback = loadReferenceBlock;
    int result = acquire_vio(refCounts->slab->allocator, blockWaiter);
    if (result != VDO_SUCCESS) {
      // This should never happen.
      refCounts->activeCount -= (refCounts->referenceBlockCount - i);
      enterRefCountsReadOnlyMode(refCounts, result);
      return;
    }
  }
}

/**********************************************************************/
void drainRefCounts(struct ref_counts *refCounts)
{
  struct vdo_slab *slab = refCounts->slab;
  bool  save = false;
  switch (slab->state.state) {
  case ADMIN_STATE_SCRUBBING:
    if (mustLoadRefCounts(slab->allocator->summary, slab->slabNumber)) {
      loadReferenceBlocks(refCounts);
      return;
    }

    break;

  case ADMIN_STATE_SAVE_FOR_SCRUBBING:
    if (!mustLoadRefCounts(slab->allocator->summary, slab->slabNumber)) {
      // These reference counts were never written, so mark them all dirty.
      dirtyAllReferenceBlocks(refCounts);
    }
    save = true;
    break;

  case ADMIN_STATE_REBUILDING:
    if (shouldSaveFullyBuiltSlab(slab)) {
      dirtyAllReferenceBlocks(refCounts);
      save = true;
    }
    break;

  case ADMIN_STATE_SAVING:
    save = !isUnrecoveredSlab(slab);
    break;

  case ADMIN_STATE_RECOVERING:
  case ADMIN_STATE_SUSPENDING:
    break;

  default:
    notifyRefCountsAreDrained(slab, VDO_SUCCESS);
    return;
  }

  if (save) {
    saveDirtyReferenceBlocks(refCounts);
  }
}

/**********************************************************************/
void acquireDirtyBlockLocks(struct ref_counts *refCounts)
{
  dirtyAllReferenceBlocks(refCounts);
  BlockCount i;
  for (i = 0; i < refCounts->referenceBlockCount; i++) {
    refCounts->blocks[i].slabJournalLock = 1;
  }

  adjustSlabJournalBlockReference(refCounts->slab->journal, 1,
                                  refCounts->referenceBlockCount);
}

/**********************************************************************/
void dumpRefCounts(const struct ref_counts *refCounts)
{
  // Terse because there are a lot of slabs to dump and syslog is lossy.
  logInfo("  refCounts: free=%" PRIu32 "/%" PRIu32 " blocks=%" PRIu32
          " dirty=%zu active=%zu journal@(%llu,%" PRIu16 ")%s",
          refCounts->freeBlocks, refCounts->blockCount,
          refCounts->referenceBlockCount,
          countWaiters(&refCounts->dirtyBlocks),
          refCounts->activeCount,
          refCounts->slabJournalPoint.sequence_number,
          refCounts->slabJournalPoint.entry_count,
          (refCounts->updatingSlabSummary ? " updating" : ""));
}
