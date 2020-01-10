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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slab.c#16 $
 */

#include "slab.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "completion.h"
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
int makeSlab(PhysicalBlockNumber       slabOrigin,
             struct block_allocator   *allocator,
             PhysicalBlockNumber       translation,
             struct recovery_journal  *recoveryJournal,
             SlabCount                 slabNumber,
             bool                      isNew,
             struct vdo_slab         **slabPtr)
{
  struct vdo_slab *slab;
  int result = ALLOCATE(1, struct vdo_slab, __func__, &slab);
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

  result = makeSlabJournal(allocator, slab, recoveryJournal, &slab->journal);
  if (result != VDO_SUCCESS) {
    freeSlab(&slab);
    return result;
  }

  if (isNew) {
    slab->state.state = ADMIN_STATE_NEW;
    result = allocateRefCountsForSlab(slab);
    if (result != VDO_SUCCESS) {
      freeSlab(&slab);
      return result;
    }
  }

  *slabPtr = slab;
  return VDO_SUCCESS;
}

/**********************************************************************/
int allocateRefCountsForSlab(struct vdo_slab *slab)
{
  struct block_allocator *allocator  = slab->allocator;
  const SlabConfig       *slabConfig = getSlabConfig(allocator->depot);

  int result = ASSERT(slab->referenceCounts == NULL,
                      "vdo_slab %u doesn't allocate refcounts twice",
                      slab->slabNumber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return makeRefCounts(slabConfig->dataBlocks, slab, slab->refCountsOrigin,
                       allocator->readOnlyNotifier, &slab->referenceCounts);
}

/**********************************************************************/
void freeSlab(struct vdo_slab **slabPtr)
{
  struct vdo_slab *slab = *slabPtr;
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
ZoneCount getSlabZoneNumber(struct vdo_slab *slab)
{
  return slab->allocator->zoneNumber;
}

/**********************************************************************/
void markSlabReplaying(struct vdo_slab *slab)
{
  if (slab->status == SLAB_REBUILT) {
    slab->status = SLAB_REPLAYING;
  }
}

/**********************************************************************/
void markSlabUnrecovered(struct vdo_slab *slab)
{
  slab->status = SLAB_REQUIRES_SCRUBBING;
}

/**********************************************************************/
BlockCount getSlabFreeBlockCount(const struct vdo_slab *slab)
{
  return getUnreferencedBlockCount(slab->referenceCounts);
}

/**********************************************************************/
int modifySlabReferenceCount(struct vdo_slab            *slab,
                             const struct journal_point *journalPoint,
                             struct reference_operation  operation)
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
int acquireProvisionalReference(struct vdo_slab     *slab,
                                PhysicalBlockNumber  pbn,
                                struct pbn_lock     *lock)
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
int slabBlockNumberFromPBN(struct vdo_slab     *slab,
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
bool shouldSaveFullyBuiltSlab(const struct vdo_slab *slab)
{
  // Write out the refCounts if the slab has written them before, or it has
  // any non-zero reference counts, or there are any slab journal blocks.
  BlockCount dataBlocks = getSlabConfig(slab->allocator->depot)->dataBlocks;
  return (mustLoadRefCounts(slab->allocator->summary, slab->slabNumber)
          || (getSlabFreeBlockCount(slab) != dataBlocks)
          || !isSlabJournalBlank(slab->journal));
}

/**
 * Initiate a slab action.
 *
 * Implements AdminInitiator.
 **/
static void initiateSlabAction(struct admin_state *state)
{
  struct vdo_slab *slab = container_of(state, struct vdo_slab, state);
  if (state->state == ADMIN_STATE_SCRUBBING) {
    slab->status = SLAB_REBUILDING;
    drainSlabJournal(slab->journal);
    return;
  }

  if (is_loading(state)) {
    decodeSlabJournal(slab->journal);
    return;
  }

  if (is_draining(state)) {
    drainSlabJournal(slab->journal);
    return;
  }

  if (is_resuming(state)) {
    queueSlab(slab);
    finish_resuming(state);
    return;
  }

  finish_operation_with_result(state, VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
void startSlabAction(struct vdo_slab       *slab,
                     AdminStateCode         operation,
                     struct vdo_completion *parent)
{
  start_operation_with_waiter(&slab->state, operation, parent,
                              initiateSlabAction);
}

/**********************************************************************/
void notifySlabJournalIsLoaded(struct vdo_slab *slab, int result)
{
  if ((result == VDO_SUCCESS) && is_clean_load(&slab->state)) {
    // Since this is a normal or new load, we don't need the memory to read and
    // process the recovery journal, so we can allocate reference counts now.
    result = allocateRefCountsForSlab(slab);
  }

  finish_loading_with_result(&slab->state, result);
}

/**********************************************************************/
bool isSlabOpen(struct vdo_slab *slab)
{
  return (!is_quiescing(&slab->state) && !is_quiescent(&slab->state));
}

/**********************************************************************/
bool isSlabDraining(struct vdo_slab *slab)
{
  return is_draining(&slab->state);
}

/**********************************************************************/
void notifySlabJournalIsDrained(struct vdo_slab *slab, int result)
{
  if (slab->referenceCounts == NULL) {
    // This can happen when shutting down a VDO that was in read-only mode when
    // loaded.
    notifyRefCountsAreDrained(slab, result);
    return;
  }

  set_operation_result(&slab->state, result);
  drainRefCounts(slab->referenceCounts);
}

/**********************************************************************/
void notifyRefCountsAreDrained(struct vdo_slab *slab, int result)
{
  finish_draining_with_result(&slab->state, result);
}

/**********************************************************************/
bool isSlabResuming(struct vdo_slab *slab)
{
  return is_resuming(&slab->state);
}

/**********************************************************************/
void finishScrubbingSlab(struct vdo_slab *slab)
{
  slab->status = SLAB_REBUILT;
  queueSlab(slab);
  reopenSlabJournal(slab->journal);
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
void dumpSlab(const struct vdo_slab *slab)
{
  if (slab->referenceCounts != NULL) {
    // Terse because there are a lot of slabs to dump and syslog is lossy.
    logInfo("slab %u: P%u, %llu free",
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
