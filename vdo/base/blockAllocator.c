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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockAllocator.c#3 $
 */

#include "blockAllocatorInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "heap.h"
#include "numUtils.h"
#include "priorityTable.h"
#include "refCounts.h"
#include "slab.h"
#include "slabCompletion.h"
#include "slabDepotInternals.h"
#include "slabIterator.h"
#include "slabJournalInternals.h"
#include "slabScrubber.h"
#include "slabSummary.h"
#include "vio.h"
#include "vioPool.h"

/**
 * Assert that a block allocator function was called from the correct thread.
 *
 * @param threadID      The allocator's thread id
 * @param functionName  The name of the function
 **/
static inline void assertOnAllocatorThread(ThreadID    threadID,
                                           const char *functionName)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == threadID),
                  "%s called on correct thread", functionName);
}

/**
 * Get the priority for a slab in the allocator's slab queue. Slabs are
 * essentially prioritized by an approximation of the number of free blocks in
 * the slab so slabs with lots of free blocks with be opened for allocation
 * before slabs that have few free blocks.
 *
 * @param slab  The slab whose queue priority is desired
 *
 * @return the queue priority of the slab
 **/
static unsigned int calculateSlabPriority(Slab *slab)
{
  BlockCount freeBlocks = getSlabFreeBlockCount(slab);

  // Slabs that are completely full must be the only ones with the lowest
  // priority: zero.
  if (freeBlocks == 0) {
    return 0;
  }

  /*
   * Slabs that have never been opened (empty, newly initialized, never been
   * written to) have lower priority than previously opened slabs that have a
   * signficant number of free blocks. This ranking causes VDO to avoid
   * writing physical blocks for the first time until there are very few free
   * blocks that have been previously written to. That policy makes VDO a
   * better client of any underlying storage that is thinly-provisioned
   * [VDOSTORY-123].
   */
  unsigned int unopenedSlabPriority = slab->allocator->unopenedSlabPriority;
  if (isSlabJournalBlank(slab->journal)) {
    return unopenedSlabPriority;
  }

  /*
   * For all other slabs, the priority is derived from the logarithm of the
   * number of free blocks. Slabs with the same order of magnitude of free
   * blocks have the same priority. With 2^23 blocks, the priority will range
   * from 1 to 25. The reserved unopenedSlabPriority divides the range and is
   * skipped by the logarithmic mapping.
   */
  unsigned int priority = (1 + logBaseTwo(freeBlocks));
  return ((priority < unopenedSlabPriority) ? priority : priority + 1);
}

/**
 * Add a slab to the priority queue of slabs available for allocation.
 *
 * @param slab  The slab to prioritize
 **/
static void prioritizeSlab(Slab *slab)
{
  ASSERT_LOG_ONLY(isRingEmpty(&slab->ringNode),
                  "a slab must not already be on a ring when prioritizing");
  slab->priority = calculateSlabPriority(slab);
  priorityTableEnqueue(slab->allocator->prioritizedSlabs, slab->priority,
                       &slab->ringNode);
}

/**********************************************************************/
void registerSlabWithAllocator(BlockAllocator *allocator,
                               Slab           *slab,
			       bool            resizing)
{
  allocator->slabCount++;
  allocator->lastSlab = slab->slabNumber;
  if (resizing) {
    prioritizeSlab(slab);
  }
}

/**
 * Get an iterator over all the slabs in the allocator.
 *
 * @param allocator  The allocator
 *
 * @return An iterator over the allocator's slabs
 **/
static SlabIterator getSlabIterator(const BlockAllocator *allocator)
{
  return iterateSlabs(allocator->depot->slabs, allocator->lastSlab,
                      allocator->zoneNumber, allocator->depot->zoneCount);
}

/**********************************************************************/
int makeAllocatorPoolVIOs(PhysicalLayer  *layer,
                          void           *parent,
                          void           *buffer,
                          VIO           **vioPtr)
{
  return createVIO(layer, VIO_TYPE_SLAB_JOURNAL, VIO_PRIORITY_METADATA, parent,
                   buffer, vioPtr);
}

/**
 * Allocate those component of the block allocator which are needed only at
 * load time, not at format time.
 *
 * @param allocator           The allocator
 * @param layer               The physical layer below this allocator
 * @param vioPoolSize         The VIO pool size
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocateComponents(BlockAllocator  *allocator,
                              PhysicalLayer   *layer,
                              BlockCount       vioPoolSize)
{
  /*
   * If createVIO is NULL, the block allocator is only being used to format
   * or audit the VDO. These only require the SuperBlock component, so we can
   * just skip allocating all the memory needed for runtime components.
   */
  if (layer->createMetadataVIO == NULL) {
    return VDO_SUCCESS;
  }

  SlabDepot *depot = allocator->depot;
  int result = initializeEnqueueableCompletion(&allocator->completion,
                                               BLOCK_ALLOCATOR_COMPLETION,
                                               layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  allocator->summary = getSlabSummaryForZone(depot, allocator->zoneNumber);

  result = makeVIOPool(layer, vioPoolSize, makeAllocatorPoolVIOs, NULL,
                       &allocator->vioPool);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeSlabCompletion(layer, &allocator->slabCompletion);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount slabJournalSize = depot->slabConfig.slabJournalBlocks;
  result = makeSlabScrubber(layer, slabJournalSize, allocator->readOnlyContext,
                            &allocator->slabScrubber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // The number of data blocks is the maximum number of free blocks that could
  // be used in calculateSlabPriority().
  BlockCount maxFreeBlocks = depot->slabConfig.dataBlocks;
  unsigned int maxPriority = (2 + logBaseTwo(maxFreeBlocks));
  result = makePriorityTable(maxPriority, &allocator->prioritizedSlabs);
  if (result != VDO_SUCCESS) {
    return result;
  }

  /*
   * VDOSTORY-123 requires that we try to open slabs that already have
   * allocated blocks in preference to slabs that have never been opened. For
   * reasons we have not been able to fully understand, performance tests on
   * SSD harvards have been very sensitive (50% reduction in test throughput)
   * to very slight differences in the timing and locality of block
   * allocation. Assigning a low priority to unopened slabs (maxPriority/2,
   * say) would be ideal for the story, but anything less than a very high
   * threshold (maxPriority - 1) hurts PMI results.
   *
   * This sets the free block threshold for preferring to open an unopened
   * slab to the binary floor of 3/4ths the total number of datablocks in a
   * slab, which will generally evaluate to about half the slab size, but
   * avoids degenerate behavior in unit tests where the number of data blocks
   * is artificially constrained to a power of two.
   */
  allocator->unopenedSlabPriority = (1 + logBaseTwo((maxFreeBlocks * 3) / 4));

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeBlockAllocator(SlabDepot            *depot,
                       ZoneCount             zoneNumber,
                       ThreadID              threadID,
                       Nonce                 nonce,
                       BlockCount            vioPoolSize,
                       PhysicalLayer        *layer,
                       ReadOnlyModeContext  *readOnlyContext,
                       BlockAllocator      **allocatorPtr)
{

  BlockAllocator *allocator;
  int result = ALLOCATE(1, BlockAllocator, __func__, &allocator);
  if (result != VDO_SUCCESS) {
    return result;
  }

  allocator->depot           = depot;
  allocator->zoneNumber      = zoneNumber;
  allocator->threadID        = threadID;
  allocator->nonce           = nonce;
  allocator->readOnlyContext = readOnlyContext;
  initializeRing(&allocator->dirtySlabJournals);

  result = allocateComponents(allocator, layer, vioPoolSize);
  if (result != VDO_SUCCESS) {
    freeBlockAllocator(&allocator);
    return result;
  }

  *allocatorPtr = allocator;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeBlockAllocator(BlockAllocator **blockAllocatorPtr)
{
  BlockAllocator *allocator = *blockAllocatorPtr;
  if (allocator == NULL) {
    return;
  }

  freeSlabScrubber(&allocator->slabScrubber);
  freeSlabCompletion(&allocator->slabCompletion);
  freeVIOPool(&allocator->vioPool);
  freePriorityTable(&allocator->prioritizedSlabs);
  destroyEnqueueable(&allocator->completion);
  FREE(allocator);
  *blockAllocatorPtr = NULL;
}

/**********************************************************************/
int replaceVIOPool(BlockAllocator *allocator,
                   size_t          size,
                   PhysicalLayer  *layer)
{
  freeVIOPool(&allocator->vioPool);
  return makeVIOPool(layer, size, makeAllocatorPoolVIOs, NULL,
                     &allocator->vioPool);
}

/**********************************************************************/
void notifyBlockAllocatorOfReadOnlyMode(BlockAllocator *allocator)
{
  assertOnAllocatorThread(allocator->threadID, __func__);
  SlabIterator iterator = getSlabIterator(allocator);
  while (hasNextSlab(&iterator)) {
    Slab *slab = nextSlab(&iterator);
    abortSlabJournalWaiters(slab->journal);
  }
}

/**
 * Get the maximum number of data blocks that can be allocated.
 *
 * @param allocator  The block allocator to query
 *
 * @return The number of data blocks that can be allocated
 **/
__attribute__((warn_unused_result))
static inline BlockCount getDataBlockCount(const BlockAllocator *allocator)
{
  return (allocator->slabCount * allocator->depot->slabConfig.dataBlocks);
}

/**********************************************************************/
BlockCount getAllocatedBlocks(const BlockAllocator *allocator)
{
  return relaxedLoad64(&allocator->statistics.allocatedBlocks);
}

/**********************************************************************/
BlockCount getUnrecoveredSlabCount(const BlockAllocator *allocator)
{
  return getScrubberSlabCount(allocator->slabScrubber);
}

/**********************************************************************/
void queueSlab(Slab *slab)
{
  ASSERT_LOG_ONLY(isRingEmpty(&slab->ringNode),
                  "a requeued slab must not already be on a ring");
  BlockAllocator *allocator  = slab->allocator;
  BlockCount      freeBlocks = getSlabFreeBlockCount(slab);
  int result = ASSERT((freeBlocks <= allocator->depot->slabConfig.dataBlocks),
                      "rebuilt slab %u must have a valid free block count"
                      " (has %" PRIu64 ", expected maximum %" PRIu64 ")",
                      slab->slabNumber, freeBlocks,
                      allocator->depot->slabConfig.dataBlocks);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(allocator->readOnlyContext, result);
    return;
  }

  relaxedAdd64(&allocator->statistics.allocatedBlocks, -freeBlocks);
  if (!isSlabJournalBlank(slab->journal)) {
    relaxedAdd64(&allocator->statistics.slabsOpened, 1);
  }

  // All other slabs are kept in a priority queue for allocation.
  prioritizeSlab(slab);
}

/**********************************************************************/
void adjustFreeBlockCount(Slab *slab, bool increment)
{
  BlockAllocator *allocator = slab->allocator;
  // The sense of increment is reversed since allocations are being counted.
  relaxedAdd64(&allocator->statistics.allocatedBlocks, (increment ? -1 : 1));

  // The open slab doesn't need to be reprioritized until it is closed.
  if (slab == allocator->openSlab) {
    return;
  }

  // The slab priority rarely changes; if no change, then don't requeue it.
  if (slab->priority == calculateSlabPriority(slab)) {
    return;
  }

  // Reprioritize the slab to reflect the new free block count by removing it
  // from the table and re-enqueuing it with the new priority.
  priorityTableRemove(allocator->prioritizedSlabs, &slab->ringNode);
  prioritizeSlab(slab);
}

/**
 * Allocate the next free physical block in a slab.
 *
 * The block allocated will have a provisional reference and the
 * reference must be either confirmed with a subsequent call to
 * incrementReferenceCount() or vacated with a subsequent call to
 * decrementReferenceCount().
 *
 * @param [in]  slab            The slab
 * @param [out] blockNumberPtr  A pointer to receive the allocated block number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int allocateSlabBlock(Slab *slab, PhysicalBlockNumber *blockNumberPtr)
{
  PhysicalBlockNumber pbn;
  int result = allocateUnreferencedBlock(slab->referenceCounts, &pbn);
  if (result != VDO_SUCCESS) {
    return result;
  }

  adjustFreeBlockCount(slab, false);

  *blockNumberPtr = pbn;
  return VDO_SUCCESS;
}

/**********************************************************************/
int allocateBlock(BlockAllocator *allocator,
                  PhysicalBlockNumber *blockNumberPtr)
{
  if (allocator->openSlab != NULL) {
    // Try to allocate the next block in the currently open slab.
    int result = allocateSlabBlock(allocator->openSlab, blockNumberPtr);
    if ((result == VDO_SUCCESS) || (result != VDO_NO_SPACE)) {
      return result;
    }

    // Put the exhausted open slab back into the priority table.
    prioritizeSlab(allocator->openSlab);
  }

  // Remove the highest priority slab from the priority table and make it
  // the open slab.
  allocator->openSlab
    = slabFromRingNode(priorityTableDequeue(allocator->prioritizedSlabs));

  if (isSlabJournalBlank(allocator->openSlab->journal)) {
    relaxedAdd64(&allocator->statistics.slabsOpened, 1);
    int result = dirtyAllReferenceBlocks(allocator->openSlab->referenceCounts);
    if (result != VDO_SUCCESS) {
      return result;
    }
  } else {
    relaxedAdd64(&allocator->statistics.slabsReopened, 1);
  }

  // Try allocating again. If we're out of space immediately after opening a
  // slab, then every slab must be fully allocated.
  return allocateSlabBlock(allocator->openSlab, blockNumberPtr);
}

/**********************************************************************/
void releaseBlockReference(BlockAllocator      *allocator,
                           PhysicalBlockNumber  pbn,
                           const char          *why)
{
  if (pbn == ZERO_BLOCK) {
    return;
  }

  Slab *slab = getSlab(allocator->depot, pbn);
  ReferenceOperation operation = {
    .type = DATA_DECREMENT,
    .pbn  = pbn,
  };
  int result = modifySlabReferenceCount(slab, NULL, operation);
  if (result != VDO_SUCCESS) {
    logErrorWithStringError(result,
                            "Failed to release reference to %s "
                            "physical block %" PRIu64,
                            why, pbn);
  }
}

/**
 * This is a HeapComparator function that orders SlabStatuses using the
 * 'isClean' field as the primary key and the 'emptiness' field as the
 * secondary key.
 *
 * Slabs need to be pushed onto the rings in the same order they are to be
 * popped off. Popping should always get the most empty first, so pushing
 * should be from most empty to least empty. Thus, the comparator order is
 * the usual sense since Heap returns larger elements before smaller ones.
 *
 * @param item1  The first item to compare
 * @param item2  The second item to compare
 *
 * @return  1 if the first item is cleaner or emptier than the second;
 *          0 if the two items are equally clean and empty;
           -1 otherwise
 **/
static int compareSlabStatuses(const void *item1, const void *item2)
{
  const SlabStatus *info1 = (const SlabStatus *) item1;
  const SlabStatus *info2 = (const SlabStatus *) item2;

  if (info1->isClean != info2->isClean) {
    return (info1->isClean ? 1 : -1);
  }
  if (info1->emptiness != info2->emptiness) {
    return ((info1->emptiness > info2->emptiness) ? 1 : -1);
  }
  return ((info1->slabNumber < info2->slabNumber) ? 1 : -1);
}

/**********************************************************************/
int prepareSlabsForAllocation(BlockAllocator *allocator)
{
  relaxedStore64(&allocator->statistics.allocatedBlocks,
                 getDataBlockCount(allocator));

  SlabDepot *depot     = allocator->depot;
  SlabCount  slabCount = depot->slabCount;

  SlabStatus *slabStatuses;
  int result = ALLOCATE(slabCount, SlabStatus, __func__, &slabStatuses);
  if (result != VDO_SUCCESS) {
    return result;
  }

  getSummarizedSlabStatuses(allocator->summary, slabCount, slabStatuses);

  // Sort the slabs by cleanliness, then by emptiness hint.
  Heap heap;
  initializeHeap(&heap, compareSlabStatuses, slabStatuses, slabCount,
                 sizeof(SlabStatus));
  buildHeap(&heap, slabCount);

  SlabStatus currentSlabStatus;
  while (popMaxHeapElement(&heap, &currentSlabStatus)) {
    Slab *slab = depot->slabs[currentSlabStatus.slabNumber];
    if (slab->allocator != allocator) {
      continue;
    }

    if ((depot->loadType == NO_LOAD)
        || (!mustLoadRefCounts(allocator->summary, slab->slabNumber)
            && currentSlabStatus.isClean)) {
      queueSlab(slab);
      continue;
    }

    markSlabUnrecovered(slab);
    bool highPriority
      = ((currentSlabStatus.isClean && (depot->loadType == NORMAL_LOAD))
         || requiresScrubbing(slab->journal));
    registerSlabForScrubbing(allocator->slabScrubber, slab, highPriority);
  }
  FREE(slabStatuses);

  return VDO_SUCCESS;
}

/**********************************************************************/
void prepareAllocatorToAllocate(BlockAllocator *allocator,
                                VDOCompletion  *parent,
                                VDOAction      *callback,
                                VDOAction      *errorHandler)
{
  int result = prepareSlabsForAllocation(allocator);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  scrubHighPrioritySlabs(allocator->slabScrubber,
                         isPriorityTableEmpty(allocator->prioritizedSlabs),
                         parent, callback, errorHandler);
}

/**********************************************************************/
void registerNewSlabsForAllocator(BlockAllocator *allocator,
                                  VDOCompletion  *parent,
                                  VDOAction      *callback,
                                  VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  SlabDepot *depot = allocator->depot;
  for (SlabCount i = depot->slabCount; i < depot->newSlabCount; i++) {
    Slab *slab = depot->newSlabs[i];
    if (slab->allocator == allocator) {
      registerSlabWithAllocator(allocator, slab, true);
    }
  }
  finishCompletion(&allocator->completion, VDO_SUCCESS);
}

/**********************************************************************/
void suspendSummaryZone(BlockAllocator *allocator,
                        VDOCompletion  *parent,
                        VDOAction      *callback,
                        VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  suspendSlabSummaryZone(allocator->summary, &allocator->completion);
}

/**********************************************************************/
void resumeSummaryZone(BlockAllocator *allocator,
                       VDOCompletion  *parent,
                       VDOAction      *callback,
                       VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  resumeSlabSummaryZone(allocator->summary, &allocator->completion);
}

/**
 * Handle an error while closing an allocator.
 *
 * @param completion  The slab completion
 **/
static void handleCloseError(VDOCompletion *completion)
{
  // Preserve the error.
  BlockAllocator *allocator = completion->parent;
  setCompletionResult(&allocator->completion, completion->result);

  // Continue along the close path
  completion->callback(completion);
}

/**
 * Perform a step in closing the allocator. This method is its own callback.
 *
 * @param completion  The slab completion
 **/
static void doCloseAllocatorStep(VDOCompletion *completion)
{
  BlockAllocator *allocator = completion->parent;
  resetCompletion(completion);
  completion->requeue = true;
  switch (++allocator->closeStep) {
  case CLOSE_ALLOCATOR_STEP_SAVE_SLABS:
    saveSlabs(completion, getSlabIterator(allocator));
    return;

  case CLOSE_ALLOCATOR_STEP_CLOSE_SLAB_SUMMARY:
    closeSlabSummaryZone(allocator->summary, completion);
    return;

  case CLOSE_ALLOCATOR_VIO_POOL:
    closeObjectPool(allocator->vioPool, completion);
    return;

  default:
    finishCompletion(&allocator->completion, VDO_SUCCESS);
  }
}

/**
 * Initiate the close of the allocator now that scrubbing is complete.
 *
 * @param allocator  The allocater to close
 **/
static void launchClose(BlockAllocator *allocator)
{
  allocator->closeStep = CLOSE_ALLOCATOR_START;
  prepareForRequeue(allocator->slabCompletion, doCloseAllocatorStep,
                    handleCloseError, allocator->threadID, allocator);
  doCloseAllocatorStep(allocator->slabCompletion);
}

/**********************************************************************/
void closeBlockAllocator(BlockAllocator *allocator,
                         VDOCompletion  *parent,
                         VDOAction      *callback,
                         VDOAction      *errorHandler)
{
  allocator->saveRequested = true;
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  if (isScrubbing(allocator->slabScrubber)) {
    stopScrubbing(allocator->slabScrubber);
  } else {
    launchClose(allocator);
  }
}

/**********************************************************************/
void saveBlockAllocatorForFullRebuild(BlockAllocator *allocator,
                                      VDOCompletion  *parent,
                                      VDOAction      *callback,
                                      VDOAction      *errorHandler)
{
  prepareCompletion(allocator->slabCompletion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  saveFullyRebuiltSlabs(allocator->slabCompletion, getSlabIterator(allocator));
}

/**
 * Save the slab summary zone for this allocator now that the slab journals
 * have all been flushed. This callback is registered in
 * flushAllocatorSlabJournals().
 *
 * @param completion  The slab completion
 **/
static void finishFlushingSlabJournals(VDOCompletion *completion)
{
  BlockAllocator *allocator = completion->parent;
  saveSlabSummaryZone(allocator->summary, &allocator->completion);
}

/**
 * Handle an error flushing slab journals.
 *
 * @param completion  The slab completion
 **/
static void handleFlushError(VDOCompletion *completion)
{
  // Preserve the error.
  BlockAllocator *allocator = completion->parent;
  setCompletionResult(&allocator->completion, completion->result);
  finishFlushingSlabJournals(completion);
}

/**********************************************************************/
void flushAllocatorSlabJournals(BlockAllocator *allocator,
                                VDOCompletion  *parent,
                                VDOAction       callback,
                                VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  prepareCompletion(allocator->slabCompletion, finishFlushingSlabJournals,
                    handleFlushError, allocator->threadID, allocator);
  flushSlabJournals(allocator->slabCompletion, getSlabIterator(allocator));
}

/**********************************************************************/
void saveRebuiltSlab(Slab          *slab,
                     VDOCompletion *parent,
                     VDOAction     *callback,
                     VDOAction     *errorHandler)
{
  BlockAllocator *allocator = slab->allocator;
  prepareCompletion(allocator->slabCompletion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  saveSlab(allocator->slabCompletion, slab);
}

/**********************************************************************/
void releaseTailBlockLocks(BlockAllocator *allocator,
                           VDOCompletion  *parent,
                           VDOAction      *callback,
                           VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  RingNode *ring = &allocator->dirtySlabJournals;
  while (!isRingEmpty(ring)) {
    if (!releaseRecoveryJournalLock(slabJournalFromDirtyNode(ring->next),
                                    allocator->depot->activeReleaseRequest)) {
      break;
    }
  }
  completeCompletion(&allocator->completion);
}

/**********************************************************************/
SlabSummaryZone *getSlabSummaryZone(const BlockAllocator *allocator)
{
  return allocator->summary;
}

/**********************************************************************/
int acquireVIO(BlockAllocator *allocator, Waiter *waiter)
{
  return acquireVIOFromPool(allocator->vioPool, waiter);
}

/**********************************************************************/
void returnVIO(BlockAllocator *allocator, VIOPoolEntry *entry)
{
  returnVIOToPool(allocator->vioPool, entry);
}

/**
 * Notify the allocator that the slab scrubber has stopped. This callback
 * is registered in scrubAllUnrecoveredSlabsInZone().
 *
 * @param completion  The slab scrubber completion
 **/
static void scrubberFinished(VDOCompletion *completion)
{
  BlockAllocator *allocator = completion->parent;
  notifyZoneStoppedScrubbing(allocator->depot);
  if (allocator->saveRequested) {
    launchClose(allocator);
  }
}

/**********************************************************************/
void scrubAllUnrecoveredSlabsInZone(BlockAllocator *allocator,
                                    VDOCompletion  *parent,
                                    VDOAction      *callback,
                                    VDOAction      *errorHandler)
{
  prepareCompletion(&allocator->completion, callback, errorHandler,
                    parent->callbackThreadID, parent);
  scrubSlabs(allocator->slabScrubber, allocator, scrubberFinished,
             scrubberFinished, allocator->threadID);
  completeCompletion(&allocator->completion);
}

/**********************************************************************/
int enqueueForCleanSlab(BlockAllocator *allocator, Waiter *waiter)
{
  return enqueueCleanSlabWaiter(allocator->slabScrubber, waiter);
}

/**********************************************************************/
void increaseScrubbingPriority(Slab *slab)
{
  registerSlabForScrubbing(slab->allocator->slabScrubber, slab, true);
}

/**********************************************************************/
void allocateFromAllocatorLastSlab(BlockAllocator *allocator)
{
  ASSERT_LOG_ONLY(allocator->openSlab == NULL, "mustn't have an open slab");
  Slab *lastSlab = allocator->depot->slabs[allocator->lastSlab];
  priorityTableRemove(allocator->prioritizedSlabs, &lastSlab->ringNode);
  allocator->openSlab = lastSlab;
}

/**********************************************************************/
BlockAllocatorStatistics
getBlockAllocatorStatistics(const BlockAllocator *allocator)
{
  const AtomicAllocatorStatistics *atoms = &allocator->statistics;
  return (BlockAllocatorStatistics) {
    .slabCount     = allocator->slabCount,
    .slabsOpened   = relaxedLoad64(&atoms->slabsOpened),
    .slabsReopened = relaxedLoad64(&atoms->slabsReopened),
  };
}

/**********************************************************************/
SlabJournalStatistics getSlabJournalStatistics(const BlockAllocator *allocator)
{
  const AtomicSlabJournalStatistics *atoms = &allocator->slabJournalStatistics;
  return (SlabJournalStatistics) {
    .diskFullCount = atomicLoad64(&atoms->diskFullCount),
    .flushCount    = atomicLoad64(&atoms->flushCount),
    .blockedCount  = atomicLoad64(&atoms->blockedCount),
    .blocksWritten = atomicLoad64(&atoms->blocksWritten),
    .tailBusyCount = atomicLoad64(&atoms->tailBusyCount),
  };
}

/**********************************************************************/
RefCountsStatistics getRefCountsStatistics(const BlockAllocator *allocator)
{
  const AtomicRefCountStatistics *atoms = &allocator->refCountStatistics;
  return (RefCountsStatistics) {
    .blocksWritten = atomicLoad64(&atoms->blocksWritten),
  };
}

/**********************************************************************/
void dumpBlockAllocator(const BlockAllocator *allocator)
{
  unsigned int pauseCounter = 0;
  logInfo("BlockAllocator zone %u", allocator->zoneNumber);
  SlabIterator iterator = getSlabIterator(allocator);
  while (hasNextSlab(&iterator)) {
    dumpSlab(nextSlab(&iterator));

    // Wait for a while after each batch of 32 slabs dumped, allowing the
    // kernel log a chance to be flushed instead of being overrun.
    if (pauseCounter++ == 31) {
      pauseCounter = 0;
      pauseForLogger();
    }
  }

  dumpSlabScrubber(allocator->slabScrubber);
}
