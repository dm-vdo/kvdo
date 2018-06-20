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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/referenceCountRebuild.c#4 $
 */

#include "referenceCountRebuild.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "forest.h"
#include "constants.h"
#include "numUtils.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

/**
 * A reference count rebuild completion.
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the rebuild frees them.
 **/
typedef struct {
  /** completion header */
  VDOCompletion      completion;
  /** the completion for flushing the block map */
  VDOCompletion      subTaskCompletion;
  /** the thread on which all block map operations must be done */
  ThreadID           logicalThreadID;
  /** the admin thread */
  ThreadID           adminThreadID;
  /** the block map */
  BlockMap          *blockMap;
  /** the slab depot */
  SlabDepot         *depot;
  /** whether this recovery has been aborted */
  bool               aborted;
  /** whether we are currently launching the initial round of requests */
  bool               launching;
  /** The number of logical blocks observed used */
  BlockCount        *logicalBlocksUsed;
  /** The number of block map data blocks */
  BlockCount        *blockMapDataBlocks;
  /** the next page to fetch */
  PageCount          pageToFetch;
  /** the number of leaf pages in the block map */
  PageCount          leafPages;
  /** the last slot of the block map */
  BlockMapSlot       lastSlot;
  /** number of pending (non-ready) requests*/
  PageCount          outstanding;
  /** number of page completions */
  PageCount          pageCount;
  /** array of requested, potentially ready page completions */
  VDOPageCompletion  pageCompletions[];
} RebuildCompletion;

/**
 * Convert a VDOCompletion to a RebuildCompletion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a RebuildCompletion
 **/
__attribute__((warn_unused_result))
static inline RebuildCompletion *asRebuildCompletion(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(RebuildCompletion, completion) == 0);
  assertCompletionType(completion->type, REFERENCE_COUNT_REBUILD_COMPLETION);
  return (RebuildCompletion *) completion;
}

/**
 * Free a RebuildCompletion and null out the reference to it.
 *
 * @param completionPtr  a pointer to the completion to free
 **/
static void freeRebuildCompletion(VDOCompletion **completionPtr)
{
  VDOCompletion *completion = *completionPtr;
  if (completion == NULL) {
    return;
  }

  RebuildCompletion *rebuild = asRebuildCompletion(completion);
  destroyEnqueueable(&rebuild->subTaskCompletion);
  destroyEnqueueable(completion);
  FREE(rebuild);
  *completionPtr = NULL;
}

/**
 * Free the RebuildCompletion and notify the parent that the block map
 * rebuild is done. This callback is registered in rebuildBlockMap().
 *
 * @param completion  The RebuildCompletion
 **/
static void finishRebuild(VDOCompletion *completion)
{
  int            result = completion->result;
  VDOCompletion *parent = completion->parent;
  freeRebuildCompletion(&completion);
  finishCompletion(parent, result);
}

/**
 * Make a new rebuild completion.
 *
 * @param [in]  vdo                 The VDO
 * @param [in]  logicalBlocksUsed   A pointer to hold the logical blocks used
 * @param [in]  blockMapDataBlocks  A pointer to hold the number of block map
 *                                  data blocks
 * @param [in]  parent              The parent of the rebuild completion
 * @param [out] rebuildPtr          The new block map rebuild completion
 *
 * @return a success or error code
 **/
static int makeRebuildCompletion(VDO                *vdo,
                                 BlockCount         *logicalBlocksUsed,
                                 BlockCount         *blockMapDataBlocks,
                                 VDOCompletion      *parent,
                                 RebuildCompletion **rebuildPtr)
{
  BlockMap *blockMap = getBlockMap(vdo);
  PageCount pageCount
    = minPageCount(getConfiguredCacheSize(vdo) >> 1,
                   MAXIMUM_SIMULTANEOUS_BLOCK_MAP_RESTORATION_READS);

  RebuildCompletion *rebuild;
  int result = ALLOCATE_EXTENDED(RebuildCompletion, pageCount,
                                 VDOPageCompletion, __func__, &rebuild);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&rebuild->completion,
                                           REFERENCE_COUNT_REBUILD_COMPLETION,
                                           vdo->layer);
  if (result != VDO_SUCCESS) {
    VDOCompletion *completion = &rebuild->completion;
    freeRebuildCompletion(&completion);
    return result;
  }

  result = initializeEnqueueableCompletion(&rebuild->subTaskCompletion,
                                           SUB_TASK_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    VDOCompletion *completion = &rebuild->completion;
    freeRebuildCompletion(&completion);
    return result;
  }

  rebuild->blockMap           = blockMap;
  rebuild->depot              = vdo->depot;
  rebuild->logicalBlocksUsed  = logicalBlocksUsed;
  rebuild->blockMapDataBlocks = blockMapDataBlocks;
  rebuild->pageCount          = pageCount;
  rebuild->leafPages          = computeBlockMapPageCount(blockMap->entryCount);

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  rebuild->logicalThreadID         = getLogicalZoneThread(threadConfig, 0);
  rebuild->adminThreadID           = getAdminThread(threadConfig);

  ASSERT_LOG_ONLY((getCallbackThreadID() == rebuild->logicalThreadID),
                  "%s must be called on logical thread %u (not %u)", __func__,
                  rebuild->logicalThreadID, getCallbackThreadID());
  prepareCompletion(&rebuild->completion, finishRebuild, finishRebuild,
                    rebuild->logicalThreadID, parent);

  *rebuildPtr = rebuild;
  return VDO_SUCCESS;
}

/**
 * Flush the block map now that all the reference counts are rebuilt. This
 * callback is registered in finishIfDone().
 *
 * @param completion  The sub-task completion
 **/
static void flushBlockMapUpdates(VDOCompletion *completion)
{
  logInfo("Flushing block map changes");
  prepareToFinishParent(completion, completion->parent);
  flushBlockMap(asRebuildCompletion(completion->parent)->blockMap, completion);
}

/**
 * Check whether the rebuild is done. If it succeeded, continue by flushing the
 * block map.
 *
 * @param rebuild  The rebuild completion
 *
 * @return <code>true</code> if the rebuild is complete
 **/
static bool finishIfDone(RebuildCompletion *rebuild)
{
  if (rebuild->launching || (rebuild->outstanding > 0)) {
    return false;
  }

  if (rebuild->aborted) {
    completeCompletion(&rebuild->completion);
    return true;
  }

  if (rebuild->pageToFetch < rebuild->leafPages) {
    return false;
  }

  prepareCompletion(&rebuild->subTaskCompletion, flushBlockMapUpdates,
                    finishParentCallback, rebuild->adminThreadID, rebuild);
  invokeCallback(&rebuild->subTaskCompletion);
  return true;
}

/**
 * Record that there has been an error during the rebuild.
 *
 * @param rebuild  The rebuild completion
 * @param result   The error result to use, if one is not already saved
 **/
static void abortRebuild(RebuildCompletion *rebuild, int result)
{
  rebuild->aborted = true;
  setCompletionResult(&rebuild->completion, result);
}

/**
 * Handle an error loading a page.
 *
 * @param completion  The VDOPageCompletion
 **/
static void handlePageLoadError(VDOCompletion *completion)
{
  RebuildCompletion *rebuild = asRebuildCompletion(completion->parent);
  rebuild->outstanding--;
  abortRebuild(rebuild, completion->result);
  releaseVDOPageCompletion(completion);
  finishIfDone(rebuild);
}

/**
 * Rebuild reference counts from a block map page.
 *
 * @param rebuild     The rebuild completion
 * @param completion  The page completion holding the page
 *
 * @return VDO_SUCCESS or an error
 **/
static int rebuildReferenceCountsFromPage(RebuildCompletion *rebuild,
                                          VDOCompletion     *completion)
{
  BlockMapPage *page = dereferenceWritableVDOPage(completion);
  int result = ASSERT(page != NULL, "page available");
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (!isBlockMapPageInitialized(page)) {
    return VDO_SUCCESS;
  }

  // Remove any bogus entries which exist beyond the end of the logical space.
  if (getBlockMapPagePBN(page) == rebuild->lastSlot.pbn) {
    for (SlotNumber slot = rebuild->lastSlot.slot;
         slot < BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
      DataLocation mapping = unpackBlockMapEntry(&page->entries[slot]);
      if (isMappedLocation(&mapping)) {
        page->entries[slot] = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
        requestVDOPageWrite(completion);
      }
    }
  }

  // Inform the slab depot of all entries on this page.
  for (SlotNumber slot = 0; slot < BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
    DataLocation mapping = unpackBlockMapEntry(&page->entries[slot]);
    if (!isValidLocation(&mapping)) {
      // This entry is invalid, so remove it from the page.
      page->entries[slot] = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
      requestVDOPageWrite(completion);
      continue;
    }

    if (!isMappedLocation(&mapping)) {
      continue;
    }

    (*rebuild->logicalBlocksUsed)++;
    if (mapping.pbn == ZERO_BLOCK) {
      continue;
    }

    if (!isPhysicalDataBlock(rebuild->depot, mapping.pbn)) {
      // This is a nonsense mapping. Remove it from the map so we're at least
      // consistent and mark the page dirty.
      page->entries[slot] = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
      requestVDOPageWrite(completion);
      continue;
    }

    Slab *slab   = getSlab(rebuild->depot, mapping.pbn);
    int   result = adjustReferenceCountForRebuild(slab->referenceCounts,
                                                  mapping.pbn, DATA_INCREMENT);
    if (result != VDO_SUCCESS) {
      logErrorWithStringError(result,
                              "Could not adjust reference count for PBN"
                              " %" PRIu64 ", slot %u mapped to PBN %" PRIu64,
                              getBlockMapPagePBN(page), slot, mapping.pbn);
      page->entries[slot] = packPBN(ZERO_BLOCK, MAPPING_STATE_UNMAPPED);
      requestVDOPageWrite(completion);
    }
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
static void fetchPage(RebuildCompletion *rebuild, VDOCompletion *completion);

/**
 * Process a page which has just been loaded. This callback is registered by
 * fetchPage().
 *
 * @param completion  The VDOPageCompletion for the fetched page
 **/
static void pageLoaded(VDOCompletion *completion)
{
  RebuildCompletion *rebuild = asRebuildCompletion(completion->parent);
  rebuild->outstanding--;

  int result = rebuildReferenceCountsFromPage(rebuild, completion);
  if (result != VDO_SUCCESS) {
    abortRebuild(rebuild, result);
  }

  releaseVDOPageCompletion(completion);
  if (finishIfDone(rebuild)) {
    return;
  }

  // Advance progress to the next page, and fetch the next page we
  // haven't yet requested.
  fetchPage(rebuild, completion);
}

/**
 * Fetch a page from the block map.
 *
 * @param rebuild     the RebuildCompletion
 * @param completion  the page completion to use
 **/
static void fetchPage(RebuildCompletion *rebuild, VDOCompletion *completion)
{
  while (rebuild->pageToFetch < rebuild->leafPages) {
    PhysicalBlockNumber pbn = findBlockMapPagePBN(rebuild->blockMap,
                                                  rebuild->pageToFetch++);
    if (pbn == ZERO_BLOCK) {
      continue;
    }

    if (!isPhysicalDataBlock(rebuild->depot, pbn)) {
      abortRebuild(rebuild, VDO_BAD_MAPPING);
      if (finishIfDone(rebuild)) {
        return;
      }
      continue;
    }

    initVDOPageCompletion(((VDOPageCompletion *) completion),
                          rebuild->blockMap->zones[0].pageCache,
                          pbn, true, &rebuild->completion,
                          pageLoaded, handlePageLoadError);
    rebuild->outstanding++;
    getVDOPageAsync(completion);
    return;
  }
}

/**
 * Rebuild reference counts from the leaf block map pages now that reference
 * counts have been rebuilt from the interior tree pages (which have been
 * loaded in the process). This callback is registered in
 * rebuildReferenceCounts().
 *
 * @param completion  The sub-task completion
 **/
static void rebuildFromLeaves(VDOCompletion *completion)
{
  RebuildCompletion *rebuild = asRebuildCompletion(completion->parent);
  *rebuild->logicalBlocksUsed = 0;

  // The PBN calculation doesn't work until the tree pages have been loaded,
  // so we can't set this value at the start of rebuild.
  rebuild->lastSlot = (BlockMapSlot) {
    .slot = rebuild->blockMap->entryCount % BLOCK_MAP_ENTRIES_PER_PAGE,
    .pbn  = findBlockMapPagePBN(rebuild->blockMap, rebuild->leafPages - 1),
  };

  // Prevent any page from being processed until all pages have been launched.
  rebuild->launching = true;
  for (PageCount i = 0; i < rebuild->pageCount; i++) {
    fetchPage(rebuild, &rebuild->pageCompletions[i].completion);
  }
  rebuild->launching = false;
  finishIfDone(rebuild);
}

/**
 * Process a single entry from the block map tree.
 *
 * <p>Implements EntryCallback.
 *
 * @param pbn         A pbn which holds a block map tree page
 * @param completion  The parent completion of the traversal
 *
 * @return VDO_SUCCESS or an error
 **/
static int processEntry(PhysicalBlockNumber pbn, VDOCompletion *completion)
{
  RebuildCompletion *rebuild = asRebuildCompletion(completion->parent);
  if ((pbn == ZERO_BLOCK) || !isPhysicalDataBlock(rebuild->depot, pbn)) {
    return logErrorWithStringError(VDO_BAD_CONFIGURATION,
                                   "PBN %" PRIu64 " out of range",
                                   pbn);
  }

  Slab *slab   = getSlab(rebuild->depot, pbn);
  int   result = adjustReferenceCountForRebuild(slab->referenceCounts, pbn,
                                                BLOCK_MAP_INCREMENT);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result,
                                   "Could not adjust reference count for "
                                   "block map tree PBN %" PRIu64,
                                   pbn);
  }

  (*rebuild->blockMapDataBlocks)++;
  return VDO_SUCCESS;
}

/**********************************************************************/
void rebuildReferenceCounts(VDO           *vdo,
                            VDOCompletion *parent,
                            BlockCount    *logicalBlocksUsed,
                            BlockCount    *blockMapDataBlocks)
{
  RebuildCompletion *rebuild;
  int result = makeRebuildCompletion(vdo, logicalBlocksUsed,
                                     blockMapDataBlocks, parent, &rebuild);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  // Completion chaining from page cache hits can lead to stack overflow
  // during the rebuild, so clear out the cache before this rebuild phase.
  result = invalidateVDOPageCache(rebuild->blockMap->zones[0].pageCache);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  // First traverse the block map trees.
  *rebuild->blockMapDataBlocks = 0;
  VDOCompletion *completion = &rebuild->subTaskCompletion;
  prepareCompletion(completion, rebuildFromLeaves, finishParentCallback,
                    rebuild->logicalThreadID, rebuild);
  traverseForest(rebuild->blockMap, processEntry, completion);
}
