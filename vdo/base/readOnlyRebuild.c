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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyRebuild.c#14 $
 */

#include "readOnlyRebuild.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMapInternals.h"
#include "blockMapRecovery.h"
#include "completion.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalInternals.h"
#include "recoveryUtils.h"
#include "referenceCountRebuild.h"
#include "slabDepot.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

struct read_only_rebuild_completion {
  /** The completion header */
  struct vdo_completion          completion;
  /** A sub task completion */
  struct vdo_completion          subTaskCompletion;
  /** The vdo in question */
  struct vdo                    *vdo;
  /** A buffer to hold the data read off disk */
  char                          *journalData;
  /** The entry data for the block map rebuild */
  struct numbered_block_mapping *entries;
  /** The number of entries in the entry array */
  size_t                         entryCount;
  /** The sequence number of the first valid block of the journal (if known) */
  SequenceNumber                 head;
  /** The sequence number of the last valid block of the journal (if known) */
  SequenceNumber                 tail;
  /** The number of logical blocks in use */
  BlockCount                     logicalBlocksUsed;
  /** The number of allocated block map pages */
  BlockCount                     blockMapDataBlocks;
};

/**
 * Convert a generic completion to a read_only_rebuild_completion.
 *
 * @param completion    The completion to convert
 *
 * @return the journal rebuild completion
 **/
__attribute__((warn_unused_result))
static inline struct read_only_rebuild_completion *
asReadOnlyRebuildCompletion(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct read_only_rebuild_completion, completion) == 0);
  assertCompletionType(completion->type, READ_ONLY_REBUILD_COMPLETION);
  return (struct read_only_rebuild_completion *) completion;
}

/**
 * Free a rebuild completion and all underlying structures.
 *
 * @param rebuildPtr  A pointer to the rebuild completion to free
 */
static void
freeRebuildCompletion(struct read_only_rebuild_completion **rebuildPtr)
{
  struct read_only_rebuild_completion *rebuild = *rebuildPtr;
  if (rebuild == NULL) {
    return;
  }

  destroyEnqueueable(&rebuild->subTaskCompletion);
  FREE(rebuild->journalData);
  FREE(rebuild->entries);
  FREE(rebuild);
  *rebuildPtr = NULL;
}

/**
 * Allocate and initialize a read only rebuild completion.
 *
 * @param [in]  vdo         The vdo in question
 * @param [out] rebuildPtr  A pointer to return the created rebuild completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int
makeRebuildCompletion(struct vdo                           *vdo,
		      struct read_only_rebuild_completion **rebuildPtr)
{
  struct read_only_rebuild_completion *rebuild;
  int result = ALLOCATE(1, struct read_only_rebuild_completion,
                        __func__, &rebuild);
  if (result != VDO_SUCCESS) {
    return result;
  }

  initializeCompletion(&rebuild->completion, READ_ONLY_REBUILD_COMPLETION,
                       vdo->layer);

  result = initializeEnqueueableCompletion(&rebuild->subTaskCompletion,
                                           SUB_TASK_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    freeRebuildCompletion(&rebuild);
    return result;
  }

  rebuild->vdo = vdo;
  *rebuildPtr = rebuild;
  return VDO_SUCCESS;
}

/**
 * Clean up the rebuild process, whether or not it succeeded, by freeing the
 * rebuild completion and notifying the parent of the outcome.
 *
 * @param completion  The rebuild completion
 **/
static void completeRebuild(struct vdo_completion *completion)
{
  struct vdo_completion     *parent  = completion->parent;
  int                        result  = completion->result;
  struct read_only_rebuild_completion *rebuild
                                     = asReadOnlyRebuildCompletion(completion);
  struct vdo                *vdo     = rebuild->vdo;
  setVDOPageCacheRebuildMode(getBlockMap(vdo)->zones[0].pageCache, false);
  freeRebuildCompletion(&rebuild);
  finishCompletion(parent, result);
}

/**
 * Finish rebuilding, free the rebuild completion and notify the parent.
 *
 * @param completion  The rebuild completion
 **/
static void finishRebuild(struct vdo_completion *completion)
{
  struct read_only_rebuild_completion *rebuild
    = asReadOnlyRebuildCompletion(completion);
  initializeRecoveryJournalPostRebuild(rebuild->vdo->recoveryJournal,
                                       rebuild->vdo->completeRecoveries,
                                       rebuild->tail,
                                       rebuild->logicalBlocksUsed,
                                       rebuild->blockMapDataBlocks);
  logInfo("Read-only rebuild complete");
  completeRebuild(completion);
}

/**
 * Handle a rebuild error.
 *
 * @param completion  The rebuild completion
 **/
static void abortRebuild(struct vdo_completion *completion)
{
  logInfo("Read-only rebuild aborted");
  completeRebuild(completion);
}

/**
 * Abort a rebuild if there is an error.
 *
 * @param result   The result to check
 * @param rebuild  The journal rebuild completion
 *
 * @return <code>true</code> if the result was an error
 **/
__attribute__((warn_unused_result))
static bool abortRebuildOnError(int                                  result,
                                struct read_only_rebuild_completion *rebuild)
{
  if (result == VDO_SUCCESS) {
    return false;
  }

  finishCompletion(&rebuild->completion, result);
  return true;
}

/**
 * Clean up after finishing the reference count rebuild. This callback is
 * registered in launchReferenceCountRebuild().
 *
 * @param completion  The sub-task completion
 **/
static void finishReferenceCountRebuild(struct vdo_completion *completion)
{
  struct read_only_rebuild_completion *rebuild = completion->parent;
  struct vdo                          *vdo     = rebuild->vdo;
  assertOnAdminThread(vdo, __func__);
  if (vdo->loadState != VDO_REBUILD_FOR_UPGRADE) {
    // A "rebuild" for upgrade should not increment this count.
    vdo->completeRecoveries++;
  }

  logInfo("Saving rebuilt state");
  prepareToFinishParent(completion, &rebuild->completion);
  drainSlabDepot(vdo->depot, ADMIN_STATE_REBUILDING, completion);
}

/**
 * Rebuild the reference counts from the block map now that all journal entries
 * have been applied to the block map. This callback is registered in
 * applyJournalEntries().
 *
 * @param completion  The sub-task completion
 **/
static void launchReferenceCountRebuild(struct vdo_completion *completion)
{
  struct read_only_rebuild_completion *rebuild = completion->parent;
  struct vdo                          *vdo     = rebuild->vdo;

  // We must allocate RefCounts before we can rebuild them.
  int result = allocateSlabRefCounts(vdo->depot);
  if (abortRebuildOnError(result, rebuild)) {
    return;
  }

  prepareCompletion(completion, finishReferenceCountRebuild,
                    finishParentCallback, getAdminThread(getThreadConfig(vdo)),
                    completion->parent);
  rebuildReferenceCounts(vdo, completion, &rebuild->logicalBlocksUsed,
                         &rebuild->blockMapDataBlocks);
}

/**
 * Append an array of recovery journal entries from a journal block sector to
 * the array of numbered mappings in the rebuild completion, numbering each
 * entry in the order they are appended.
 *
 * @param rebuild     The journal rebuild completion
 * @param sector      The recovery journal sector with entries
 * @param entryCount  The number of entries to append
 **/
static void
appendSectorEntries(struct read_only_rebuild_completion *rebuild,
                    struct packed_journal_sector        *sector,
                    JournalEntryCount                    entryCount)
{
  JournalEntryCount i;
  for (i = 0; i < entryCount; i++) {
    struct recovery_journal_entry entry
      = unpackRecoveryJournalEntry(&sector->entries[i]);
    int result = validateRecoveryJournalEntry(rebuild->vdo, &entry);
    if (result != VDO_SUCCESS) {
      // When recovering from read-only mode, ignore damaged entries.
      continue;
    }

    if (isIncrementOperation(entry.operation)) {
      rebuild->entries[rebuild->entryCount] = (struct numbered_block_mapping) {
        .blockMapSlot  = entry.slot,
        .blockMapEntry = packPBN(entry.mapping.pbn, entry.mapping.state),
        .number        = rebuild->entryCount,
      };
      rebuild->entryCount++;
    }
  }
}

/**
 * Create an array of all valid journal entries, in order, and store
 * it in the rebuild completion.
 *
 * @param rebuild  The journal rebuild completion
 *
 * @return VDO_SUCCESS or an error code
 **/
static int extractJournalEntries(struct read_only_rebuild_completion *rebuild)
{
  struct vdo              *vdo      = rebuild->vdo;
  struct recovery_journal *journal  = vdo->recoveryJournal;
  SequenceNumber           first    = rebuild->head;
  SequenceNumber           last     = rebuild->tail;
  BlockCount               maxCount = ((last - first + 1)
                                       * journal->entriesPerBlock);

  /*
   * Allocate an array of numbered_block_mapping structures large
   * enough to transcribe every PackedRecoveryJournalEntry from every
   * valid journal block.
   */
  int result = ALLOCATE(maxCount, struct numbered_block_mapping, __func__,
                        &rebuild->entries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SequenceNumber i;
  for (i = first; i <= last; i++) {
    PackedJournalHeader *packedHeader
      = getJournalBlockHeader(journal, rebuild->journalData, i);
    struct recovery_block_header header;
    unpackRecoveryBlockHeader(packedHeader, &header);

    if (!isExactRecoveryJournalBlock(journal, &header, i)) {
      // This block is invalid, so skip it.
      continue;
    }

    // Don't extract more than the expected maximum entries per block.
    JournalEntryCount blockEntries = minBlock(journal->entriesPerBlock,
                                              header.entryCount);
    uint8_t j;
    for (j = 1; j < SECTORS_PER_BLOCK; j++) {
      // Stop when all entries counted in the header are applied or skipped.
      if (blockEntries == 0) {
        break;
      }

      struct packed_journal_sector *sector
        = getJournalBlockSector(packedHeader, j);
      if (!isValidRecoveryJournalSector(&header, sector)) {
        blockEntries -= minBlock(blockEntries,
                                 RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
        continue;
      }

      // Don't extract more than the expected maximum entries per sector.
      JournalEntryCount sectorEntries
        = minBlock(sector->entryCount, RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
      // Only extract as many as the block header calls for.
      sectorEntries = minBlock(sectorEntries, blockEntries);
      appendSectorEntries(rebuild, sector, sectorEntries);
      // Even if the sector wasn't full, count it as full when counting up
      // to the entry count the block header claims.
      blockEntries -= minBlock(blockEntries,
                               RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
    }
  }

  return VDO_SUCCESS;
}

/**
 * Determine the limits of the valid recovery journal and apply all
 * valid entries to the block map. This callback is registered in
 * rebuildJournalAsync().
 *
 * @param completion   The sub-task completion
 **/
static void applyJournalEntries(struct vdo_completion *completion)
{
  struct read_only_rebuild_completion *rebuild
    = asReadOnlyRebuildCompletion(completion->parent);
  struct vdo *vdo = rebuild->vdo;

  logInfo("Finished reading recovery journal");
  assertOnLogicalZoneThread(vdo, 0, __func__);

  bool foundEntries = findHeadAndTail(vdo->recoveryJournal,
                                     rebuild->journalData, &rebuild->tail,
                                     &rebuild->head, NULL);
  if (foundEntries) {
    int result = extractJournalEntries(rebuild);
    if (abortRebuildOnError(result, rebuild)) {
      return;
    }
  }

  // Suppress block map errors.
  setVDOPageCacheRebuildMode(getBlockMap(vdo)->zones[0].pageCache, true);

  // Play the recovery journal into the block map.
  prepareCompletion(completion, launchReferenceCountRebuild,
                    finishParentCallback, completion->callbackThreadID,
                    completion->parent);
  recoverBlockMap(vdo, rebuild->entryCount, rebuild->entries, completion);
}

/**
 * Begin loading the journal.
 *
 * @param completion    The sub task completion
 **/
static void loadJournal(struct vdo_completion *completion)
{
  struct read_only_rebuild_completion *rebuild
    = asReadOnlyRebuildCompletion(completion->parent);
  struct vdo *vdo = rebuild->vdo;
  assertOnLogicalZoneThread(vdo, 0, __func__);

  prepareCompletion(completion, applyJournalEntries, finishParentCallback,
                    completion->callbackThreadID, completion->parent);
  loadJournalAsync(vdo->recoveryJournal, completion, &rebuild->journalData);
}

/**********************************************************************/
void launchRebuild(struct vdo *vdo, struct vdo_completion *parent)
{
  // Note: These messages must be recognizable by Permabit::VDODeviceBase.
  if (vdo->loadState == VDO_REBUILD_FOR_UPGRADE) {
    logWarning("Rebuilding reference counts for upgrade");
  } else {
    logWarning("Rebuilding reference counts to clear read-only mode");
    vdo->readOnlyRecoveries++;
  }

  struct read_only_rebuild_completion *rebuild;
  int result = makeRebuildCompletion(vdo, &rebuild);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  struct vdo_completion *completion = &rebuild->completion;
  prepareCompletion(completion, finishRebuild, abortRebuild,
                    parent->callbackThreadID, parent);

  struct vdo_completion *subTaskCompletion = &rebuild->subTaskCompletion;
  prepareCompletion(subTaskCompletion, loadJournal, finishParentCallback,
                    getLogicalZoneThread(getThreadConfig(vdo), 0),
                    completion);
  loadSlabDepot(vdo->depot, ADMIN_STATE_LOADING_FOR_REBUILD,
                subTaskCompletion, NULL);
}
