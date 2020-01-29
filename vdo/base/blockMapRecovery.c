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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapRecovery.c#12 $
 */

#include "blockMapRecovery.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMapInternals.h"
#include "blockMapPage.h"
#include "heap.h"
#include "numUtils.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoPageCache.h"

/**
 * A completion to manage recovering the block map from the recovery journal.
 * Note that the page completions kept in this structure are not immediately
 * freed, so the corresponding pages will be locked down in the page cache
 * until the recovery frees them.
 **/
struct block_map_recovery_completion {
  /** completion header */
  struct vdo_completion          completion;
  /** the completion for flushing the block map */
  struct vdo_completion          subTaskCompletion;
  /** the thread from which the block map may be flushed */
  ThreadID                       adminThread;
  /** the thread on which all block map operations must be done */
  ThreadID                       logicalThreadID;
  /** the block map */
  struct block_map              *blockMap;
  /** whether this recovery has been aborted */
  bool                           aborted;
  /** whether we are currently launching the initial round of requests */
  bool                           launching;

  // Fields for the journal entries.
  /** the journal entries to apply */
  struct numbered_block_mapping *journalEntries;
  /**
   * a heap wrapping journalEntries. It re-orders and sorts journal entries in
   * ascending LBN               order, then original journal order. This permits efficient
   * iteration over the journal entries in order.
   **/
  struct heap                    replayHeap;

  // Fields tracking progress through the journal entries.
  /** a pointer to the next journal entry to apply */
  struct numbered_block_mapping *currentEntry;
  /** the next entry for which the block map page has not been requested */
  struct numbered_block_mapping *currentUnfetchedEntry;

  // Fields tracking requested pages.
  /** the absolute PBN of the current page being processed */
  PhysicalBlockNumber            pbn;
  /** number of pending (non-ready) requests */
  PageCount                      outstanding;
  /** number of page completions */
  PageCount                      pageCount;
  /** array of requested, potentially ready page completions */
  struct vdo_page_completion     pageCompletions[];
};

/**
 * This is a HeapComparator function that orders NumberedBlockMappings using
 * the 'blockMapSlot' field as the primary key and the mapping 'number' field
 * as the secondary key. Using the mapping number preserves the journal order
 * of entries for the same slot, allowing us to sort by slot while still
 * ensuring we replay all entries with the same slot in the exact order as they
 * appeared in the journal.
 *
 * <p>The comparator order is reversed from the usual sense since the
 * heap structure is a max-heap, returning larger elements before
 * smaller ones, but we want to pop entries off the heap in ascending
 * LBN order.
 **/
static int compareMappings(const void *item1, const void *item2)
{
  const struct numbered_block_mapping *mapping1
    = (const struct numbered_block_mapping *) item1;
  const struct numbered_block_mapping *mapping2
    = (const struct numbered_block_mapping *) item2;

  if (mapping1->blockMapSlot.pbn != mapping2->blockMapSlot.pbn) {
    return
      ((mapping1->blockMapSlot.pbn < mapping2->blockMapSlot.pbn) ? 1 : -1);
  }

  if (mapping1->blockMapSlot.slot != mapping2->blockMapSlot.slot) {
    return
      ((mapping1->blockMapSlot.slot < mapping2->blockMapSlot.slot) ? 1 : -1);
  }

  if (mapping1->number != mapping2->number) {
    return ((mapping1->number < mapping2->number) ? 1 : -1);
  }

  return 0;
}

/**
 * Swap two numbered_block_mapping structures. Implements HeapSwapper.
 **/
static void swapMappings(void *item1, void *item2)
{
  struct numbered_block_mapping *mapping1 = item1;
  struct numbered_block_mapping *mapping2 = item2;
  struct numbered_block_mapping temp = *mapping1;
  *mapping1 = *mapping2;
  *mapping2 = temp;
}

/**
 * Convert a vdo_completion to a block_map_recovery_completion.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a block_map_recovery_completion
 **/
__attribute__((warn_unused_result))
static inline struct block_map_recovery_completion *
asBlockMapRecoveryCompletion(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct block_map_recovery_completion, completion) == 0);
  assertCompletionType(completion->type, BLOCK_MAP_RECOVERY_COMPLETION);
  return (struct block_map_recovery_completion *) completion;
}

/**
 * Free a block_map_recovery_completion and null out the reference to it.
 *
 * @param completionPtr  a pointer to the completion to free
 **/
static void freeRecoveryCompletion(struct vdo_completion **completionPtr)
{
  struct vdo_completion *completion = *completionPtr;
  if (completion == NULL) {
    return;
  }

  struct block_map_recovery_completion *recovery
    = asBlockMapRecoveryCompletion(*completionPtr);
  destroyEnqueueable(completion);
  destroyEnqueueable(&recovery->subTaskCompletion);
  FREE(recovery);
  *completionPtr = NULL;
}

/**
 * Free the block_map_recovery_completion and notify the parent that the
 * block map recovery is done. This callback is registered in
 * makeRecoveryCompletion().
 *
 * @param completion  The block_map_recovery_completion
 **/
static void finishBlockMapRecovery(struct vdo_completion *completion)
{
  int            result = completion->result;
  struct vdo_completion *parent = completion->parent;
  freeRecoveryCompletion(&completion);
  finishCompletion(parent, result);
}

/**
 * Make a new block map recovery completion.
 *
 * @param [in]  vdo             The vdo
 * @param [in]  entryCount      The number of journal entries
 * @param [in]  journalEntries  An array of journal entries to process
 * @param [in]  parent          The parent of the recovery completion
 * @param [out] recoveryPtr     The new block map recovery completion
 *
 * @return a success or error code
 **/
static int
makeRecoveryCompletion(struct vdo                            *vdo,
                       BlockCount                             entryCount,
                       struct numbered_block_mapping         *journalEntries,
                       struct vdo_completion                 *parent,
                       struct block_map_recovery_completion **recoveryPtr)
{
  struct block_map *blockMap = getBlockMap(vdo);
  PageCount pageCount
    = minPageCount(getConfiguredCacheSize(vdo) >> 1,
                   MAXIMUM_SIMULTANEOUS_BLOCK_MAP_RESTORATION_READS);

  struct block_map_recovery_completion *recovery;
  int result = ALLOCATE_EXTENDED(struct block_map_recovery_completion,
                                 pageCount, struct vdo_page_completion,
                                 __func__, &recovery);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&recovery->completion,
                                           BLOCK_MAP_RECOVERY_COMPLETION,
                                           vdo->layer);
  if (result != VDO_SUCCESS) {
    struct vdo_completion *completion = &recovery->completion;
    freeRecoveryCompletion(&completion);
    return result;
  }

  result = initializeEnqueueableCompletion(&recovery->subTaskCompletion,
                                           SUB_TASK_COMPLETION, vdo->layer);
  if (result != VDO_SUCCESS) {
    struct vdo_completion *completion = &recovery->completion;
    freeRecoveryCompletion(&completion);
    return result;
  }

  recovery->blockMap       = blockMap;
  recovery->journalEntries = journalEntries;
  recovery->pageCount      = pageCount;
  recovery->currentEntry   = &recovery->journalEntries[entryCount - 1];

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  recovery->adminThread     = getAdminThread(threadConfig);
  recovery->logicalThreadID = getLogicalZoneThread(threadConfig, 0);

  // Organize the journal entries into a binary heap so we can iterate over
  // them in sorted order incrementally, avoiding an expensive sort call.
  initializeHeap(&recovery->replayHeap, compareMappings, swapMappings,
                 journalEntries, entryCount,
                 sizeof(struct numbered_block_mapping));
  buildHeap(&recovery->replayHeap, entryCount);

  ASSERT_LOG_ONLY((getCallbackThreadID() == recovery->logicalThreadID),
                  "%s must be called on logical thread %u (not %u)", __func__,
                  recovery->logicalThreadID, getCallbackThreadID());
  prepareCompletion(&recovery->completion, finishBlockMapRecovery,
                    finishBlockMapRecovery, recovery->logicalThreadID, parent);

  // This message must be recognizable by VDOTest::RebuildBase.
  logInfo("Replaying %zu recovery entries into block map",
          recovery->replayHeap.count);

  *recoveryPtr = recovery;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void flushBlockMap(struct vdo_completion *completion)
{
  logInfo("Flushing block map changes");
  struct block_map_recovery_completion *recovery
    = asBlockMapRecoveryCompletion(completion->parent);
  ASSERT_LOG_ONLY((completion->callbackThreadID == recovery->adminThread),
                  "flushBlockMap() called on admin thread");

  prepareToFinishParent(completion, completion->parent);
  drainBlockMap(recovery->blockMap, ADMIN_STATE_RECOVERING, completion);
}

/**
 * Check whether the recovery is done. If so, finish it by either flushing the
 * block map (if the recovery was successful), or by cleaning up (if it
 * wasn't).
 *
 * @param recovery  The recovery completion
 *
 * @return <code>true</code> if the recovery or recovery is complete
 **/
static bool finishIfDone(struct block_map_recovery_completion *recovery)
{
  // Pages are still being launched or there is still work to do
  if (recovery->launching || (recovery->outstanding > 0)
      || (!recovery->aborted
          && (recovery->currentEntry >= recovery->journalEntries))) {
    return false;
  }

  if (recovery->aborted) {
    /*
     * We need to be careful here to only free completions that exist. But
     * since we know none are outstanding, we just go through the ready ones.
     */
    size_t i;
    for (i = 0; i < recovery->pageCount; i++) {
      struct vdo_page_completion *pageCompletion = &recovery->pageCompletions[i];
      if (recovery->pageCompletions[i].ready) {
        releaseVDOPageCompletion(&pageCompletion->completion);
      }
    }
    completeCompletion(&recovery->completion);
  } else {
    launchCallbackWithParent(&recovery->subTaskCompletion, flushBlockMap,
                             recovery->adminThread, &recovery->completion);
  }

  return true;
}

/**
 * Note that there has been an error during the recovery and finish it if there
 * is nothing else outstanding.
 *
 * @param recovery  The block_map_recovery_completion
 * @param result    The error result to use, if one is not already saved
 **/
static void abortRecovery(struct block_map_recovery_completion *recovery,
                          int result)
{
  recovery->aborted = true;
  setCompletionResult(&recovery->completion, result);
  finishIfDone(recovery);
}

/**
 * Find the first journal entry after a given entry which is not on the same
 * block map page.
 *
 * @param recovery      the block_map_recovery_completion
 * @param currentEntry  the entry to search from
 * @param needsSort     Whether sorting is needed to proceed
 *
 * @return Pointer to the first later journal entry on a different block map
 *         page, or a pointer to just before the journal entries if no
 *         subsequent entry is on a different block map page.
 **/
static struct numbered_block_mapping *
findEntryStartingNextPage(struct block_map_recovery_completion *recovery,
                          struct numbered_block_mapping        *currentEntry,
                          bool                                  needsSort)
{
  // If currentEntry is invalid, return immediately.
  if (currentEntry < recovery->journalEntries) {
    return currentEntry;
  }
  size_t currentPage = currentEntry->blockMapSlot.pbn;

  // Decrement currentEntry until it's out of bounds or on a different page.
  while ((currentEntry >= recovery->journalEntries)
         && (currentEntry->blockMapSlot.pbn == currentPage)) {
    if (needsSort) {
      struct numbered_block_mapping *justSortedEntry
        = sortNextHeapElement(&recovery->replayHeap);
      ASSERT_LOG_ONLY(justSortedEntry < currentEntry,
                      "heap is returning elements in an unexpected order");
    }
    currentEntry--;
  }
  return currentEntry;
}

/**
 * Apply a range of journal entries to a block map page.
 *
 * @param page           The block map page being modified
 * @param startingEntry  The first journal entry to apply
 * @param endingEntry    The entry just past the last journal entry to apply
 **/
static void
applyJournalEntriesToPage(struct block_map_page             *page,
                          struct numbered_block_mapping     *startingEntry,
                          struct numbered_block_mapping     *endingEntry)
{
  struct numbered_block_mapping *currentEntry  = startingEntry;
  while (currentEntry != endingEntry) {
    page->entries[currentEntry->blockMapSlot.slot]
      = currentEntry->blockMapEntry;
    currentEntry--;
  }
}

/**********************************************************************/
static void recoverReadyPages(struct block_map_recovery_completion *recovery,
                              struct vdo_completion                *completion);

/**
 * Note that a page is now ready and attempt to process pages. This callback is
 * registered in fetchPage().
 *
 * @param completion  The vdo_page_completion for the fetched page
 **/
static void pageLoaded(struct vdo_completion *completion)
{
  struct block_map_recovery_completion *recovery
    = asBlockMapRecoveryCompletion(completion->parent);
  recovery->outstanding--;
  if (!recovery->launching) {
    recoverReadyPages(recovery, completion);
  }
}

/**
 * Handle an error loading a page.
 *
 * @param completion  The vdo_page_completion
 **/
static void handlePageLoadError(struct vdo_completion *completion)
{
  struct block_map_recovery_completion *recovery
    = asBlockMapRecoveryCompletion(completion->parent);
  recovery->outstanding--;
  abortRecovery(recovery, completion->result);
}

/**
 * Fetch a page from the block map.
 *
 * @param recovery    the block_map_recovery_completion
 * @param completion  the page completion to use
 **/
static void fetchPage(struct block_map_recovery_completion *recovery,
                      struct vdo_completion                *completion)
{
  if (recovery->currentUnfetchedEntry < recovery->journalEntries) {
    // Nothing left to fetch.
    return;
  }

  // Fetch the next page we haven't yet requested.
  PhysicalBlockNumber newPBN
    = recovery->currentUnfetchedEntry->blockMapSlot.pbn;
  recovery->currentUnfetchedEntry
    = findEntryStartingNextPage(recovery, recovery->currentUnfetchedEntry,
                                true);
  initVDOPageCompletion(((struct vdo_page_completion *) completion),
                        recovery->blockMap->zones[0].pageCache,
                        newPBN, true, &recovery->completion,
                        pageLoaded, handlePageLoadError);
  recovery->outstanding++;
  getVDOPageAsync(completion);
}

/**
 * Get the next page completion to process. If it isn't ready, we'll try again
 * when it is.
 *
 * @param recovery    The recovery completion
 * @param completion  The current page completion
 *
 * @return The next page completion to process
 **/
static struct vdo_page_completion *
getNextPageCompletion(struct block_map_recovery_completion *recovery,
                      struct vdo_page_completion           *completion)
{
  completion++;
  if (completion == (&recovery->pageCompletions[recovery->pageCount])) {
    completion = &recovery->pageCompletions[0];
  }
  return completion;
}

/**
 * Recover from as many pages as possible.
 *
 * @param recovery    The recovery completion
 * @param completion  The first page completion to process
 **/
static void recoverReadyPages(struct block_map_recovery_completion *recovery,
                              struct vdo_completion                *completion)
{
  if (finishIfDone(recovery)) {
    return;
  }

  struct vdo_page_completion *pageCompletion
    = (struct vdo_page_completion *) completion;
  if (recovery->pbn != pageCompletion->pbn) {
    return;
  }

  while (pageCompletion->ready) {
    struct block_map_page *page   = dereferenceWritableVDOPage(completion);
    int           result = ASSERT(page != NULL, "page available");
    if (result != VDO_SUCCESS) {
      abortRecovery(recovery, result);
      return;
    }

    struct numbered_block_mapping *startOfNextPage
      = findEntryStartingNextPage(recovery, recovery->currentEntry, false);
    applyJournalEntriesToPage(page, recovery->currentEntry, startOfNextPage);
    recovery->currentEntry = startOfNextPage;
    requestVDOPageWrite(completion);
    releaseVDOPageCompletion(completion);

    if (finishIfDone(recovery)) {
      return;
    }

    recovery->pbn = recovery->currentEntry->blockMapSlot.pbn;
    fetchPage(recovery, completion);
    pageCompletion = getNextPageCompletion(recovery, pageCompletion);
    completion     = &pageCompletion->completion;
  }
}

/**********************************************************************/
void recoverBlockMap(struct vdo                    *vdo,
                     BlockCount                     entryCount,
                     struct numbered_block_mapping *journalEntries,
                     struct vdo_completion         *parent)
{
  struct block_map_recovery_completion *recovery;
  int result = makeRecoveryCompletion(vdo, entryCount, journalEntries, parent,
                                      &recovery);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  if (isHeapEmpty(&recovery->replayHeap)) {
    finishCompletion(&recovery->completion, VDO_SUCCESS);
    return;
  }

  struct numbered_block_mapping *firstSortedEntry
    = sortNextHeapElement(&recovery->replayHeap);
  ASSERT_LOG_ONLY(firstSortedEntry == recovery->currentEntry,
                  "heap is returning elements in an unexpected order");

  // Prevent any page from being processed until all pages have been launched.
  recovery->launching = true;
  recovery->pbn       = recovery->currentEntry->blockMapSlot.pbn;
  recovery->currentUnfetchedEntry = recovery->currentEntry;
  PageCount i;
  for (i = 0; i < recovery->pageCount; i++) {
    if (recovery->currentUnfetchedEntry < recovery->journalEntries) {
      break;
    }

    fetchPage(recovery, &recovery->pageCompletions[i].completion);
  }
  recovery->launching = false;

  // Process any ready pages.
  recoverReadyPages(recovery, &recovery->pageCompletions[0].completion);
}
