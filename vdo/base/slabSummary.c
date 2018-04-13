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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabSummary.c#1 $
 */

#include "slabSummary.h"

#include "memoryAlloc.h"

#include "constants.h"
#include "extent.h"
#include "readOnlyModeContext.h"
#include "slabDepot.h"
#include "slabSummaryInternals.h"
#include "threadConfig.h"
#include "types.h"

// SIZING

/**********************************************************************/
static BlockCount getSlabSummaryZoneSize(BlockSize blockSize)
{
  SlabCount entriesPerBlock = blockSize / sizeof(SlabSummaryEntry);
  BlockCount blocksNeeded   = MAX_SLABS / entriesPerBlock;
  return blocksNeeded;
}

/**********************************************************************/
BlockCount getSlabSummarySize(BlockSize blockSize)
{
  return getSlabSummaryZoneSize(blockSize) * MAX_PHYSICAL_ZONES;
}

// FULLNESS HINT COMPUTATION

/**
 * Translate a slab's free block count into a 'fullness hint' that can be
 * stored in a SlabSummaryEntry's 7 bits that are dedicated to its free count.
 *
 * Note: the number of free blocks must be strictly less than 2^23 blocks,
 * even though theoretically slabs could contain precisely 2^23 blocks; there
 * is an assumption that at least one block is used by metadata. This
 * assumption is necessary; otherwise, the fullness hint might overflow.
 * The fullness hint formula is roughly (fullness >> 16) & 0x7f, but
 * ((1 << 23) >> 16) & 0x7f is the same as (0 >> 16) & 0x7f, namely 0, which
 * is clearly a bad hint if it could indicate both 2^23 free blocks or 0 free
 * blocks.
 *
 * @param summary     The summary which is being updated
 * @param freeBlocks  The number of free blocks
 *
 * @return A fullness hint, which can be stored in 7 bits.
 **/
__attribute__((warn_unused_result))
static uint8_t computeFullnessHint(SlabSummary *summary, BlockCount freeBlocks)
{
  ASSERT_LOG_ONLY((freeBlocks < (1 << 23)),
                  "free blocks must be less than 2^23");

  if (freeBlocks == 0) {
    return 0;
  }

  BlockCount hint = freeBlocks >> summary->hintShift;
  return ((hint == 0) ? 1 : hint);
}

/**
 * Translate a slab's free block hint into an approximate count, such that
 * computeFullnessHint() is the inverse function of getApproximateFreeBlocks()
 * (i.e. computeFullnessHint(getApproximateFreeBlocks(x)) == x).
 *
 * @param  summary        The summary from which the hint was obtained
 * @param  freeBlockHint  The hint read from the summary
 *
 * @return An approximation to the free block count
 **/
__attribute__((warn_unused_result))
static BlockCount getApproximateFreeBlocks(SlabSummary *summary,
                                           uint8_t      freeBlockHint)
{
  return ((BlockCount) freeBlockHint) << summary->hintShift;
}

// MAKE/FREE FUNCTIONS

/**********************************************************************/
static void finishUpdatingSlabSummaryBlock(VDOCompletion *completion);
static void handleWriteError(VDOCompletion *completion);
static void launchWrite(SlabSummaryBlock *summaryBlock);

/**
 * Initialize a SlabSummaryBlock.
 *
 * @param layer             The backing layer
 * @param summaryZone       The parent SlabSummaryZone
 * @param threadID          The ID of the thread of physical zone of this block
 * @param entries           The entries this block manages
 * @param pbn               The physical location of this block on disk
 * @param slabSummaryBlock  The block to intialize
 *
 * @return VDO_SUCCESS or an error
 **/
static int initializeSlabSummaryBlock(PhysicalLayer       *layer,
                                      SlabSummaryZone     *summaryZone,
                                      ThreadID             threadID,
                                      SlabSummaryEntry    *entries,
                                      PhysicalBlockNumber  pbn,
                                      SlabSummaryBlock    *slabSummaryBlock)
{
  int result = ALLOCATE(VDO_BLOCK_SIZE, char, __func__,
                        &slabSummaryBlock->outgoingEntries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = createVIO(layer, VIO_TYPE_SLAB_SUMMARY, VIO_PRIORITY_METADATA,
                     slabSummaryBlock, slabSummaryBlock->outgoingEntries,
                     &slabSummaryBlock->vio);
  if (result != VDO_SUCCESS) {
    return result;
  }

  slabSummaryBlock->vio->completion.callbackThreadID = threadID;
  slabSummaryBlock->zone                             = summaryZone;
  slabSummaryBlock->entries                          = entries;
  slabSummaryBlock->pbn                              = pbn;
  return VDO_SUCCESS;
}

/**
 * Create a new, empty SlabSummaryZone object.
 *
 * @param [in]  summary           The summary to which the new zone will belong
 * @param [in]  layer             The layer
 * @param [in]  zoneNumber        The zone this is
 * @param [in]  threadID          The ID of the thread for this zone
 * @param [in]  pbn               The translated pbn at which this zone's on
 *                                disk data begins
 * @param [in]  entries           The buffer to hold the entries in this zone
 *
 * @return VDO_SUCCESS or an error
 **/
static int makeSlabSummaryZone(SlabSummary         *summary,
                               PhysicalLayer       *layer,
                               ZoneCount            zoneNumber,
                               ThreadID             threadID,
                               PhysicalBlockNumber  pbn,
                               SlabSummaryEntry    *entries)
{
  int result = ALLOCATE_EXTENDED(SlabSummaryZone, summary->blocksPerZone,
                                 SlabSummaryBlock, __func__,
                                 &summary->zones[zoneNumber]);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SlabSummaryZone *summaryZone = summary->zones[zoneNumber];
  summaryZone->summary         = summary;
  summaryZone->zoneNumber      = zoneNumber;
  summaryZone->entries         = entries;

  if (layer->createMetadataVIO == NULL) {
    // Blocks are only used for writing, and without a createVIO() call,
    // we'll never be writing anything.
    return VDO_SUCCESS;
  }

  // Initialize each block.
  for (BlockCount i = 0; i < summary->blocksPerZone; i++, pbn++) {
    result = initializeSlabSummaryBlock(layer, summaryZone, threadID, entries,
                                        pbn, &summaryZone->summaryBlocks[i]);
    if (result != VDO_SUCCESS) {
      return result;
    }
    entries += summary->entriesPerBlock;
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int makeSlabSummary(PhysicalLayer        *layer,
                    Partition            *partition,
                    const ThreadConfig   *threadConfig,
                    unsigned int          slabSizeShift,
                    BlockCount            maximumFreeBlocksPerSlab,
                    ReadOnlyModeContext  *readOnlyContext,
                    SlabSummary         **slabSummaryPtr)
{
  BlockCount blocksPerZone   = getSlabSummaryZoneSize(VDO_BLOCK_SIZE);
  SlabCount  entriesPerBlock = MAX_SLABS / blocksPerZone;
  int result = ASSERT((entriesPerBlock * blocksPerZone) == MAX_SLABS,
                      "block size must be a multiple of entry size");
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (partition == NULL) {
    // Don't make a slab summary for the formatter since it doesn't need it.
    return VDO_SUCCESS;
  }

  SlabSummary *summary;
  result = ALLOCATE_EXTENDED(SlabSummary, threadConfig->physicalZoneCount,
                             SlabSummaryZone *, __func__, &summary);
  if (result != VDO_SUCCESS) {
    return result;
  }

  summary->zoneCount       = threadConfig->physicalZoneCount;
  summary->readOnlyContext = readOnlyContext;
  summary->hintShift       = (slabSizeShift > 6) ? (slabSizeShift - 6) : 0;
  summary->blocksPerZone   = blocksPerZone;
  summary->entriesPerBlock = entriesPerBlock;
  initializeCompletion(&summary->completion, SLAB_SUMMARY_COMPLETION,
                       layer);

  size_t totalEntries = MAX_SLABS * MAX_PHYSICAL_ZONES;
  size_t entryBytes = totalEntries * sizeof(SlabSummaryEntry);
  result = layer->allocateIOBuffer(layer, entryBytes, "summary entries",
                                   (char **) &summary->entries);
  if (result != VDO_SUCCESS) {
    freeSlabSummary(&summary);
    return result;
  }

  // Initialize all the entries.
  uint8_t hint = computeFullnessHint(summary, maximumFreeBlocksPerSlab);
  for (size_t i = 0; i < totalEntries; i++) {
    // This default tail block offset must be reflected in
    // slabJournal.c::readSlabJournalTail().
    summary->entries[i] = (SlabSummaryEntry) {
      .tailBlockOffset = 0,
      .fullnessHint    = hint,
      .loadRefCounts   = false,
      .isDirty         = false,
    };
  }

  setSlabSummaryOrigin(summary, partition);
  PhysicalBlockNumber pbn = summary->origin;
  for (ZoneCount zone = 0; zone < summary->zoneCount; zone++) {
    result = makeSlabSummaryZone(summary, layer, zone,
                                 getPhysicalZoneThread(threadConfig, zone),
                                 pbn, summary->entries + (MAX_SLABS * zone));
    if (result != VDO_SUCCESS) {
      freeSlabSummary(&summary);
      return result;
    }
    pbn += blocksPerZone;
  }

  *slabSummaryPtr = summary;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabSummary(SlabSummary **slabSummaryPtr)
{
  if (*slabSummaryPtr == NULL) {
    return;
  }

  SlabSummary *summary = *slabSummaryPtr;
  for (ZoneCount zone = 0; zone < summary->zoneCount; zone++) {
    SlabSummaryZone *summaryZone = summary->zones[zone];
    if (summaryZone != NULL) {
      for (BlockCount i = 0; i < summary->blocksPerZone; i++) {
        freeVIO(&summaryZone->summaryBlocks[i].vio);
        FREE(summaryZone->summaryBlocks[i].outgoingEntries);
      }
      FREE(summaryZone);
    }
  }
  FREE(summary->entries);
  FREE(summary);
  *slabSummaryPtr = NULL;
}

/**********************************************************************/
SlabSummaryZone *getSummaryForZone(SlabSummary *summary, ZoneCount zone)
{
  return summary->zones[zone];
}

// WRITING FUNCTIONALITY

/**
 * Process a completed save/wait on a block.
 *
 * @param summaryZone  The zone to which this block belongs
 **/
static void checkForSaveComplete(SlabSummaryZone *summaryZone)
{
  if (summaryZone->saveWaiter == NULL) {
    return;
  }

  for (BlockCount i = 0; i < summaryZone->summary->blocksPerZone; i++) {
    SlabSummaryBlock *block = &summaryZone->summaryBlocks[i];
    if (block->currentlyWriting || block->needsWriting) {
      return;
    }
  }

  if (summaryZone->pendingAction == SUSPEND_REQUESTED) {
    // The save is done, suspend now.
    summaryZone->pendingAction = NONE_REQUESTED;
    summaryZone->suspended     = true;
  }

  VDOCompletion *saveWaiter = summaryZone->saveWaiter;
  summaryZone->saveWaiter   = NULL;
  finishCompletion(saveWaiter,
                   (isReadOnly(summaryZone->summary->readOnlyContext)
                    ? VDO_READ_ONLY : VDO_SUCCESS));
}

/**
 * Wake all the waiters in a given queue. If the VDO is in read-only mode they
 * will be given a VDO_READ_ONLY error code as their context, otherwise they
 * will be given VDO_SUCCESS.
 *
 * @param summaryZone  The slab summary which owns the queue
 * @param queue        The queue to notify
 **/
static void notifyWaiters(SlabSummaryZone *summaryZone, WaitQueue *queue)
{
  int result = (isReadOnly(summaryZone->summary->readOnlyContext)
                ? VDO_READ_ONLY : VDO_SUCCESS);
  notifyAllWaiters(queue, NULL, &result);
}

/**
 * Finish an update-generated block write.
 *
 * @param completion  The write VIO
 **/
static void finishUpdatingSlabSummaryBlock(VDOCompletion *completion)
{
  SlabSummaryBlock *block = completion->parent;
  notifyWaiters(block->zone, &block->currentUpdateWaiters);
  atomicAdd64(&block->zone->summary->statistics.blocksWritten, 1);
  block->currentlyWriting = false;
  if (block->needsWriting) {
    launchWrite(block);
    return;
  }

  checkForSaveComplete(block->zone);
}

/**
 * Finish processing a block which cannot be written due to the VDO going into
 * read-only mode.
 *
 * @param block The block which cannot be written
 **/
static void finishUnwritableBlock(SlabSummaryBlock *block)
{
  notifyWaiters(block->zone, &block->currentUpdateWaiters);
  notifyWaiters(block->zone, &block->nextUpdateWaiters);
  block->currentlyWriting = false;
  block->needsWriting     = false;
  checkForSaveComplete(block->zone);
}

/**
 * Handle an error writing a slab summary block.
 *
 * @param completion  The write VIO
 **/
static void handleWriteError(VDOCompletion *completion)
{
  SlabSummaryBlock *block = completion->parent;
  enterReadOnlyMode(block->zone->summary->readOnlyContext, completion->result);
  finishUnwritableBlock(block);
}

/**
 * Given a SlabSummaryBlock, determine if it needs to be written, and if so,
 * set up and launch a write of this block.
 *
 * @param [in] block  The block that needs to be committed
 **/
static void launchWrite(SlabSummaryBlock *block)
{
  if (block->zone->suspended || block->currentlyWriting
      || !block->needsWriting) {
    return;
  }

  block->needsWriting = false;

  if (isReadOnly(block->zone->summary->readOnlyContext)) {
    finishUnwritableBlock(block);
    return;
  }

  transferAllWaiters(&block->nextUpdateWaiters, &block->currentUpdateWaiters);

  block->currentlyWriting = true;

  SlabCount entriesPerBlock = block->zone->summary->entriesPerBlock;
  memcpy(block->outgoingEntries, block->entries,
         sizeof(SlabSummaryEntry) * entriesPerBlock);

  // Flush before writing to ensure that the slab journal tail blocks and
  // reference updates covered by this summary update are stable (VDO-2332).
  launchWriteMetadataVIOWithFlush(block->vio, block->pbn,
                                  finishUpdatingSlabSummaryBlock,
                                  handleWriteError, true, false);
}

/**
 * Launch writes for all blocks that need writing.
 *
 * @param summaryZone  The zone in question
 * @param forceWrite   Whether to force the write of all blocks
 **/
static void launchWriteOfAllBlocks(SlabSummaryZone *summaryZone,
                                   bool             forceWrite)
{
  SlabSummary *summary       = summaryZone->summary;
  BlockCount   blocksPerZone = summary->blocksPerZone;
  PhysicalBlockNumber pbn
    = (summary->origin + (summaryZone->zoneNumber * blocksPerZone));

  // Prepare all the blocks. Do this before launching any to ensure that we
  // don't prematurely decide that the save is complete.
  for (BlockCount i = 0; i < blocksPerZone; i++) {
    SlabSummaryBlock *block = &summaryZone->summaryBlocks[i];
    // Make sure the PBNs of the blocks are accurate.
    block->pbn = pbn++;
    if (forceWrite) {
      block->needsWriting = true;
    }
  }

  for (BlockCount i = 0; i < blocksPerZone; i++) {
    launchWrite(&summaryZone->summaryBlocks[i]);
  }
}

/**********************************************************************/
void saveSlabSummaryZone(SlabSummaryZone *summaryZone, VDOCompletion *parent)
{
  summaryZone->saveWaiter = parent;

  launchWriteOfAllBlocks(summaryZone, true);
}

/**********************************************************************/
void suspendSlabSummaryZone(SlabSummaryZone *summaryZone,
                            VDOCompletion   *parent)
{
  summaryZone->pendingAction = SUSPEND_REQUESTED;
  saveSlabSummaryZone(summaryZone, parent);
}

/**********************************************************************/
void resumeSlabSummaryZone(SlabSummaryZone *summaryZone,
                           VDOCompletion   *parent)
{
  summaryZone->suspended = false;
  launchWriteOfAllBlocks(summaryZone, false);
  finishCompletion(parent, VDO_SUCCESS);
}

/**********************************************************************/
void closeSlabSummaryZone(SlabSummaryZone *summaryZone, VDOCompletion *parent)
{
  summaryZone->saveWaiter    = parent;
  summaryZone->pendingAction = CLOSE_REQUESTED;
  checkForSaveComplete(summaryZone);
}

// READ/UPDATE FUNCTIONS

/**
 * Get the summary block, and offset into it, for storing the summary for a
 * slab.
 *
 * @param summaryZone    The SlabSummaryZone being queried
 * @param slabNumber     The slab whose summary location is sought
 *
 * @return A pointer to the SlabSummaryEntryBlock containing this
 *         SlabSummaryEntry
 **/
static SlabSummaryBlock *getSummaryBlockForSlab(SlabSummaryZone *summaryZone,
                                                SlabCount        slabNumber)
{
  SlabCount entriesPerBlock = summaryZone->summary->entriesPerBlock;
  return &summaryZone->summaryBlocks[slabNumber / entriesPerBlock];
}

/**********************************************************************/
void updateSlabSummaryEntry(SlabSummaryZone *summaryZone,
                            Waiter          *waiter,
                            SlabCount        slabNumber,
                            TailBlockOffset  tailBlockOffset,
                            bool             loadRefCounts,
                            bool             isClean,
                            BlockCount       freeBlocks)
{
  int result;
  if (isReadOnly(summaryZone->summary->readOnlyContext)) {
    result = VDO_READ_ONLY;
    waiter->callback(waiter, &result);
    return;
  }

  if (summaryZone->pendingAction == CLOSE_REQUESTED) {
    result = VDO_COMPONENT_BUSY;
    waiter->callback(waiter, &result);
    return;
  }

  SlabSummaryBlock *block = getSummaryBlockForSlab(summaryZone, slabNumber);
  block->needsWriting     = true;
  SlabSummaryEntry *entry = &summaryZone->entries[slabNumber];
  entry->tailBlockOffset  = tailBlockOffset;
  entry->loadRefCounts    = (entry->loadRefCounts || loadRefCounts);
  entry->isDirty          = !isClean;
  entry->fullnessHint
    = computeFullnessHint(summaryZone->summary, freeBlocks);

  result = enqueueWaiter(&block->nextUpdateWaiters, waiter);
  if (result != VDO_SUCCESS) {
    waiter->callback(waiter, &result);
    return;
  }

  launchWrite(block);
}

/**********************************************************************/
TailBlockOffset getSummarizedTailBlockOffset(SlabSummaryZone *summaryZone,
                                             SlabCount        slabNumber)
{
  return summaryZone->entries[slabNumber].tailBlockOffset;
}

/**********************************************************************/
bool mustLoadRefCounts(SlabSummaryZone *summaryZone, SlabCount slabNumber)
{
  return summaryZone->entries[slabNumber].loadRefCounts;
}

/**********************************************************************/
bool getSummarizedCleanliness(SlabSummaryZone *summaryZone,
                              SlabCount        slabNumber)
{
  return !summaryZone->entries[slabNumber].isDirty;
}

/**********************************************************************/
BlockCount getSummarizedFreeBlockCount(SlabSummaryZone *summaryZone,
                                       SlabCount        slabNumber)
{
  SlabSummaryEntry *entry = &summaryZone->entries[slabNumber];
  return getApproximateFreeBlocks(summaryZone->summary, entry->fullnessHint);
}

/**********************************************************************/
void getSummarizedRefCountsState(SlabSummaryZone *summaryZone,
                                 SlabCount        slabNumber,
                                 size_t          *freeBlockHint,
                                 bool            *isClean)
{
  SlabSummaryEntry *entry = &summaryZone->entries[slabNumber];
  *freeBlockHint          = entry->fullnessHint;
  *isClean                = !entry->isDirty;
}

/**********************************************************************/
void getSummarizedSlabStatuses(SlabSummaryZone *summaryZone,
                               SlabCount        slabCount,
                               SlabStatus      *statuses)
{
  for (SlabCount i = 0; i < slabCount; i++) {
    statuses[i] = (SlabStatus) {
      .slabNumber = i,
      .isClean    = !summaryZone->entries[i].isDirty,
      .emptiness  = summaryZone->entries[i].fullnessHint
    };
  }
}

// RESIZE FUNCTIONS

/**********************************************************************/
void setSlabSummaryOrigin(SlabSummary *summary, Partition *partition)
{
  summary->origin = getFixedLayoutPartitionOffset(partition);
}

// COMBINING FUNCTIONS (LOAD)

/**
 * Clean up after saving out the combined slab summary. This callback is
 * registered in finishLoadingSummary() and loadSlabSummary().
 *
 * @param completion  The extent which was used to write the summary data
 **/
static void finishCombiningZones(VDOCompletion *completion)
{
  SlabSummary *summary = completion->parent;
  int          result  = completion->result;
  VDOExtent   *extent  = asVDOExtent(completion);
  freeExtent(&extent);
  finishCompletion(&summary->completion, result);
}

/**********************************************************************/
void combineZones(SlabSummary *summary)
{
  // Combine all the old summary data into the portion of the buffer
  // corresponding to the first zone.
  ZoneCount zone = 0;
  if (summary->zonesToCombine > 1) {
    for (SlabCount entryNumber = 0; entryNumber < MAX_SLABS; entryNumber++) {
      if (zone != 0) {
        memcpy(summary->entries + entryNumber,
               summary->entries + (zone * MAX_SLABS) + entryNumber,
               sizeof(SlabSummaryEntry));
      }
      zone++;
      if (zone == summary->zonesToCombine) {
        zone = 0;
      }
    }
  }

  // Copy the combined data to each zones's region of the buffer.
  for (zone = 1; zone < MAX_PHYSICAL_ZONES; zone++) {
    memcpy(summary->entries + (zone * MAX_SLABS), summary->entries,
           MAX_SLABS * sizeof(SlabSummaryEntry));
  }
}

/**
 * Combine the slab summary data from all the previously written zones
 * and copy the combined summary to each partition's data region. Then write
 * the combined summary back out to disk. This callback is registered in
 * loadSlabSummary().
 *
 * @param completion  The extent which was used to read the summary data
 **/
static void finishLoadingSummary(VDOCompletion *completion)
{
  SlabSummary *summary = completion->parent;
  int          result  = completion->result;
  VDOExtent   *extent  = asVDOExtent(completion);
  if (result != VDO_SUCCESS) {
    freeExtent(&extent);
    finishCompletion(&summary->completion, result);
    return;
  }

  // Combine the zones so each zone is correct for all slabs.
  combineZones(summary);

  // Write the combined summary back out.
  PhysicalBlockNumber pbn = summary->zones[0]->summaryBlocks[0].pbn;
  extent->completion.callback = finishCombiningZones;
  writeMetadataExtent(extent, pbn);
}

/**********************************************************************/
void loadSlabSummary(SlabSummary *summary,
                     ZoneCount    zonesToCombine,
                     void        *parent,
                     VDOAction   *callback)
{
  resetCompletion(&summary->completion);
  summary->completion.parent   = parent;
  summary->completion.callback = callback;
  summary->zonesToCombine      = zonesToCombine;

  VDOExtent *extent;
  BlockCount blocks = summary->blocksPerZone * MAX_PHYSICAL_ZONES;
  int        result = createExtent(summary->completion.layer,
                                   VIO_TYPE_SLAB_SUMMARY,
                                   VIO_PRIORITY_METADATA, blocks,
                                   (char *) summary->entries, &extent);
  if (result != VDO_SUCCESS) {
    finishCompletion(&summary->completion, result);
    return;
  }

  PhysicalBlockNumber pbn = summary->zones[0]->summaryBlocks[0].pbn;
  if (zonesToCombine == 0) {
    prepareCompletion(&extent->completion, finishCombiningZones,
                      finishCombiningZones, 0, summary);
    writeMetadataExtent(extent, pbn);
    return;
  }

  prepareCompletion(&extent->completion, finishLoadingSummary,
                    finishLoadingSummary, 0, summary);
  readMetadataExtent(extent, pbn);
}

/**********************************************************************/
SlabSummaryStatistics getSlabSummaryStatistics(const SlabSummary *summary)
{
  const AtomicSlabSummaryStatistics *atoms = &summary->statistics;
  return (SlabSummaryStatistics) {
    .blocksWritten = atomicLoad64(&atoms->blocksWritten),
  };
}
