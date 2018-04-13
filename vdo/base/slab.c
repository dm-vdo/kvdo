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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slab.c#1 $
 */

#include "slab.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocatorInternals.h"
#include "constants.h"
#include "numUtils.h"
#include "pbnLock.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "slabJournalInternals.h"
#include "slabSummary.h"

/**********************************************************************/
int configureSlab(BlockCount  slabSize,
                  BlockCount  slabJournalBlocks,
                  SlabConfig *slabConfig)
{
  if (slabJournalBlocks >= slabSize) {
    return VDO_BAD_CONFIGURATION;
  }

  /*
   * This calculation should technically be a recurrence, but the total number
   * of metadata blocks is currently less than a single block of refCounts, so
   * we'd gain at most one data block in each slab with more iteration.
   */
  BlockCount refBlocks
    = getSavedReferenceCountSize(slabSize - slabJournalBlocks);
  BlockCount metaBlocks = (refBlocks + slabJournalBlocks);

  // Make sure test code hasn't configured slabs to be too small.
  if (metaBlocks >= slabSize) {
    return VDO_BAD_CONFIGURATION;
  }

  /*
   * If the slab size is very small, assume this must be a unit test and
   * override the number of data blocks to be a power of two (wasting blocks
   * in the slab). Many tests need their dataBlocks fields to be the exact
   * capacity of the configured volume, and that used to fall out since they
   * use a power of two for the number of data blocks, the slab size was a
   * power of two, and every block in a slab was a data block.
   *
   * XXX Try to figure out some way of structuring testParameters and unit
   * tests so this hack isn't needed without having to edit several unit tests
   * every time the metadata size changes by one block.
   */
  BlockCount dataBlocks = slabSize - metaBlocks;
  if ((slabSize < 1024) && !isPowerOfTwo(dataBlocks)) {
    dataBlocks = ((BlockCount) 1 << logBaseTwo(dataBlocks));
  }

  /*
   * Configure the slab journal thresholds. The flush threshold is 168 of 224
   * blocks in production, or 3/4ths, so we use this ratio for all sizes.
   */
  BlockCount flushingThreshold = ((slabJournalBlocks * 3) + 3) / 4;
  /*
   * The blocking threshold should be far enough from the the flushing
   * threshold to not produce delays, but far enough from the end of the
   * journal to allow multiple successive recovery failures.
   */
  BlockCount remaining = slabJournalBlocks - flushingThreshold;
  BlockCount blockingThreshold = flushingThreshold + ((remaining * 5) / 7);
  /*
   * The scrubbing threshold should be at least 2048 entries before the end of
   * the journal.
   */
  BlockCount minimalExtraSpace
    = 1 + (MAXIMUM_USER_VIOS / SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK);
  BlockCount scrubbingThreshold = blockingThreshold;
  if (slabJournalBlocks > minimalExtraSpace) {
    scrubbingThreshold = slabJournalBlocks - minimalExtraSpace;
  }
  if (blockingThreshold > scrubbingThreshold) {
    blockingThreshold = scrubbingThreshold;
  }

  *slabConfig = (SlabConfig) {
    .slabBlocks                    = slabSize,
    .dataBlocks                    = dataBlocks,
    .referenceCountBlocks          = refBlocks,
    .slabJournalBlocks             = slabJournalBlocks,
    .slabJournalFlushingThreshold  = flushingThreshold,
    .slabJournalBlockingThreshold  = blockingThreshold,
    .slabJournalScrubbingThreshold = scrubbingThreshold
  };
  return VDO_SUCCESS;
}

/**********************************************************************/
PhysicalBlockNumber getSlabJournalStartBlock(const SlabConfig    *slabConfig,
                                             PhysicalBlockNumber  origin)
{
  return origin + slabConfig->dataBlocks + slabConfig->referenceCountBlocks;
}

/**********************************************************************/
int makeSlab(PhysicalBlockNumber   slabOrigin,
             BlockAllocator       *allocator,
             PhysicalBlockNumber   translation,
             RecoveryJournal      *recoveryJournal,
             PhysicalLayer        *layer,
             SlabCount             slabNumber,
             Slab                **slabPtr)
{
  Slab *slab;
  int result = ALLOCATE(1, Slab, __func__, &slab);
  if (result != VDO_SUCCESS) {
    return result;
  }

  const SlabConfig *slabConfig = getSlabConfig(allocator->depot);

  slab->allocator  = allocator;
  slab->start      = slabOrigin;
  slab->end        = slab->start + slabConfig->slabBlocks;
  slab->slabNumber = slabNumber;
  initializeRing(&slab->ringNode);

  slab->refCountsOrigin = slabOrigin + slabConfig->dataBlocks + translation;
  slab->journalOrigin   = (getSlabJournalStartBlock(slabConfig, slabOrigin)
                           + translation);

  result = makeSlabJournal(allocator, slab, layer, recoveryJournal,
                           &slab->journal);
  if (result != VDO_SUCCESS) {
    freeSlab(&slab);
    return result;
  }

  *slabPtr = slab;
  return VDO_SUCCESS;
}

/**********************************************************************/
int allocateRefCountsForSlab(PhysicalLayer *layer, Slab *slab)
{
  BlockAllocator   *allocator  = slab->allocator;
  const SlabConfig *slabConfig = getSlabConfig(allocator->depot);

  int result = ASSERT(slab->referenceCounts == NULL,
                      "Slab %u doesn't allocate refcounts twice",
                      slab->slabNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeRefCounts(layer, slabConfig->dataBlocks, slab,
                         slab->refCountsOrigin,
                         allocator->readOnlyContext,
                         &slab->referenceCounts);
  return result;
}

/**********************************************************************/
void freeSlab(Slab **slabPtr)
{
  Slab *slab = *slabPtr;
  if (slab == NULL) {
    return;
  }

  unspliceRingNode(&slab->ringNode);
  freeSlabJournal(&slab->journal);
  freeRefCounts(&slab->referenceCounts);
  FREE(slab);
  *slabPtr = NULL;
}

/**********************************************************************/
ZoneCount getSlabZoneNumber(Slab *slab)
{
  return slab->allocator->zoneNumber;
}

/**********************************************************************/
void markSlabReplaying(Slab *slab)
{
  if (slab->status == SLAB_REBUILT) {
    slab->status = SLAB_REPLAYING;
  }
}

/**********************************************************************/
void markSlabUnrecovered(Slab *slab)
{
  slab->status = SLAB_REQUIRES_SCRUBBING;
}

/**********************************************************************/
BlockCount getSlabFreeBlockCount(const Slab *slab)
{
  return getUnreferencedBlockCount(slab->referenceCounts);
}

/**********************************************************************/
int modifySlabReferenceCount(Slab               *slab,
                             const JournalPoint *journalPoint,
                             ReferenceOperation  operation)
{
  if (slab == NULL) {
    return VDO_SUCCESS;
  }

  /*
   * If the slab is unrecovered, preserve the refCount state and let scrubbing
   * correct the refCount. Note that the slab journal has already captured all
   * refCount updates.
   */
  if (isUnrecoveredSlab(slab)) {
    SequenceNumber entryLock = journalPoint->sequenceNumber;
    adjustSlabJournalBlockReference(slab->journal, entryLock, -1);
    return VDO_SUCCESS;
  }

  bool freeStatusChanged;
  int result = adjustReferenceCount(slab->referenceCounts, operation,
                                    journalPoint, &freeStatusChanged);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (freeStatusChanged) {
    adjustFreeBlockCount(slab, !isIncrementOperation(operation.type));
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int acquireProvisionalReference(Slab                *slab,
                                PhysicalBlockNumber  pbn,
                                PBNLock             *lock)
{
  if (hasProvisionalReference(lock)) {
    return VDO_SUCCESS;
  }

  int result = provisionallyReferenceBlock(slab->referenceCounts, pbn, lock);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (hasProvisionalReference(lock)) {
    adjustFreeBlockCount(slab, false);
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int slabBlockNumberFromPBN(Slab                *slab,
                           PhysicalBlockNumber  physicalBlockNumber,
                           SlabBlockNumber     *slabBlockNumberPtr)
{
  if (physicalBlockNumber < slab->start) {
    return VDO_OUT_OF_RANGE;
  }

  uint64_t slabBlockNumber = physicalBlockNumber - slab->start;
  if (slabBlockNumber >= getSlabConfig(slab->allocator->depot)->dataBlocks) {
    return VDO_OUT_OF_RANGE;
  }

  *slabBlockNumberPtr = slabBlockNumber;
  return VDO_SUCCESS;
}

/**********************************************************************/
bool shouldSaveFullyBuiltSlab(const Slab *slab)
{
  // Write out the refCounts if the slab has written them before, or it has
  // any non-zero reference counts, or there are any slab journal blocks.
  BlockCount dataBlocks = getSlabConfig(slab->allocator->depot)->dataBlocks;
  return (mustLoadRefCounts(slab->allocator->summary, slab->slabNumber)
          || (getSlabFreeBlockCount(slab) != dataBlocks)
          || !isSlabJournalBlank(slab->journal));
}

/**********************************************************************/
static const char *statusToString(SlabRebuildStatus status)
{
  switch (status) {
  case SLAB_REBUILT:
    return "REBUILT";
  case SLAB_REQUIRES_SCRUBBING:
    return "SCRUBBING";
  case SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING:
    return "PRIORITY_SCRUBBING";
  case SLAB_REBUILDING:
    return "REBUILDING";
  case SLAB_REPLAYING:
    return "REPLAYING";
  default:
    return "UNKNOWN";
  }
}

/**********************************************************************/
void dumpSlab(const Slab *slab)
{
  if (slab->referenceCounts != NULL) {
    // Terse because there are a lot of slabs to dump and syslog is lossy.
    logInfo("slab %u: P%u, %" PRIu64 " free",
            slab->slabNumber, slab->priority, getSlabFreeBlockCount(slab));
  } else {
    logInfo("slab %u: status %s", slab->slabNumber,
            statusToString(slab->status));
  }

  dumpSlabJournal(slab->journal);

  if (slab->referenceCounts != NULL) {
    dumpRefCounts(slab->referenceCounts);
  } else {
    logInfo("refCounts is null");
  }
}
