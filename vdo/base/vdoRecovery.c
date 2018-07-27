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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoRecovery.c#7 $
 */

#include "vdoRecoveryInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "blockMapRecovery.h"
#include "completion.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournal.h"
#include "recoveryUtils.h"
#include "ringNode.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "vdoInternal.h"

enum {
  // The int map needs capacity of twice the number of VIOs in the system.
  INT_MAP_CAPACITY            = MAXIMUM_USER_VIOS * 2,
  // There can be as many missing decrefs as there are VIOs in the system.
  MAXIMUM_SYNTHESIZED_DECREFS = MAXIMUM_USER_VIOS,
};

typedef struct missingDecref {
  /** A ring node, for use in tracking used and unused objects */
  RingNode            ringNode;
  /** The parent of this object */
  RecoveryCompletion *recovery;
  /** The slot for which the last decref was lost */
  BlockMapSlot        slot;
  /** The penultimate block map entry for this LBN */
  DataLocation        penultimateMapping;
  /** The page completion used to fetch the block map page for this LBN */
  VDOPageCompletion   pageCompletion;
} MissingDecref;

/**
 * Convert a RingNode to the missing decref of which it is a part.
 *
 * @param ringNode  The RingNode to convert
 *
 * @return The MissingDecref wrapping the RingNode
 **/
__attribute__((warn_unused_result))
static inline MissingDecref *asMissingDecref(RingNode *ringNode)
{
  STATIC_ASSERT(offsetof(MissingDecref, ringNode) == 0);
  return (MissingDecref *) ringNode;
}

/**
 * Create a MissingDecref.
 *
 * @param recovery   The parent recovery completion
 * @param decrefPtr  A pointer to return the allocated decref object
 *
 * @return VDO_SUCCESS or an error code
 **/
static int makeMissingDecref(RecoveryCompletion  *recovery,
                             MissingDecref      **decrefPtr)
{
  MissingDecref *decref;
  int result = ALLOCATE(1, MissingDecref, __func__, &decref);
  if (result != VDO_SUCCESS) {
    return result;
  }
  decref->recovery = recovery;
  initializeRing(&decref->ringNode);
  *decrefPtr = decref;
  return VDO_SUCCESS;
}

/**
 * Free a MissingDecref object.
 *
 * @param decrefPtr  A pointer to the decref to free
 **/
static void freeMissingDecref(MissingDecref **decrefPtr)
{
  MissingDecref *decref = *decrefPtr;
  if (decref == NULL) {
    return;
  }

  FREE(decref);
  *decrefPtr = NULL;
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
 * Move the given recovery point forward by one entry.
 *
 * @param point  The recovery point to alter
 **/
static void incrementRecoveryPoint(RecoveryPoint *point)
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
static void decrementRecoveryPoint(RecoveryPoint *point)
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
static bool beforeRecoveryPoint(const RecoveryPoint *first,
                                const RecoveryPoint *second)
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

/**********************************************************************/
int makeRecoveryCompletion(VDO *vdo, RecoveryCompletion **recoveryPtr)
{
  RecoveryCompletion *recovery;
  int result = ALLOCATE(1, RecoveryCompletion, __func__, &recovery);
 if (result != VDO_SUCCESS) {
    return result;
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

  initializeRing(&recovery->incompleteDecrefs);
  initializeRing(&recovery->completeDecrefs);
  recovery->vdo = vdo;
  *recoveryPtr  = recovery;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeRecoveryCompletion(RecoveryCompletion **recoveryPtr)
{
  RecoveryCompletion *recovery = *recoveryPtr;
  if (recovery == NULL) {
    return;
  }

  MissingDecref *currentDecref;
  while (!isRingEmpty(&recovery->incompleteDecrefs)) {
    currentDecref = asMissingDecref(popRingNode(&recovery->incompleteDecrefs));
    freeMissingDecref(&currentDecref);
  }

  while (!isRingEmpty(&recovery->completeDecrefs)) {
    currentDecref = asMissingDecref(popRingNode(&recovery->completeDecrefs));
    freeMissingDecref(&currentDecref);
  }

  freeIntMap(&recovery->slotEntryMap);
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
  VDOCompletion      *parent        = completion->parent;
  RecoveryCompletion *recovery      = asRecoveryCompletion(completion);
  VDO                *vdo           = recovery->vdo;
  uint64_t            recoveryCount = ++vdo->completeRecoveries;
  initializeRecoveryJournalPostRecovery(vdo->recoveryJournal,
                                        recoveryCount, recovery->highestTail);
  freeRecoveryCompletion(&recovery);
  logInfo("Rebuild complete.");

  // Now that we've freed the recovery completion and its vast array of
  // journal entries, we can allocate refcounts.
  int result = allocateSlabRefCounts(vdo->depot, vdo->layer);
  finishCompletion(parent, result);
}

/**
 * Handle a recovery error.
 *
 * @param completion   The recovery completion
 **/
static void abortRecovery(VDOCompletion *completion)
{
  VDOCompletion      *parent   = completion->parent;
  int                 result   = completion->result;
  RecoveryCompletion *recovery = asRecoveryCompletion(completion);
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
static bool abortRecoveryOnError(int result, RecoveryCompletion *recovery)
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
static RecoveryJournalEntry getEntry(const RecoveryCompletion *recovery,
                                     const RecoveryPoint      *point)
{
  RecoveryJournal *journal = recovery->vdo->recoveryJournal;
  PhysicalBlockNumber blockNumber
    = getRecoveryJournalBlockNumber(journal, point->sequenceNumber);
  off_t sectorOffset
    = (blockNumber * VDO_BLOCK_SIZE) + (point->sectorCount * VDO_SECTOR_SIZE);
  PackedJournalSector *sector
    = (PackedJournalSector *) &recovery->journalData[sectorOffset];
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
static int extractJournalEntries(RecoveryCompletion *recovery)
{
  // Allocate a NumberedBlockMapping array just large enough to transcribe
  // every increment PackedRecoveryJournalEntry from every valid journal block.
  int result = ALLOCATE(recovery->increfCount, NumberedBlockMapping, __func__,
                        &recovery->entries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  RecoveryPoint recoveryPoint = {
    .sequenceNumber = recovery->blockMapHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    RecoveryJournalEntry entry = getEntry(recovery, &recoveryPoint);
    result = validateRecoveryJournalEntry(recovery->vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(&recovery->vdo->readOnlyContext, result);
      return result;
    }

    if (isIncrementOperation(entry.operation)) {
      recovery->entries[recovery->entryCount] = (NumberedBlockMapping) {
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
    enterReadOnlyMode(&recovery->vdo->readOnlyContext, result);
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
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO                *vdo      = recovery->vdo;
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
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO                *vdo      = recovery->vdo;
  assertOnAdminThread(vdo, __func__);

  logInfo("Saving recovery progress");
  vdo->state = VDO_REPLAYING;

  // The block map access which follows the super block save must be done
  // in logical zone 0
  prepareCompletion(completion, launchBlockMapRecovery, finishParentCallback,
                    getLogicalZoneThread(getThreadConfig(vdo), 0),
                    completion->parent);
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Add synthesized entries into slab journals, waiting when necessary.
 *
 * @param completion  The sub-task completion
 **/
static void addSynthesizedEntries(VDOCompletion *completion)
{
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO *vdo = recovery->vdo;
  // We need to call flushDepotSlabJournals() from the admin thread later.
  assertOnAdminThread(vdo, __func__);

  // Get ready in case we need to enqueue again
  prepareCompletion(completion, addSynthesizedEntries, addSynthesizedEntries,
                    completion->callbackThreadID, &recovery->completion);

  MissingDecref *currentDecref;
  while (!isRingEmpty(&recovery->completeDecrefs)) {
    currentDecref = asMissingDecref(recovery->completeDecrefs.prev);
    DataLocation mapping = currentDecref->penultimateMapping;
    if (!isValidLocation(&mapping)
        || !isPhysicalDataBlock(vdo->depot, mapping.pbn)) {
      // The block map contained a bad mapping, so the block map is corrupt.
      int result = logErrorWithStringError(VDO_BAD_MAPPING,
                                           "Read invalid mapping for pbn %"
                                           PRIu64 " with state %u",
                                           mapping.pbn, mapping.state);
      enterReadOnlyMode(&vdo->readOnlyContext, result);
      finishCompletion(&recovery->completion, result);
      return;
    }

    if (mapping.pbn == ZERO_BLOCK) {
      if (isMappedLocation(&mapping)) {
        recovery->logicalBlocksUsed--;
      }

      popRingNode(&recovery->completeDecrefs);
      freeMissingDecref(&currentDecref);
      continue;
    }

    SlabJournal *slabJournal = getSlabJournal(vdo->depot, mapping.pbn);
    if (!mayAddSlabJournalEntry(slabJournal, DATA_DECREMENT, completion)) {
      return;
    }

    popRingNode(&recovery->completeDecrefs);
    if (isMappedLocation(&mapping)) {
      recovery->logicalBlocksUsed--;
    }

    addSlabJournalEntryForRebuild(slabJournal, mapping.pbn, DATA_DECREMENT,
                                  &recovery->nextJournalPoint);

    /*
     * These journal points may be invalid. Each synthesized decref needs a
     * unique journal point. Otherwise, in the event of a crash, we would be
     * unable to distinguish synthesized decrefs that were actually committed
     * already. Instead of using real recovery journal space for this, we can
     * use fake journal points between the last currently valid entry in the
     * tail block and the first journal entry in the next block.
     */
    recovery->nextJournalPoint.entryCount++;
    freeMissingDecref(&currentDecref);
  }

  logInfo("Synthesized %zu missing journal entries",
          recovery->missingDecrefCount);
  vdo->recoveryJournal->logicalBlocksUsed  = recovery->logicalBlocksUsed;
  vdo->recoveryJournal->blockMapDataBlocks = recovery->blockMapDataBlocks;

  prepareCompletion(completion, startSuperBlockSave, finishParentCallback,
                    completion->callbackThreadID, completion->parent);
  flushDepotSlabJournals(vdo->depot, completion, finishParentCallback,
                         finishParentCallback);
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
static int computeUsages(RecoveryCompletion *recovery)
{
  RecoveryJournal *journal = recovery->vdo->recoveryJournal;
  PackedJournalHeader *tailHeader
    = getJournalBlockHeader(journal, recovery->journalData, recovery->tail);

  RecoveryBlockHeader unpacked;
  unpackRecoveryBlockHeader(tailHeader, &unpacked);
  recovery->logicalBlocksUsed  = unpacked.logicalBlocksUsed;
  recovery->blockMapDataBlocks = unpacked.blockMapDataBlocks;

  RecoveryPoint recoveryPoint = {
    .sequenceNumber = recovery->tail,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    RecoveryJournalEntry entry = getEntry(recovery, &recoveryPoint);
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
 * Apply any synthesized decrefs entries to the appropriate slab journal.
 *
 * @param recovery  The recovery completion
 **/
static void applySynthesizedDecrefs(RecoveryCompletion *recovery)
{
  if (abortRecoveryOnError(recovery->completion.result, recovery)) {
    return;
  }

  logInfo("Recreating missing journal entries");
  int result = computeUsages(recovery);
  if (abortRecoveryOnError(result, recovery)) {
    return;
  }

  recovery->nextJournalPoint  = (JournalPoint) {
    .sequenceNumber = recovery->tail,
    .entryCount     = recovery->vdo->recoveryJournal->entriesPerBlock,
  };
  launchCallbackWithParent(&recovery->subTaskCompletion, addSynthesizedEntries,
                           getAdminThread(getThreadConfig(recovery->vdo)),
                           &recovery->completion);
}

/**
 * Process a fetched block map page for a missing decref.
 *
 * @param completion  The page completion which has just finished loading
 **/
static void processFetchedPage(VDOCompletion *completion)
{
  MissingDecref      *currentDecref = completion->parent;
  RecoveryCompletion *recovery      = currentDecref->recovery;
  assertOnLogicalZoneThread(recovery->vdo, 0, __func__);

  if (completion->result != VDO_SUCCESS) {
    setCompletionResult(&recovery->completion, completion->result);
  } else {
    const BlockMapPage *page = dereferenceReadableVDOPage(completion);
    currentDecref->penultimateMapping
      = unpackBlockMapEntry(&page->entries[currentDecref->slot.slot]);
  }

  releaseVDOPageCompletion(completion);
  recovery->outstanding--;
  if (recovery->outstanding == 0) {
    applySynthesizedDecrefs(recovery);
  }
}

/**
 * Fetch the mapping for missing decrefs whose previous mapping is unknown.
 *
 * @param recovery  The recovery completion
 **/
static void findPBNsFromBlockMap(RecoveryCompletion *recovery)
{
  assertOnLogicalZoneThread(recovery->vdo, 0, __func__);

  // Initialize page completions for MissingDecref objects that need fetches.
  BlockMap      *blockMap        = getBlockMap(recovery->vdo);
  BlockMapZone  *zone            = getBlockMapZone(blockMap, 0);
  RingNode      *currentRingNode = recovery->incompleteDecrefs.next;
  MissingDecref *currentDecref;

  while (currentRingNode != &recovery->incompleteDecrefs) {
    currentDecref = asMissingDecref(currentRingNode);
    initVDOPageCompletion(&currentDecref->pageCompletion, zone->pageCache,
                          currentDecref->slot.pbn, false, currentDecref,
                          processFetchedPage, processFetchedPage);
    recovery->outstanding++;
    currentRingNode = currentRingNode->next;
  }

  // If there are no pages that need fetches, proceed directly to finishing.
  if (recovery->outstanding == 0) {
    applySynthesizedDecrefs(recovery);
    return;
  }

  // Fetch the pages needed.
  while (!isRingEmpty(&recovery->incompleteDecrefs)) {
    currentDecref = asMissingDecref(popRingNode(&recovery->incompleteDecrefs));
    pushRingNode(&recovery->completeDecrefs, &currentDecref->ringNode);
    getVDOPageAsync(&currentDecref->pageCompletion.completion);
  }
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
static int findMissingDecrefs(RecoveryCompletion *recovery)
{
  IntMap *slotEntryMap = recovery->slotEntryMap;
  // This placeholder decref is used to mark lbns for which we have observed a
  // decref but not the paired incref (going backwards through the journal).
  MissingDecref foundDecref;

  // A buffer is allocated based on the number of incRef entries found, so use
  // the earliest head.
  SequenceNumber head = minSequenceNumber(recovery->blockMapHead,
                                          recovery->slabJournalHead);
  RecoveryPoint headPoint = {
    .sequenceNumber = head,
    .sectorCount    = 1,
    .entryCount     = 0,
  };

  RecoveryPoint recoveryPoint = recovery->tailRecoveryPoint;
  while (beforeRecoveryPoint(&headPoint, &recoveryPoint)) {
    decrementRecoveryPoint(&recoveryPoint);
    RecoveryJournalEntry entry = getEntry(recovery, &recoveryPoint);

    if (!isIncrementOperation(entry.operation)) {
      // Observe that we've seen a decref before its incref, but only if
      // the IntMap does not contain an unpaired incref for this lbn.
      int result = intMapPut(slotEntryMap, slotAsNumber(entry.slot),
                             &foundDecref, false, NULL);
      if (result != VDO_SUCCESS) {
        return result;
      }
      continue;
    }

    recovery->increfCount++;

    MissingDecref *decref
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
      int result = makeMissingDecref(recovery, &decref);
      if (result != VDO_SUCCESS) {
        return result;
      }
      recovery->missingDecrefCount++;
      pushRingNode(&recovery->incompleteDecrefs, &decref->ringNode);
      decref->slot = entry.slot;
      result = intMapPut(slotEntryMap, slotAsNumber(entry.slot),
                         decref, false, NULL);
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
    decref->penultimateMapping = entry.mapping;
    pushRingNode(&recovery->completeDecrefs, &decref->ringNode);
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
void addSlabJournalEntries(VDOCompletion *completion)
{
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO                *vdo      = recovery->vdo;
  RecoveryJournal    *journal  = vdo->recoveryJournal;
  // We want to be on the logical thread so that findPBNsFromBlockMap() later
  // is on the right thread.
  assertOnLogicalZoneThread(vdo, 0, __func__);

  // Get ready in case we need to enqueue again.
  prepareCompletion(completion, addSlabJournalEntries, addSlabJournalEntries,
                    completion->callbackThreadID, &recovery->completion);

  while (beforeRecoveryPoint(&recovery->nextRecoveryPoint,
                             &recovery->tailRecoveryPoint)) {
    RecoveryJournalEntry entry
      = getEntry(recovery, &recovery->nextRecoveryPoint);
    int result = validateRecoveryJournalEntry(vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(journal->readOnlyContext, result);
      finishCompletion(&recovery->completion, result);
      return;
    }

    if (entry.mapping.pbn == ZERO_BLOCK) {
      incrementRecoveryPoint(&recovery->nextRecoveryPoint);
      advanceJournalPoint(&recovery->nextJournalPoint,
                          journal->entriesPerBlock);
      continue;
    }

    SlabJournal *slabJournal = getSlabJournal(vdo->depot, entry.mapping.pbn);
    if (!mayAddSlabJournalEntry(slabJournal, entry.operation, completion)) {
      return;
    }

    addSlabJournalEntryForRebuild(slabJournal,
                                  entry.mapping.pbn, entry.operation,
                                  &recovery->nextJournalPoint);
    incrementRecoveryPoint(&recovery->nextRecoveryPoint);
    advanceJournalPoint(&recovery->nextJournalPoint, journal->entriesPerBlock);
    recovery->entriesAddedToSlabJournals++;
  }

  logInfo("Replayed %zu journal entries into slab journals",
          recovery->entriesAddedToSlabJournals);
  int result = findMissingDecrefs(recovery);
  if (abortRecoveryOnError(result, recovery)) {
    return;
  }

  findPBNsFromBlockMap(recovery);
}

/**
 * Find the contiguous range of journal blocks.
 *
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if there were valid journal blocks
 **/
static bool findContiguousRange(RecoveryCompletion *recovery)
{
  RecoveryJournal *journal = recovery->vdo->recoveryJournal;
  SequenceNumber head
    = minSequenceNumber(recovery->blockMapHead, recovery->slabJournalHead);

  bool foundEntries = false;
  for (SequenceNumber i = head; i <= recovery->highestTail; i++) {
    recovery->tail = i;
    recovery->tailRecoveryPoint = (RecoveryPoint) {
      .sequenceNumber = i,
      .sectorCount    = 0,
      .entryCount     = 0,
    };

    PackedJournalHeader *packedHeader
      = getJournalBlockHeader(journal, recovery->journalData, i);
    RecoveryBlockHeader header;
    unpackRecoveryBlockHeader(packedHeader, &header);

    if (!isExactRecoveryJournalBlock(journal, &header, i)
        || (header.entryCount > journal->entriesPerBlock)) {
      // A bad block header was found so this must be the end of the journal.
      break;
    }

    JournalEntryCount blockEntries = header.entryCount;
    // Examine each sector in turn to determine the last valid sector.
    for (uint8_t j = 1; j < SECTORS_PER_BLOCK; j++) {
      PackedJournalSector *sector = getJournalBlockSector(packedHeader, j);

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
static int countIncrementEntries(RecoveryCompletion *recovery)
{
  RecoveryPoint recoveryPoint = {
    .sequenceNumber = recovery->blockMapHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };
  while (beforeRecoveryPoint(&recoveryPoint, &recovery->tailRecoveryPoint)) {
    RecoveryJournalEntry entry = getEntry(recovery, &recoveryPoint);
    int result = validateRecoveryJournalEntry(recovery->vdo, &entry);
    if (result != VDO_SUCCESS) {
      enterReadOnlyMode(&recovery->vdo->readOnlyContext, result);
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
 * Determine the limits of the valid recovery journal and apply all
 * valid entries to the block map. This callback is registered in
 * recoverJournalAsync().
 *
 * @param completion  The journal load completion
 **/
static void applyJournalEntries(VDOCompletion *completion)
{
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO                *vdo      = recovery->vdo;
  RecoveryJournal    *journal  = vdo->recoveryJournal;
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
                                         ", tail: %" PRIu64,
                                         recovery->blockMapHead,
                                         recovery->slabJournalHead,
                                         recovery->tail);
    finishCompletion(&recovery->completion, result);
    return;
  }

  if (!foundEntries) {
    // This message must be recognizable by VDOTest::RebuildBase.
    logInfo("Replaying 0 recovery entries into block map");
    finishCompletion(&recovery->completion, VDO_SUCCESS);
    return;
  }

  logInfo("Highest-numbered recovery journal block has sequence number"
          " %" PRIu64 ", and the highest-numbered usable block is %"
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
    launchCallbackWithParent(&recovery->subTaskCompletion,
                             launchBlockMapRecovery,
                             getLogicalZoneThread(getThreadConfig(vdo), 0),
                             &recovery->completion);
    return;
  }

  logInfo("Replaying entries into slab journals");
  recovery->nextRecoveryPoint = (RecoveryPoint) {
    .sequenceNumber = recovery->slabJournalHead,
    .sectorCount    = 1,
    .entryCount     = 0,
  };

  recovery->nextJournalPoint = (JournalPoint) {
    .sequenceNumber = recovery->slabJournalHead,
    .entryCount     = 0,
  };

  // This doesn't care what thread it's on.
  launchCallbackWithParent(&recovery->subTaskCompletion, addSlabJournalEntries,
                           getCallbackThreadID(), &recovery->completion);
}

/**
 * Begin loading the journal.
 *
 * @param completion    The sub task completion
 **/
static void loadJournal(VDOCompletion *completion)
{
  RecoveryCompletion *recovery = asRecoveryCompletion(completion->parent);
  VDO                *vdo      = recovery->vdo;
  assertOnLogicalZoneThread(vdo, 0, __func__);

  prepareCompletion(completion, applyJournalEntries, finishParentCallback,
                    getLogicalZoneThread(getThreadConfig(vdo), 0),
                    completion->parent);
  loadJournalAsync(vdo->recoveryJournal, completion, &recovery->journalData);
}

/**********************************************************************/
void launchRecovery(VDO *vdo, VDOCompletion *parent)
{
  // Note: This message must be recognizable by Permabit::VDODeviceBase.
  logWarning("Device was dirty, rebuilding reference counts");

  RecoveryCompletion *recovery;
  int result = makeRecoveryCompletion(vdo, &recovery);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  VDOCompletion *completion = &recovery->completion;
  prepareCompletion(completion, finishRecovery, abortRecovery,
                    parent->callbackThreadID, parent);

  VDOCompletion *subTaskCompletion = &recovery->subTaskCompletion;
  prepareCompletion(subTaskCompletion, loadJournal, finishParentCallback,
                    getLogicalZoneThread(getThreadConfig(vdo), 0),
                    completion);
  loadSlabDepotForRecovery(vdo->depot, subTaskCompletion);
}
