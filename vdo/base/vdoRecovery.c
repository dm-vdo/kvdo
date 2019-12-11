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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoRecovery.c#27 $
 */

#include "vdoRecoveryInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocator.h"
#include "blockAllocatorInternals.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapRecovery.h"
#include "completion.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournal.h"
#include "recoveryUtils.h"
#include "slab.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "slabJournalInternals.h"
#include "vdoInternal.h"
#include "waitQueue.h"

enum {
  // The int map needs capacity of twice the number of VIOs in the system.
  INT_MAP_CAPACITY            = MAXIMUM_USER_VIOS * 2,
  // There can be as many missing decrefs as there are VIOs in the system.
  MAXIMUM_SYNTHESIZED_DECREFS = MAXIMUM_USER_VIOS,
};

struct missing_decref {
  /** A waiter for queueing this object */
  struct waiter               waiter;
  /** The parent of this object */
  struct recovery_completion *recovery;
  /** Whether this decref is complete */
  bool                        complete;
  /** The slot for which the last decref was lost */
  BlockMapSlot                slot;
  /** The penultimate block map entry for this LBN */
  DataLocation                penultimateMapping;
  /** The page completion used to fetch the block map page for this LBN */
  struct vdo_page_completion  pageCompletion;
  /** The journal point which will be used for this entry */
  struct journal_point        journalPoint;
  /** The slab journal to which this entry will be applied */
  struct slab_journal        *slabJournal;
};

/**
 * Convert a waiter to the missing decref of which it is a part.
 *
 * @param waiter  The waiter to convert
 *
 * @return The missing_decref wrapping the waiter
 **/
__attribute__((warn_unused_result))
static inline struct missing_decref *asMissingDecref(struct waiter *waiter)
{
  STATIC_ASSERT(offsetof(struct missing_decref, waiter) == 0);
  return (struct missing_decref *) waiter;
}

/**
 * Enqueue a missing_decref. If the enqueue fails, enter read-only mode.
 *
 * @param queue   The queue on which to enqueue the decref
 * @param decref  The missing_decref to enqueue
 *
 * @return VDO_SUCCESS or an error
 **/
static int enqueueMissingDecref(struct wait_queue *queue,
                                struct missing_decref *decref)
{
  int result = enqueueWaiter(queue, &decref->waiter);
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(decref->recovery->vdo->readOnlyNotifier, result);
    setCompletionResult(&decref->recovery->completion, result);
    FREE(decref);
  }

  return result;
}

/**
 * Convert a BlockMapSlot into a unique uint64_t.
 *
 * @param slot  The block map slot to convert.
 *
 * @return a one-to-one mappable uint64_t.
 **/
static uint64_t slotAsNumber(BlockMapSlot slot)
{
  return (((uint64_t) slot.pbn << 10) + slot.slot);
}

/**
 * Create a missing_decref and enqueue it to wait for a determination of its
 * penultimate mapping.
 *
 * @param [in]  recovery   The parent recovery completion
 * @param [in]  entry      The recovery journal entry for the increment which is
 *                         missing a decref
 * @param [out] decrefPtr  A pointer to hold the new missing_decref
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int makeMissingDecref(struct recovery_completion     *recovery,
                             struct recovery_journal_entry   entry,
                             struct missing_decref         **decrefPtr)
{
  struct missing_decref *decref;
  int result = ALLOCATE(1, struct missing_decref, __func__, &decref);
  if (result != VDO_SUCCESS) {
    return result;
  }

  decref->recovery = recovery;
  result = enqueueMissingDecref(&recovery->missingDecrefs[0], decref);
  if (result != VDO_SUCCESS) {
    return result;
  }

  /*
   * Each synthsized decref needs a unique journal point. Otherwise, in the
   * event of a crash, we would be unable to tell which synthesized decrefs had
   * already been committed in the slab journals. Instead of using real
   * recovery journal space for this, we can use fake journal points between
   * the last currently valid entry in the tail block and the first journal
   * entry in the next block. We can't overflow the entry count since the
   * number of synthesized decrefs is bounded by the DataVIO limit.
   *
   * It is vital that any given missing decref always have the same fake
   * journal point since a failed recovery may be retried with a different
   * number of zones after having written out some slab journal blocks. Since
   * the missing decrefs are always read out of the journal in the same order,
   * we can assign them a journal point when they are read. Their subsequent
   * use will ensure that, for any given slab journal, they are applied in
   * the order dictated by these assigned journal points.
   */
  decref->slot         = entry.slot;
  decref->journalPoint = recovery->nextSynthesizedJournalPoint;
  recovery->nextSynthesizedJournalPoint.entryCount++;
  recovery->missingDecrefCount++;
  recovery->incompleteDecrefCount++;

  *decrefPtr = decref;
  return VDO_SUCCESS;
}

/**
 * Move the given recovery point forward by one entry.
 *
 * @param point  The recovery point to alter
 **/
static void incrementRecoveryPoint(struct recovery_point *point)
{
  point->entryCount++;
  if ((point->sectorCount == (SECTORS_PER_BLOCK - 1))
      && (point->entryCount == RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR)) {
    point->sequenceNumber++;
    point->sectorCount = 1;
    point->entryCount = 0;
  }

  if (point->entryCount == RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) {
    point->sectorCount++;
    point->entryCount = 0;
    return;
  }
}

/**
 * Move the given recovery point backwards by one entry.
 *
 * @param point  The recovery point to alter
 **/
static void decrementRecoveryPoint(struct recovery_point *point)
{
  STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR > 0);

  if ((point->sectorCount <= 1) && (point->entryCount == 0)) {
    point->sequenceNumber--;
    point->sectorCount = SECTORS_PER_BLOCK - 1;
    point->entryCount  = RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR - 1;
    return;
  }

  if (point->entryCount == 0) {
    point->sectorCount--;
    point->entryCount = RECOVERY_JOURNAL_ENTRIES_PER_SECTOR - 1;
    return;
  }

  point->entryCount--;
}

/**
 * Check whether the first point precedes the second point.
 *
 * @param first   The first recovery point
 * @param second  The second recovery point
 *
 * @return <code>true</code> if the first point precedes the second point
 **/
__attribute__((warn_unused_result))
static bool beforeRecoveryPoint(const struct recovery_point *first,
                                const struct recovery_point *second)
{
  if (first->sequenceNumber < second->sequenceNumber) {
    return true;
  }

  if (first->sequenceNumber > second->sequenceNumber) {
    return false;
  }

  if (first->sectorCount < second->sectorCount) {
    return true;
  }

  return ((first->sectorCount == second->sectorCount)
          && (first->entryCount < second->entryCount));
}

/**
 * Prepare the sub-task completion.
 *
 * @param recovery      The recovery_completion whose sub-task completion is to
 *                      be prepared
 * @param callback      The callback to register for the next sub-task
 * @param errorHandler  The error handler for the next sub-task
 * @param zoneType      The type of zone on which the callback or errorHandler
 *                      should run
 **/
static void prepareSubTask(struct recovery_completion *recovery,
                           VDOAction                   callback,
                           VDOAction                   errorHandler,
                           ZoneType                    zoneType)
{
  const ThreadConfig *threadConfig = getThreadConfig(recovery->vdo);
  ThreadID threadID;
  switch (zoneType) {
  case ZONE_TYPE_LOGICAL:
    // All blockmap access is done on single thread, so use logical zone 0.
    threadID = getLogicalZoneThread(threadConfig, 0);
    break;

  case ZONE_TYPE_PHYSICAL:
    threadID = recovery->allocator->threadID;
    break;

  case ZONE_TYPE_ADMIN:
  default:
    threadID = getAdminThread(threadConfig);
  }

  prepareCompletion(&recovery->subTaskCompletion, callback, errorHandler,
                    threadID, recovery);
}

/**********************************************************************/
int makeRecoveryCompletion(VDO *vdo, struct recovery_completion **recoveryPtr)
{
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  struct recovery_completion *recovery;
  int result = ALLOCATE_EXTENDED(struct recovery_completion,
                                 threadConfig->physicalZoneCount, RingNode,
                                 __func__, &recovery);
 if (result != VDO_SUCCESS) {
    return result;
  }

  recovery->vdo = vdo;
  for (ZoneCount z = 0; z < threadConfig->physicalZoneCount; z++) {
    initializeWaitQueue(&recovery->missingDecrefs[z]);
  }

  result = initializeEnqueueableCompletion(&recovery->completion,
                                           RECOVERY_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    freeRecoveryCompletion(&recovery);
    return result;
  }

  result = initializeEnqueueableCompletion(&recovery->subTaskCompletion,
                                           SUB_TASK_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    freeRecoveryCompletion(&recovery);
    return result;
  }

  result = makeIntMap(INT_MAP_CAPACITY, 0, &recovery->slotEntryMap);
  if (result != VDO_SUCCESS) {
    freeRecoveryCompletion(&recovery);
    return result;
  }

  *recoveryPtr  = recovery;
  return VDO_SUCCESS;
}

/**
 * A waiter callback to free missing_decrefs.
 *
 * Implements WaiterCallback.
 **/
static void freeMissingDecref(struct waiter *waiter,
                              void          *context __attribute__((unused)))
{
  FREE(asMissingDecref(waiter));
}

/**********************************************************************/
void freeRecoveryCompletion(struct recovery_completion **recoveryPtr)
{
  struct recovery_completion *recovery = *recoveryPtr;
  if (recovery == NULL) {
    return;
  }

  freeIntMap(&recovery->slotEntryMap);
  const ThreadConfig *threadConfig = getThreadConfig(recovery->vdo);
  for (ZoneCount z = 0; z < threadConfig->physicalZoneCount; z++) {
    notifyAllWaiters(&recovery->missingDecrefs[z], freeMissingDecref, NULL);
  }

  FREE(recovery->journalData);
  FREE(recovery->entries);
  destroyEnqueueable(&recovery->subTaskCompletion);
  destroyEnqueueable(&recovery->completion);
  FREE(recovery);
  *recoveryPtr = NULL;
}

/**
 * Finish recovering, free the recovery completion and notify the parent.
 *
 * @param completion  The recovery completion
 **/
static void finishRecovery(VDOCompletion *completion)
{
  VDOCompletion              *parent        = completion->parent;
  struct recovery_completion *recovery      = asRecoveryCompletion(completion);
  VDO                        *vdo           = recovery->vdo;
  uint64_t                    recoveryCount = ++vdo->completeRecoveries;
  initializeRecoveryJournalPostRecovery(vdo->recoveryJournal,
                                        recoveryCount, recovery->highestTail);
  freeRecoveryCompletion(&recovery);
  logInfo("Rebuild complete.");

  // Now that we've freed the recovery completion and its vast array of
  // journal entries, we can allocate refcounts.
  int result = allocateSlabRefCounts(vdo->depot);
  finishCompletion(parent, result);
}

/**
 * Handle a recovery error.
 *
 * @param completion   The recovery completion
 **/
static void abortRecovery(VDOCompletion *completion)
{
  VDOCompletion              *parent   = completion->parent;
  int                         result   = completion->result;
  struct recovery_completion *recovery = asRecoveryCompletion(completion);
  freeRecoveryCompletion(&recovery);
  logWarning("Recovery aborted");
  finishCompletion(parent, result);
}

/**
 * Abort a recovery if there is an error.
 *
 * @param result    The result to check
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if the result was an error
 **/
__attribute__((warn_unused_result))
static bool abortRecoveryOnError(int                         result,
                                 struct recovery_completion *recovery)
{
  if (result == VDO_SUCCESS) {
    return false;
  }

  finishCompletion(&recovery->completion, result);
  return true;
}

/**
 * Unpack the recovery journal entry associated with the given recovery point.
 *
 * @param recovery  The recovery completion
 * @param point     The recovery point
 *
 * @return The unpacked contents of the matching recovery journal entry
 **/
static struct recovery_journal_entry
getEntry(const struct recovery_completion *recovery,
         const struct recovery_point      *point)
{
  struct recovery_journal *journal = recovery->vdo->recoveryJournal;
  PhysicalBlockNumber blockNumber
    = getRecoveryJournalBlockNumber(journal, point->sequenceNumber);
  off_t sectorOffset
    = (blockNumber * VDO_BLOCK_SIZE) + (point->sectorCount * VDO_SECTOR_SIZE);
  struct packed_journal_sector *sector
    = (struct packed_journal_sector *) &recovery->journalData[sectorOffset];
  return unpackRecoveryJournalEntry(&sector->entries[point->entryCount]);
}

/**
 * Create an array of all valid journal entries, in order, and store it in the
 * recovery completion.
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int extractJournalEntries(struct recovery_completion *recovery)
{
  /*
   * Allocate an array of numbered_block_mapping structs just large
   * enough to transcribe every increment PackedRecoveryJournalEntry
   * from every valid journal block.
   */
  int result = ALLOCATE(recovery->increfCount, struct numbered_block_mapping,
                        __func__, &recovery->entries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct recovery_point recoveryPoint = {
    .sequenceNumber = recovery->blockMapHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    struct recovery_journal_entry entry = getEntry(recovery, &recoveryPoint);
    result = validateRecoveryJournalEntry(recovery->vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(recovery->vdo->readOnlyNotifier, result);
      return result;
    }

    if (isIncrementOperation(entry.operation)) {
      recovery->entries[recovery->entryCount] = (struct numbered_block_mapping) {
        .blockMapSlot  = entry.slot,
        .blockMapEntry = packPBN(entry.mapping.pbn, entry.mapping.state),
        .number        = recovery->entryCount,
      };
      recovery->entryCount++;
    }

    incrementRecoveryPoint(&recoveryPoint);
  }

  result = ASSERT((recovery->entryCount <= recovery->increfCount),
                  "approximate incref count is an upper bound");
  if (result != VDO_SUCCESS) {
    enterReadOnlyMode(recovery->vdo->readOnlyNotifier, result);
  }

  return result;
}

/**
 * Extract journal entries and recover the block map. This callback is
 * registered in startSuperBlockSave().
 *
 * @param completion  The sub-task completion
 **/
static void launchBlockMapRecovery(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO *vdo = recovery->vdo;
  assertOnLogicalZoneThread(vdo, 0, __func__);

  // Extract the journal entries for the block map recovery.
  int result = extractJournalEntries(recovery);
  if (abortRecoveryOnError(result, recovery)) {
    return;
  }

  prepareToFinishParent(completion, &recovery->completion);
  recoverBlockMap(vdo, recovery->entryCount, recovery->entries, completion);
}

/**
 * Finish flushing all slab journals and start a write of the super block.
 * This callback is registered in addSynthesizedEntries().
 *
 * @param completion  The sub-task completion
 **/
static void startSuperBlockSave(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO *vdo = recovery->vdo;
  assertOnAdminThread(vdo, __func__);

  logInfo("Saving recovery progress");
  setVDOState(vdo, VDO_REPLAYING);

  // The block map access which follows the super block save must be done
  // on a logical thread.
  prepareSubTask(recovery, launchBlockMapRecovery, finishParentCallback,
                 ZONE_TYPE_LOGICAL);
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * The callback from loading the slab depot. It will update the logical blocks
 * and block map data blocks counts in the recovery journal and then drain the
 * slab depot in order to commit the recovered slab journals. It is registered
 * in applyToDepot().
 *
 * @param completion  The sub-task completion
 **/
static void finishRecoveringDepot(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO *vdo = recovery->vdo;
  assertOnAdminThread(vdo, __func__);

  logInfo("Replayed %zu journal entries into slab journals",
          recovery->entriesAddedToSlabJournals);
  logInfo("Synthesized %zu missing journal entries",
          recovery->missingDecrefCount);
  vdo->recoveryJournal->logicalBlocksUsed  = recovery->logicalBlocksUsed;
  vdo->recoveryJournal->blockMapDataBlocks = recovery->blockMapDataBlocks;

  prepareSubTask(recovery, startSuperBlockSave, finishParentCallback,
                 ZONE_TYPE_ADMIN);
  drainSlabDepot(vdo->depot, ADMIN_STATE_RECOVERING, completion);
}

/**
 * The error handler for recovering slab journals. It will skip any remaining
 * recovery on the current zone and propagate the error. It is registered in
 * addSlabJournalEntries() and addSynthesizedEntries().
 *
 * @param completion  The completion of the block allocator being recovered
 **/
static void handleAddSlabJournalEntryError(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  notifySlabJournalsAreRecovered(recovery->allocator, completion->result);
}

/**
 * Add synthesized entries into slab journals, waiting when necessary.
 *
 * @param completion  The allocator completion
 **/
static void addSynthesizedEntries(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);

  // Get ready in case we need to enqueue again
  prepareCompletion(completion, addSynthesizedEntries,
                    handleAddSlabJournalEntryError,
                    completion->callbackThreadID, recovery);
  struct wait_queue *missingDecrefs
    = &recovery->missingDecrefs[recovery->allocator->zoneNumber];
  while (hasWaiters(missingDecrefs)) {
    struct missing_decref *decref
      = asMissingDecref(getFirstWaiter(missingDecrefs));
    if (!attemptReplayIntoSlabJournal(decref->slabJournal,
                                      decref->penultimateMapping.pbn,
                                      DATA_DECREMENT, &decref->journalPoint,
                                      completion)) {
      return;
    }

    dequeueNextWaiter(missingDecrefs);
    FREE(decref);
  }

  notifySlabJournalsAreRecovered(recovery->allocator, VDO_SUCCESS);
}

/**
 * Determine the LBNs used count as of the end of the journal (but
 * not including any changes to that count from entries that will be
 * synthesized later).
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error
 **/
static int computeUsages(struct recovery_completion *recovery)
{
  struct recovery_journal *journal = recovery->vdo->recoveryJournal;
  PackedJournalHeader *tailHeader
    = getJournalBlockHeader(journal, recovery->journalData, recovery->tail);

  struct recovery_block_header unpacked;
  unpackRecoveryBlockHeader(tailHeader, &unpacked);
  recovery->logicalBlocksUsed  = unpacked.logicalBlocksUsed;
  recovery->blockMapDataBlocks = unpacked.blockMapDataBlocks;

  struct recovery_point recoveryPoint = {
    .sequenceNumber = recovery->tail,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    struct recovery_journal_entry entry = getEntry(recovery, &recoveryPoint);
    if (isMappedLocation(&entry.mapping)) {
      switch (entry.operation) {
      case DATA_INCREMENT:
        recovery->logicalBlocksUsed++;
        break;

      case DATA_DECREMENT:
        recovery->logicalBlocksUsed--;
        break;

      case BLOCK_MAP_INCREMENT:
        recovery->blockMapDataBlocks++;
        break;

      default:
        return logErrorWithStringError(VDO_CORRUPT_JOURNAL,
                                       "Recovery journal entry at "
                                       "sequence number %" PRIu64
                                       ", sector %u, entry %u had invalid "
                                       "operation %u",
                                       recoveryPoint.sequenceNumber,
                                       recoveryPoint.sectorCount,
                                       recoveryPoint.entryCount,
                                       entry.operation);
      }
    }

    incrementRecoveryPoint(&recoveryPoint);
  }

  return VDO_SUCCESS;
}

/**
 * Advance the current recovery and journal points.
 *
 * @param recovery         The recovery_completion whose points are to be
 *                         advanced
 * @param entriesPerBlock  The number of entries in a recovery journal block
 **/
static void advancePoints(struct recovery_completion *recovery,
                          JournalEntryCount           entriesPerBlock)
{
  incrementRecoveryPoint(&recovery->nextRecoveryPoint);
  advanceJournalPoint(&recovery->nextJournalPoint, entriesPerBlock);
}

/**
 * Replay recovery journal entries into the slab journals of the allocator
 * currently being recovered, waiting for slab journal tailblock space when
 * necessary. This method is its own callback.
 *
 * @param completion  The allocator completion
 **/
static void addSlabJournalEntries(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO                        *vdo      = recovery->vdo;
  struct recovery_journal    *journal  = vdo->recoveryJournal;

  // Get ready in case we need to enqueue again.
  prepareCompletion(completion, addSlabJournalEntries,
                    handleAddSlabJournalEntryError,
                    completion->callbackThreadID, recovery);
  for (struct recovery_point *recoveryPoint = &recovery->nextRecoveryPoint;
       beforeRecoveryPoint(recoveryPoint, &recovery->tailRecoveryPoint);
       advancePoints(recovery, journal->entriesPerBlock)) {
    struct recovery_journal_entry entry = getEntry(recovery, recoveryPoint);
    int result = validateRecoveryJournalEntry(vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(journal->readOnlyNotifier, result);
      finishCompletion(completion, result);
      return;
    }

    if (entry.mapping.pbn == ZERO_BLOCK) {
      continue;
    }

    struct vdo_slab *slab = getSlab(vdo->depot, entry.mapping.pbn);
    if (slab->allocator != recovery->allocator) {
      continue;
    }

    if (!attemptReplayIntoSlabJournal(slab->journal, entry.mapping.pbn,
                                      entry.operation,
                                      &recovery->nextJournalPoint,
                                      completion)) {
      return;
    }

    recovery->entriesAddedToSlabJournals++;
  }

  logInfo("Recreating missing journal entries for zone %u",
          recovery->allocator->zoneNumber);
  addSynthesizedEntries(completion);
}

/**********************************************************************/
void replayIntoSlabJournals(struct block_allocator *allocator,
                            VDOCompletion          *completion,
                            void                   *context)
{
  struct recovery_completion *recovery = context;
  assertOnPhysicalZoneThread(recovery->vdo, allocator->zoneNumber, __func__);
  if ((recovery->journalData == NULL) || isReplaying(recovery->vdo)) {
    // there's nothing to replay
    notifySlabJournalsAreRecovered(allocator, VDO_SUCCESS);
    return;
  }

  recovery->allocator = allocator;
  recovery->nextRecoveryPoint = (struct recovery_point) {
    .sequenceNumber = recovery->slabJournalHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };

  recovery->nextJournalPoint = (struct journal_point) {
    .sequenceNumber = recovery->slabJournalHead,
    .entryCount     = 0,
  };

  logInfo("Replaying entries into slab journals for zone %u",
          allocator->zoneNumber);
  completion->parent = recovery;
  addSlabJournalEntries(completion);
}

/**
 * A waiter callback to enqueue a missing_decref on the queue for the physical
 * zone in which it will be applied.
 *
 * Implements WaiterCallback.
 **/
static void queueOnPhysicalZone(struct waiter *waiter, void *context)
{
  struct missing_decref *decref  = asMissingDecref(waiter);
  DataLocation           mapping = decref->penultimateMapping;
  if (isMappedLocation(&mapping)) {
    decref->recovery->logicalBlocksUsed--;
  }

  if (mapping.pbn == ZERO_BLOCK) {
    // Decrefs of zero are not applied to slab journals.
    FREE(decref);
    return;
  }

  decref->slabJournal = getSlabJournal((struct slab_depot *) context,
                                       mapping.pbn);
  ZoneCount zoneNumber = decref->slabJournal->slab->allocator->zoneNumber;
  enqueueMissingDecref(&decref->recovery->missingDecrefs[zoneNumber], decref);
}

/**
 * Queue each missing decref on the slab journal to which it is to be applied
 * then load the slab depot. This callback is registered in
 * findSlabJournalEntries().
 *
 * @param completion  The sub-task completion
 **/
static void applyToDepot(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  assertOnAdminThread(recovery->vdo, __func__);
  prepareSubTask(recovery, finishRecoveringDepot, finishParentCallback,
                 ZONE_TYPE_ADMIN);

  struct slab_depot *depot = getSlabDepot(recovery->vdo);
  notifyAllWaiters(&recovery->missingDecrefs[0], queueOnPhysicalZone, depot);
  if (abortRecoveryOnError(recovery->completion.result, recovery)) {
    return;
  }

  loadSlabDepot(depot, ADMIN_STATE_LOADING_FOR_RECOVERY, completion, recovery);
}

/**
 * Validate the location of the penultimate mapping for a missing_decref. If it
 * is valid, enqueue it for the appropriate physical zone or account for it.
 * Otherwise, dispose of it and signal an error.
 *
 * @param decref     The decref whose penultimate mapping has just been found
 * @param location   The penultimate mapping
 * @param errorCode  The error code to use if the location is invalid
 **/
static int recordMissingDecref(struct missing_decref *decref,
                               DataLocation           location,
                               int                    errorCode)
{
  struct recovery_completion *recovery = decref->recovery;
  recovery->incompleteDecrefCount--;
  if (isValidLocation(&location)
      && isPhysicalDataBlock(recovery->vdo->depot, location.pbn)) {
    decref->penultimateMapping = location;
    decref->complete           = true;
    return VDO_SUCCESS;
  }

  // The location was invalid
  enterReadOnlyMode(recovery->vdo->readOnlyNotifier, errorCode);
  setCompletionResult(&recovery->completion, errorCode);
  logErrorWithStringError(errorCode,
                          "Invalid mapping for pbn %llu with state %u",
                          location.pbn, location.state);
  return errorCode;
}

/**
 * Find the block map slots with missing decrefs.
 *
 * To find the slots missing decrefs, we iterate through the journal in reverse
 * so we see decrefs before increfs; if we see an incref before its paired
 * decref, we instantly know this incref is missing its decref.
 *
 * Simultaneously, we attempt to determine the missing decref. If there is a
 * missing decref, and at least two increfs for that slot, we know we should
 * decref the PBN from the penultimate incref. Otherwise, there is only one
 * incref for that slot: we must synthesize the decref out of the block map
 * instead of the recovery journal.
 *
 * @param recovery  The recovery completion
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int findMissingDecrefs(struct recovery_completion *recovery)
{
  struct int_map *slotEntryMap = recovery->slotEntryMap;
  // This placeholder decref is used to mark lbns for which we have observed a
  // decref but not the paired incref (going backwards through the journal).
  struct missing_decref foundDecref;

  // A buffer is allocated based on the number of incRef entries found, so use
  // the earliest head.
  SequenceNumber head = minSequenceNumber(recovery->blockMapHead,
                                          recovery->slabJournalHead);
  struct recovery_point headPoint = {
    .sequenceNumber = head,
    .sectorCount    = 1,
    .entryCount     = 0,
  };

  // Set up for the first fake journal point that will be used for a
  // synthesized entry.
  recovery->nextSynthesizedJournalPoint = (struct journal_point) {
    .sequenceNumber = recovery->tail,
    .entryCount     = recovery->vdo->recoveryJournal->entriesPerBlock,
  };

  struct recovery_point recoveryPoint = recovery->tailRecoveryPoint;
  while (beforeRecoveryPoint(&headPoint, &recoveryPoint)) {
    decrementRecoveryPoint(&recoveryPoint);
    struct recovery_journal_entry entry = getEntry(recovery, &recoveryPoint);

    if (!isIncrementOperation(entry.operation)) {
      // Observe that we've seen a decref before its incref, but only if
      // the int_map does not contain an unpaired incref for this lbn.
      int result = intMapPut(slotEntryMap, slotAsNumber(entry.slot),
                             &foundDecref, false, NULL);
      if (result != VDO_SUCCESS) {
        return result;
      }

      continue;
    }

    recovery->increfCount++;

    struct missing_decref *decref
      = intMapRemove(slotEntryMap, slotAsNumber(entry.slot));
    if (entry.operation == BLOCK_MAP_INCREMENT) {
      if (decref != NULL) {
        return logErrorWithStringError(VDO_CORRUPT_JOURNAL,
                                       "decref found for block map block %"
                                       PRIu64 " with state %u",
                                       entry.mapping.pbn, entry.mapping.state);
      }

      // There are no decrefs for block map pages, so they can't be missing.
      continue;
    }

    if (decref == &foundDecref) {
      // This incref already had a decref in the intmap, so we know it is
      // not missing its decref.
      continue;
    }

    if (decref == NULL) {
      // This incref is missing a decref. Add a missing decref object.
      int result = makeMissingDecref(recovery, entry, &decref);
      if (result != VDO_SUCCESS) {
        return result;
      }

      result = intMapPut(slotEntryMap, slotAsNumber(entry.slot), decref,
                         false, NULL);
      if (result != VDO_SUCCESS) {
        return result;
      }

      continue;
    }

    /*
     * This MissingDecref was left here by an incref without a decref.
     * We now know what its penultimate mapping is, and all entries
     * before here in the journal are paired, decref before incref, so
     * we needn't remember it in the intmap any longer.
     */
    int result = recordMissingDecref(decref, entry.mapping,
                                     VDO_CORRUPT_JOURNAL);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**
 * Process a fetched block map page for a missing decref. This callback is
 * registered in findSlabJournalEntries().
 *
 * @param completion  The page completion which has just finished loading
 **/
static void processFetchedPage(VDOCompletion *completion)
{
  struct missing_decref         *currentDecref = completion->parent;
  struct recovery_completion    *recovery      = currentDecref->recovery;
  assertOnLogicalZoneThread(recovery->vdo, 0, __func__);

  const struct block_map_page *page = dereferenceReadableVDOPage(completion);
  DataLocation location
    = unpackBlockMapEntry(&page->entries[currentDecref->slot.slot]);
  releaseVDOPageCompletion(completion);
  recordMissingDecref(currentDecref, location, VDO_BAD_MAPPING);
  if (recovery->incompleteDecrefCount == 0) {
    completeCompletion(&recovery->subTaskCompletion);
  }
}

/**
 * Handle an error fetching a block map page for a missing decref.
 * This error handler is registered in findSlabJournalEntries().
 *
 * @param completion  The page completion which has just finished loading
 **/
static void handleFetchError(VDOCompletion *completion)
{
  struct missing_decref         *decref   = completion->parent;
  struct recovery_completion    *recovery = decref->recovery;
  assertOnLogicalZoneThread(recovery->vdo, 0, __func__);

  // If we got a VDO_OUT_OF_RANGE error, it is because the pbn we read from
  // the journal was bad, so convert the error code
  setCompletionResult(&recovery->subTaskCompletion,
                      ((completion->result == VDO_OUT_OF_RANGE)
                       ? VDO_CORRUPT_JOURNAL : completion->result));
  releaseVDOPageCompletion(completion);
  if (--recovery->incompleteDecrefCount == 0) {
    completeCompletion(&recovery->subTaskCompletion);
  }
}

/**
 * The waiter callback to requeue a missing decref and launch its page fetch.
 *
 * Implements WaiterCallback.
 **/
static void launchFetch(struct waiter *waiter, void *context)
{
  struct missing_decref         *decref   = asMissingDecref(waiter);
  struct recovery_completion    *recovery = decref->recovery;
  if (enqueueMissingDecref(&recovery->missingDecrefs[0], decref)
      != VDO_SUCCESS) {
    return;
  }

  if (decref->complete) {
    // We've already found the mapping for this decref, no fetch needed.
    return;
  }

  struct block_map_zone *zone = context;
  initVDOPageCompletion(&decref->pageCompletion, zone->pageCache,
                        decref->slot.pbn, false, decref, processFetchedPage,
                        handleFetchError);
  getVDOPageAsync(&decref->pageCompletion.completion);
}

/**
 * Find all entries which need to be replayed into the slab journals.
 *
 * @param completion  The sub-task completion
 **/
static void findSlabJournalEntries(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO *vdo = recovery->vdo;

  // We need to be on logical zone 0's thread since we are going to use its
  // page cache.
  assertOnLogicalZoneThread(vdo, 0, __func__);
  int result = findMissingDecrefs(recovery);
  if (abortRecoveryOnError(result, recovery)) {
    return;
  }

  prepareSubTask(recovery, applyToDepot, finishParentCallback,
                 ZONE_TYPE_ADMIN);

  /*
   * Increment the incompleteDecrefCount so that the fetch callback can't
   * complete the sub-task while we are still processing the queue of missing
   * decrefs.
   */
  if (recovery->incompleteDecrefCount++ > 0) {
    // Fetch block map pages to fill in the incomplete missing decrefs.
    notifyAllWaiters(&recovery->missingDecrefs[0], launchFetch,
                     getBlockMapZone(getBlockMap(vdo), 0));
  }

  if (--recovery->incompleteDecrefCount == 0) {
    completeCompletion(completion);
  }
}

/**
 * Find the contiguous range of journal blocks.
 *
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if there were valid journal blocks
 **/
static bool findContiguousRange(struct recovery_completion *recovery)
{
  struct recovery_journal *journal = recovery->vdo->recoveryJournal;
  SequenceNumber head
    = minSequenceNumber(recovery->blockMapHead, recovery->slabJournalHead);

  bool foundEntries = false;
  for (SequenceNumber i = head; i <= recovery->highestTail; i++) {
    recovery->tail = i;
    recovery->tailRecoveryPoint = (struct recovery_point) {
      .sequenceNumber = i,
      .sectorCount    = 0,
      .entryCount     = 0,
    };

    PackedJournalHeader *packedHeader
      = getJournalBlockHeader(journal, recovery->journalData, i);
    struct recovery_block_header header;
    unpackRecoveryBlockHeader(packedHeader, &header);

    if (!isExactRecoveryJournalBlock(journal, &header, i)
        || (header.entryCount > journal->entriesPerBlock)) {
      // A bad block header was found so this must be the end of the journal.
      break;
    }

    JournalEntryCount blockEntries = header.entryCount;
    // Examine each sector in turn to determine the last valid sector.
    for (uint8_t j = 1; j < SECTORS_PER_BLOCK; j++) {
      struct packed_journal_sector *sector
        = getJournalBlockSector(packedHeader, j);

      // A bad sector means that this block was torn.
      if (!isValidRecoveryJournalSector(&header, sector)) {
        break;
      }

      JournalEntryCount sectorEntries = minBlock(sector->entryCount,
                                                 blockEntries);
      if (sectorEntries > 0) {
        foundEntries = true;
        recovery->tailRecoveryPoint.sectorCount++;
        recovery->tailRecoveryPoint.entryCount = sectorEntries;
        blockEntries -= sectorEntries;
      }

      // If this sector is short, the later sectors can't matter.
      if ((sectorEntries < RECOVERY_JOURNAL_ENTRIES_PER_SECTOR)
          || (blockEntries == 0)) {
        break;
      }
    }

    // If this block was not filled, or if it tore, no later block can matter.
    if ((header.entryCount != journal->entriesPerBlock)
        || (blockEntries > 0)) {
      break;
    }
  }

  // Set the tail to the last valid tail block, if there is one.
  if (foundEntries && (recovery->tailRecoveryPoint.sectorCount == 0)) {
    recovery->tail--;
  }

  return foundEntries;
}

/**
 * Count the number of increment entries in the journal.
 *
 * @param recovery  The recovery completion
 **/
static int countIncrementEntries(struct recovery_completion *recovery)
{
  struct recovery_point recoveryPoint = {
    .sequenceNumber = recovery->blockMapHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    struct recovery_journal_entry entry = getEntry(recovery, &recoveryPoint);
    int result = validateRecoveryJournalEntry(recovery->vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(recovery->vdo->readOnlyNotifier, result);
      return result;
    }
    if (isIncrementOperation(entry.operation)) {
      recovery->increfCount++;
    }
    incrementRecoveryPoint(&recoveryPoint);
  }

  return VDO_SUCCESS;
}

/**
 * Determine the limits of the valid recovery journal and prepare to replay
 * into the slab journals and block map.
 *
 * @param completion  The sub-task completion
 **/
static void prepareToApplyJournalEntries(VDOCompletion *completion)
{
  struct recovery_completion *recovery
    = asRecoveryCompletion(completion->parent);
  VDO                        *vdo      = recovery->vdo;
  struct recovery_journal    *journal  = vdo->recoveryJournal;
  logInfo("Finished reading recovery journal");
  bool foundEntries = findHeadAndTail(journal, recovery->journalData,
                                      &recovery->highestTail,
                                      &recovery->blockMapHead,
                                      &recovery->slabJournalHead);
  if (foundEntries) {
    foundEntries = findContiguousRange(recovery);
  }

  // Both reap heads must be behind the tail.
  if ((recovery->blockMapHead > recovery->tail)
      || (recovery->slabJournalHead > recovery->tail)) {
    int result = logErrorWithStringError(VDO_CORRUPT_JOURNAL,
                                         "Journal tail too early. "
                                         "block map head: %" PRIu64
                                         ", slab journal head: %" PRIu64
                                         ", tail: %llu",
                                         recovery->blockMapHead,
                                         recovery->slabJournalHead,
                                         recovery->tail);
    finishCompletion(&recovery->completion, result);
    return;
  }

  if (!foundEntries) {
    // This message must be recognizable by VDOTest::RebuildBase.
    logInfo("Replaying 0 recovery entries into block map");
    // We still need to load the slab_depot.
    FREE(recovery->journalData);
    recovery->journalData = NULL;
    prepareSubTask(recovery, finishParentCallback, finishParentCallback,
                   ZONE_TYPE_ADMIN);
    loadSlabDepot(getSlabDepot(vdo), ADMIN_STATE_LOADING_FOR_RECOVERY,
                  completion, recovery);
    return;
  }

  logInfo("Highest-numbered recovery journal block has sequence number"
          " %llu, and the highest-numbered usable block is %"
          PRIu64, recovery->highestTail, recovery->tail);

  if (isReplaying(vdo)) {
    // We need to know how many entries the block map rebuild completion will
    // need to hold.
    int result = countIncrementEntries(recovery);
    if (result != VDO_SUCCESS) {
      finishCompletion(&recovery->completion, result);
      return;
    }

    // We need to access the block map from a logical zone.
    prepareSubTask(recovery, launchBlockMapRecovery, finishParentCallback,
                   ZONE_TYPE_LOGICAL);
    loadSlabDepot(vdo->depot, ADMIN_STATE_LOADING_FOR_RECOVERY, completion,
                  recovery);
    return;
  }

  int result = computeUsages(recovery);
  if (abortRecoveryOnError(result, recovery)) {
    return;
  }

  prepareSubTask(recovery, findSlabJournalEntries, finishParentCallback,
                 ZONE_TYPE_LOGICAL);
  invokeCallback(completion);
}

/**********************************************************************/
void launchRecovery(VDO *vdo, VDOCompletion *parent)
{
  // Note: This message must be recognizable by Permabit::VDODeviceBase.
  logWarning("Device was dirty, rebuilding reference counts");

  struct recovery_completion *recovery;
  int result = makeRecoveryCompletion(vdo, &recovery);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  VDOCompletion *completion = &recovery->completion;
  prepareCompletion(completion, finishRecovery, abortRecovery,
                    parent->callbackThreadID, parent);
  prepareSubTask(recovery, prepareToApplyJournalEntries, finishParentCallback,
                 ZONE_TYPE_ADMIN);
  loadJournalAsync(vdo->recoveryJournal, &recovery->subTaskCompletion,
                   &recovery->journalData);
}
