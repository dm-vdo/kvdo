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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoPageCache.c#16 $
 */

#include "vdoPageCacheInternals.h"

#include <linux/ratelimit.h>

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "constants.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "statusCodes.h"
#include "types.h"
#include "vio.h"

enum {
  LOG_INTERVAL                = 4000,
  DISPLAY_INTERVAL            = 100000,
};

/**********************************************************************/
static char *getPageBuffer(struct page_info *info)
{
  struct vdo_page_cache *cache = info->cache;
  return &cache->pages[(info - cache->infos) * VDO_BLOCK_SIZE];
}

/**
 * Allocate components of the cache which require their own allocation. The
 * caller is responsible for all clean up on errors.
 *
 * @param cache     The cache being constructed
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int allocateCacheComponents(struct vdo_page_cache *cache)
{
  int result = ALLOCATE(cache->pageCount, struct page_info, "page infos",
                        &cache->infos);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint64_t size = cache->pageCount * (uint64_t) VDO_BLOCK_SIZE;
  result = allocateMemory(size, VDO_BLOCK_SIZE, "cache pages", &cache->pages);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return makeIntMap(cache->pageCount, 0, &cache->pageMap);
}

/**
 * Initialize all page info structures and put them on the free list.
 *
 * @param cache  The cache to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
static int initializeInfo(struct vdo_page_cache *cache)
{
  initializeRing(&cache->freeList);
  struct page_info *info;
  for (info = cache->infos; info < cache->infos + cache->pageCount; ++info) {
    info->cache = cache;
    info->state = PS_FREE;
    info->pbn   = NO_PAGE;

    if (cache->layer->createMetadataVIO != NULL) {
      int result = createVIO(cache->layer, VIO_TYPE_BLOCK_MAP,
                             VIO_PRIORITY_METADATA, info, getPageBuffer(info),
                             &info->vio);
      if (result != VDO_SUCCESS) {
        return result;
      }

      // The thread ID should never change.
      info->vio->completion.callbackThreadID = cache->zone->threadID;
    }

    initializeRing(&info->listNode);
    pushRingNode(&cache->freeList, &info->listNode);
    initializeRing(&info->lruNode);
  }

  relaxedStore64(&cache->stats.counts.freePages, cache->pageCount);
  return VDO_SUCCESS;
}

/**********************************************************************/
static void writeDirtyPagesCallback(RingNode *node, void *context);

/**********************************************************************/
int makeVDOPageCache(PhysicalLayer          *layer,
                     PageCount               pageCount,
                     VDOPageReadFunction    *readHook,
                     VDOPageWriteFunction   *writeHook,
                     size_t                  pageContextSize,
                     BlockCount              maximumAge,
                     struct block_map_zone  *zone,
                     struct vdo_page_cache **cachePtr)
{
  int result = ASSERT(pageContextSize <= MAX_PAGE_CONTEXT_SIZE,
                      "page context size %zu cannot exceed %u bytes",
                      pageContextSize, MAX_PAGE_CONTEXT_SIZE);
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct vdo_page_cache *cache;
  result = ALLOCATE(1, struct vdo_page_cache, "page cache", &cache);
  if (result != UDS_SUCCESS) {
    return result;
  }

  cache->layer            = layer;
  cache->pageCount        = pageCount;
  cache->readHook         = readHook;
  cache->writeHook        = writeHook;
  cache->zone             = zone;

  result = allocateCacheComponents(cache);
  if (result != VDO_SUCCESS) {
    freeVDOPageCache(&cache);
    return result;
  }

  result = initializeInfo(cache);
  if (result != VDO_SUCCESS) {
    freeVDOPageCache(&cache);
    return result;
  }

  result = makeDirtyLists(maximumAge, writeDirtyPagesCallback, cache,
                          &cache->dirtyLists);
  if (result != VDO_SUCCESS) {
    freeVDOPageCache(&cache);
    return result;
  }

  // initialize empty circular queues
  initializeRing(&cache->lruList);
  initializeRing(&cache->outgoingList);

  *cachePtr = cache;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeVDOPageCache(struct vdo_page_cache **cachePtr)
{
  struct vdo_page_cache *cache = *cachePtr;
  if (cache == NULL) {
    return;
  }

  if (cache->infos != NULL) {
    struct page_info *info;
    for (info = cache->infos; info < cache->infos + cache->pageCount; ++info) {
      freeVIO(&info->vio);
    }
  }

  freeDirtyLists(&cache->dirtyLists);
  freeIntMap(&cache->pageMap);
  FREE(cache->infos);
  FREE(cache->pages);
  FREE(cache);
  *cachePtr = NULL;
}

/**********************************************************************/
void setVDOPageCacheInitialPeriod(struct vdo_page_cache *cache,
                                  SequenceNumber         period)
{
  setCurrentPeriod(cache->dirtyLists, period);
}

/**********************************************************************/
void setVDOPageCacheRebuildMode(struct vdo_page_cache *cache, bool rebuilding)
{
  cache->rebuilding = rebuilding;
}

/**
 * Assert that a function has been called on the VDO page cache's thread.
 *
 * @param cache         the page cache
 * @param functionName  the name of the function
 **/
static inline void assertOnCacheThread(struct vdo_page_cache *cache,
                                       const char            *functionName)
{
  ThreadID threadID = getCallbackThreadID();
  ASSERT_LOG_ONLY((threadID == cache->zone->threadID),
                  "%s() must only be called on cache thread %d, not thread %d",
                  functionName, cache->zone->threadID, threadID);
}

/**
 * Assert that a page cache may issue I/O.
 *
 * @param cache  the page cache
 **/
static inline void assertIOAllowed(struct vdo_page_cache *cache)
{
  ASSERT_LOG_ONLY(!is_quiescent(&cache->zone->state),
                  "VDO page cache may issue I/O");
}

/**
 * Log and, if enabled, report cache pressure.
 *
 * @param cache         the page cache
 **/
static void reportCachePressure(struct vdo_page_cache *cache)
{
  relaxedAdd64(&cache->stats.cachePressure, 1);
  if (cache->waiterCount > cache->pageCount) {
    if ((cache->pressureReport % LOG_INTERVAL) == 0) {
      logInfo("page cache pressure %llu",
              relaxedLoad64(&cache->stats.cachePressure));
    }

    if (++cache->pressureReport >= DISPLAY_INTERVAL) {
      cache->pressureReport = 0;
    }
  }
}

/**********************************************************************/
const char *vpcPageStateName(PageState state)
{
  static const char *stateNames[] = {
    "FREE",
    "INCOMING",
    "FAILED",
    "RESIDENT",
    "DIRTY",
    "OUTGOING"
  };
  STATIC_ASSERT(COUNT_OF(stateNames) == PAGE_STATE_COUNT);

  int result = ASSERT(state < COUNT_OF(stateNames),
                      "Unknown PageState value %d", state);
  if (result != UDS_SUCCESS) {
    return "[UNKNOWN PAGE STATE]";
  }

  return stateNames[state];
}

/**
 * Update the counter associated with a given state.
 *
 * @param info   the page info to count
 * @param delta  the delta to apply to the counter
 **/
static void updateCounter(struct page_info *info, int32_t delta)
{
  struct vdo_page_cache *cache = info->cache;
  switch (info->state) {
    case PS_FREE:
      relaxedAdd64(&cache->stats.counts.freePages, delta);
      return;

    case PS_INCOMING:
      relaxedAdd64(&cache->stats.counts.incomingPages, delta);
      return;

    case PS_OUTGOING:
      relaxedAdd64(&cache->stats.counts.outgoingPages, delta);
      return;

    case PS_FAILED:
      relaxedAdd64(&cache->stats.counts.failedPages, delta);
      return;

    case PS_RESIDENT:
      relaxedAdd64(&cache->stats.counts.cleanPages, delta);
      return;

    case PS_DIRTY:
      relaxedAdd64(&cache->stats.counts.dirtyPages, delta);
      return;

    default:
      return;
  }
}

/**
 * Update the lru information for an active page.
 **/
static void updateLru(struct page_info *info)
{
  struct vdo_page_cache *cache = info->cache;

  if (cache->lruList.prev != &info->lruNode) {
    pushRingNode(&cache->lruList, &info->lruNode);
  }
}

/**
 * Set the state of a page_info and put it on the right list, adjusting
 * counters.
 *
 * @param info      the page_info to modify
 * @param newState  the new state for the page_info
 **/
static void setInfoState(struct page_info *info, PageState newState)
{
  if (newState == info->state) {
    return;
  }

  updateCounter(info, -1);
  info->state = newState;
  updateCounter(info, 1);

  switch (info->state) {
  case PS_FREE:
  case PS_FAILED:
    pushRingNode(&info->cache->freeList, &info->listNode);
    return;

  case PS_OUTGOING:
    pushRingNode(&info->cache->outgoingList, &info->listNode);
    return;

  case PS_DIRTY:
    return;

  default:
    unspliceRingNode(&info->listNode);
  }
}

/**
 * Set the pbn for an info, updating the map as needed.
 *
 * @param info  The page info
 * @param pbn   The physical block number to set
 **/
__attribute__((warn_unused_result))
static int setInfoPBN(struct page_info *info, PhysicalBlockNumber pbn)
{
  struct vdo_page_cache *cache = info->cache;

  // Either the new or the old page number must be NO_PAGE.
  int result = ASSERT((pbn == NO_PAGE) || (info->pbn == NO_PAGE),
                      "Must free a page before reusing it.");
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (info->pbn != NO_PAGE) {
    intMapRemove(cache->pageMap, info->pbn);
  }

  info->pbn = pbn;

  if (pbn != NO_PAGE) {
    result = intMapPut(cache->pageMap, pbn, info, true, NULL);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return VDO_SUCCESS;
}

/**
 * Reset page info to represent an unallocated page.
 **/
static int resetPageInfo(struct page_info *info)
{
  int result = ASSERT(info->busy == 0, "VDO Page must not be busy");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(!hasWaiters(&info->waiting),
                  "VDO Page must not have waiters");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = setInfoPBN(info, NO_PAGE);
  setInfoState(info, PS_FREE);
  unspliceRingNode(&info->lruNode);
  return result;
}

/**
 * Find a free page.
 *
 * @param cache         the page cache
 *
 * @return a pointer to the page info structure (if found), NULL otherwise
 **/
__attribute__((warn_unused_result))
static struct page_info *findFreePage(struct vdo_page_cache *cache)
{
  if (cache->freeList.next == &cache->freeList) {
    return NULL;
  }
  struct page_info *info = pageInfoFromListNode(cache->freeList.next);
  unspliceRingNode(&info->listNode);
  return info;
}

/**********************************************************************/
struct page_info *vpcFindPage(struct vdo_page_cache *cache,
                              PhysicalBlockNumber pbn)
{
  if ((cache->lastFound != NULL)
      && (cache->lastFound->pbn == pbn)) {
    return cache->lastFound;
  }
  cache->lastFound = intMapGet(cache->pageMap, pbn);
  return cache->lastFound;
}

/**
 * Determine which page is least recently used.
 *
 * @param cache         the page cache structure
 *
 * @return a pointer to the info structure for a relevant page,
 *         or NULL if no such page can be found. The page can be
 *         dirty or resident.
 *
 * @note Picks the least recently used from among the non-busy entries
 *       at the front of each of the lru ring.
 *       Since whenever we mark a page busy we also put it to the end
 *       of the ring it is unlikely that the entries at the front
 *       are busy unless the queue is very short, but not impossible.
 **/
__attribute__((warn_unused_result))
static struct page_info *selectLRUPage(struct vdo_page_cache *cache)
{
  PageInfoNode *lru;
  for (lru = cache->lruList.next;
       lru != &cache->lruList;
       lru = lru->next) {
    struct page_info *info = pageInfoFromLRUNode(lru);
    if ((info->busy == 0) && !isInFlight(info)) {
      return info;
    }
  }

  return NULL;
}

/**********************************************************************/
struct atomic_page_cache_statistics *
getVDOPageCacheStatistics(struct vdo_page_cache *cache)
{
  return &cache->stats;
}

// ASYNCHRONOUS INTERFACE BEYOND THIS POINT

/**
 * Helper to complete the VDO Page Completion request successfully.
 *
 * @param info          the page info representing the result page
 * @param vdoPageComp   the VDO page completion to complete
 **/
static void completeWithPage(struct page_info           *info,
                             struct vdo_page_completion *vdoPageComp)
{
  bool available = vdoPageComp->writable ? isPresent(info) : isValid(info);
  if (!available) {
    logErrorWithStringError(VDO_BAD_PAGE,
                            "Requested cache page %llu in state %s is"
                            " not %s",
                            info->pbn, vpcPageStateName(info->state),
                            vdoPageComp->writable ? "present" : "valid");
    finishCompletion(&vdoPageComp->completion, VDO_BAD_PAGE);
    return;
  }

  vdoPageComp->info = info;
  vdoPageComp->ready = true;
  finishCompletion(&vdoPageComp->completion, VDO_SUCCESS);
}

/**
 * Complete a page completion with an error code. Implements WaiterCallback.
 *
 * @param waiter        The page completion, as a waiter
 * @param resultPtr     A pointer to the error code.
 **/
static void completeWaiterWithError(struct waiter *waiter, void *resultPtr)
{
  int                        *result     = resultPtr;
  struct vdo_page_completion *completion = pageCompletionFromWaiter(waiter);
  finishCompletion(&completion->completion, *result);
}

/**
 * Complete a queue of VDOPageCompletions with an error code.
 *
 * @param [in]      result      the error result
 * @param [in, out] queue       a pointer to the queue
 *
 * @note upon completion the queue will be empty
 **/
static void distributeErrorOverQueue(int result, struct wait_queue *queue)
{
  notifyAllWaiters(queue, completeWaiterWithError, &result);
}

/**
 * Complete a page completion with a page. Implements WaiterCallback.
 *
 * @param waiter        The page completion, as a waiter
 * @param pageInfo      The page info to complete with
 **/
static void completeWaiterWithPage(struct waiter *waiter, void *pageInfo)
{
  struct page_info           *info       = pageInfo;
  struct vdo_page_completion *completion = pageCompletionFromWaiter(waiter);
  completeWithPage(info, completion);
}

/**
 * Complete a queue of VDOPageCompletions with a page result.
 *
 * @param [in]      info        the page info describing the page
 * @param [in, out] queue       a pointer to a queue of waiters
 *
 * @return the number of pages distributed
 *
 * @note upon completion the queue will be empty
 *
 **/
static unsigned int distributePageOverQueue(struct page_info  *info,
                                            struct wait_queue *queue)
{
  updateLru(info);

  size_t pages = countWaiters(queue);

  /*
   * Increment the busy count once for each pending completion so that
   * this page does not stop being busy until all completions have
   * been processed (VDO-83).
   */
  info->busy += pages;

  notifyAllWaiters(queue, completeWaiterWithPage, info);
  return pages;
}

/**
 * Set a persistent error which all requests will receive in the future.
 *
 * @param cache         the page cache
 * @param context       a string describing what triggered the error
 * @param result        the error result
 *
 * Once triggered, all enqueued completions will get this error.
 * Any future requests will result in this error as well.
 **/
static void setPersistentError(struct vdo_page_cache *cache,
                               const char            *context,
                               int                    result)
{
  // If we're already read-only, there's no need to log.
  struct read_only_notifier *notifier = cache->zone->readOnlyNotifier;
  if ((result != VDO_READ_ONLY) && !isReadOnly(notifier)) {
    logErrorWithStringError(result, "VDO Page Cache persistent error: %s",
                            context);
    enterReadOnlyMode(notifier, result);
  }

  assertOnCacheThread(cache, __func__);

  distributeErrorOverQueue(result, &cache->freeWaiters);
  cache->waiterCount = 0;

  struct page_info *info;
  for (info = cache->infos; info < cache->infos + cache->pageCount; ++info) {
    distributeErrorOverQueue(result, &info->waiting);
  }
}

/**********************************************************************/
void initVDOPageCompletion(struct vdo_page_completion *pageCompletion,
                           struct vdo_page_cache      *cache,
                           PhysicalBlockNumber         pbn,
                           bool                        writable,
                           void                       *parent,
                           VDOAction                  *callback,
                           VDOAction                  *errorHandler)
{
  ASSERT_LOG_ONLY((pageCompletion->waiter.nextWaiter == NULL),
                  "New page completion was not already on a wait queue");

  *pageCompletion = (struct vdo_page_completion) {
    .pbn      = pbn,
    .writable = writable,
    .cache    = cache,
  };

  struct vdo_completion *completion = &pageCompletion->completion;
  initializeCompletion(completion, VDO_PAGE_COMPLETION, cache->layer);
  prepareCompletion(completion, callback, errorHandler, cache->zone->threadID,
                    parent);
}

/**
 * Helper function to check that a completion represents a successfully
 * completed VDO Page Completion referring to a valid page.
 *
 * @param completion    a VDO completion
 * @param writable      whether a writable page is required
 *
 * @return the embedding completion if valid, NULL if not
 **/
__attribute__((warn_unused_result))
static struct vdo_page_completion *
validateCompletedPage(struct vdo_completion *completion,
                      bool                   writable)
{
  struct vdo_page_completion *vpc = asVDOPageCompletion(completion);

  int result = ASSERT(vpc->ready, "VDO Page completion not ready");
  if (result != UDS_SUCCESS) {
    return NULL;
  }

  result = ASSERT(vpc->info != NULL, "VDO Page Completion must be complete");
  if (result != UDS_SUCCESS) {
    return NULL;
  }

  result = ASSERT(vpc->info->pbn == vpc->pbn,
                  "VDO Page Completion pbn must be consistent");
  if (result != UDS_SUCCESS) {
    return NULL;
  }

  result = ASSERT(isValid(vpc->info),
                  "VDO Page Completion page must be valid");
  if (result != UDS_SUCCESS) {
    return NULL;
  }

  if (writable) {
    result = ASSERT(vpc->writable, "VDO Page Completion is writable");
    if (result != UDS_SUCCESS) {
      return NULL;
    }
  }

  return vpc;
}

/**
 * Check if there are no outstanding I/O operations, and if so complete
 * any cache operation which is pending.
 *
 * @param cache   the VDO page cache
 **/
static void checkForIOComplete(struct vdo_page_cache *cache)
{
  if ((cache->outstandingReads + cache->outstandingWrites) == 0) {
    finish_draining_with_result(&cache->zone->state,
                                (isReadOnly(cache->zone->readOnlyNotifier)
                                 ? VDO_READ_ONLY : VDO_SUCCESS));
  }
}

/**
 * vio callback used when a page has been loaded.
 *
 * @param completion  A completion for the vio, the parent of which is a
 *                    page_info.
 **/
static void pageIsLoaded(struct vdo_completion *completion)
{
  struct page_info      *info   = completion->parent;
  struct vdo_page_cache *cache  = info->cache;
  assertOnCacheThread(cache, __func__);

  setInfoState(info, PS_RESIDENT);
  distributePageOverQueue(info, &info->waiting);

  /*
   * Don't decrement until right before calling checkForIOComplete() to ensure
   * that the above work can't cause the page cache to be freed out from under
   * us.
   */
  cache->outstandingReads--;
  checkForIOComplete(cache);
}

/**
 * Handle page load errors.
 *
 * @param completion  The page read vio
 **/
static void handleLoadError(struct vdo_completion *completion)
{
  int                    result = completion->result;
  struct page_info      *info   = completion->parent;
  struct vdo_page_cache *cache  = info->cache;
  assertOnCacheThread(cache, __func__);

  enterReadOnlyMode(cache->zone->readOnlyNotifier, result);
  relaxedAdd64(&cache->stats.failedReads, 1);
  setInfoState(info, PS_FAILED);
  distributeErrorOverQueue(result, &info->waiting);
  resetPageInfo(info);

  /*
   * Don't decrement until right before calling checkForIOComplete() to ensure
   * that the above work can't cause the page cache to be freed out from under
   * us.
   */
  cache->outstandingReads--;
  checkForIOComplete(cache);
}

/**
 * Run the read hook after a page is loaded. This callback is registered in
 * launchPageLoad() when there is a read hook.
 *
 * @param completion  The page load completion
 **/
static void runReadHook(struct vdo_completion *completion)
{
  struct page_info *info = completion->parent;
  completion->callback = pageIsLoaded;
  resetCompletion(completion);
  int result = info->cache->readHook(getPageBuffer(info), info->pbn,
                                     info->cache->zone, info->context);
  continueCompletion(completion, result);
}

/**
 * Handle a read error during a read-only rebuild.
 *
 * @param completion  The page load completion
 **/
static void handleRebuildReadError(struct vdo_completion *completion)
{
  struct page_info      *info   = completion->parent;
  struct vdo_page_cache *cache  = info->cache;
  assertOnCacheThread(cache, __func__);

  // We are doing a read-only rebuild, so treat this as a successful read
  // of an uninitialized page.
  relaxedAdd64(&cache->stats.failedReads, 1);
  memset(getPageBuffer(info), 0, VDO_BLOCK_SIZE);
  resetCompletion(completion);
  if (cache->readHook != NULL) {
    runReadHook(completion);
  } else {
    pageIsLoaded(completion);
  }
}

/**
 * Begin the process of loading a page.
 *
 * @param info  the page info representing where to load the page
 * @param pbn   the absolute pbn of the desired page
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int launchPageLoad(struct page_info *info, PhysicalBlockNumber pbn)
{
  struct vdo_page_cache *cache = info->cache;
  assertIOAllowed(cache);

  int result = setInfoPBN(info, pbn);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ASSERT((info->busy == 0), "Page is not busy before loading.");
  if (result != VDO_SUCCESS) {
    return result;
  }

  setInfoState(info, PS_INCOMING);
  cache->outstandingReads++;
  relaxedAdd64(&cache->stats.pagesLoaded, 1);
  launchReadMetadataVIO(info->vio, pbn,
                        (cache->readHook != NULL) ? runReadHook : pageIsLoaded,
                        (cache->rebuilding
                         ? handleRebuildReadError : handleLoadError));
  return VDO_SUCCESS;
}

/**********************************************************************/
static void writePages(struct vdo_completion *completion);

/**
 * Handle errors flushing the layer.
 *
 * @param completion  The flush vio
 **/
static void handleFlushError(struct vdo_completion *completion)
{
  struct vdo_page_cache *cache
    = ((struct page_info *) completion->parent)->cache;
  setPersistentError(cache, "flush failed", completion->result);
  writePages(completion);
}

/**
 * Attempt to save the outgoing pages by first flushing the layer.
 *
 * @param cache  The cache
 **/
static void savePages(struct vdo_page_cache *cache)
{
  if ((cache->pagesInFlush > 0) || (cache->pagesToFlush == 0)) {
    return;
  }

  assertIOAllowed(cache);

  struct page_info *info = pageInfoFromListNode(cache->outgoingList.next);
  cache->pagesInFlush = cache->pagesToFlush;
  cache->pagesToFlush = 0;
  relaxedAdd64(&cache->stats.flushCount, 1);

  struct vio    *vio   = info->vio;
  PhysicalLayer *layer = vio->completion.layer;
  if (layer->isFlushRequired(layer)) {
    launchFlush(vio, writePages, handleFlushError);
    return;
  }

  writePages(&vio->completion);
}

/**
 * Add a page to the outgoing list of pages waiting to be saved. Once in the
 * list, a page may not be used until it has been written out.
 *
 * @param info  The page to save
 **/
static void schedulePageSave(struct page_info *info)
{
  if (info->busy > 0) {
    info->writeStatus = WRITE_STATUS_DEFERRED;
    return;
  }

  info->cache->pagesToFlush++;
  info->cache->outstandingWrites++;
  setInfoState(info, PS_OUTGOING);
}

/**********************************************************************/
static void writeDirtyPagesCallback(RingNode *expired, void *context)
{
  while (!isRingEmpty(expired)) {
    schedulePageSave(pageInfoFromListNode(chopRingNode(expired)));
  }

  savePages((struct vdo_page_cache *) context);
}

/**
 * Add a page to outgoing pages waiting to be saved, and then start saving
 * pages if another save is not in progress.
 *
 * @param info  The page to save
 **/
static void launchPageSave(struct page_info *info)
{
  schedulePageSave(info);
  savePages(info->cache);
}

/**
 * Determine whether a given vdo_page_completion (as a waiter) is requesting a
 * given page number. Implements WaiterMatch.
 *
 * @param waiter        The page completion in question
 * @param context       A pointer to the pbn of the desired page
 *
 * @return true if the page completion is for the desired page number
 **/
static bool completionNeedsPage(struct waiter *waiter, void *context)
{
  PhysicalBlockNumber *pbn = context;
  return (pageCompletionFromWaiter(waiter)->pbn == *pbn);
}

/**
 * Allocate a free page to the first completion in the waiting queue,
 * and any other completions that match it in page number.
 **/
static void allocateFreePage(struct page_info *info)
{
  struct vdo_page_cache *cache = info->cache;
  assertOnCacheThread(cache, __func__);

  if (!hasWaiters(&cache->freeWaiters)) {
    if (relaxedLoad64(&cache->stats.cachePressure) > 0) {
      logInfo("page cache pressure relieved");
      relaxedStore64(&cache->stats.cachePressure, 0);
    }
    return;
  }

  int result = resetPageInfo(info);
  if (result != VDO_SUCCESS) {
    setPersistentError(cache, "cannot reset page info", result);
    return;
  }

  struct waiter *oldestWaiter = getFirstWaiter(&cache->freeWaiters);
  PhysicalBlockNumber pbn = pageCompletionFromWaiter(oldestWaiter)->pbn;

  // Remove all entries which match the page number in question
  // and push them onto the page info's wait queue.
  dequeueMatchingWaiters(&cache->freeWaiters, completionNeedsPage,
                         &pbn, &info->waiting);
  cache->waiterCount -= countWaiters(&info->waiting);

  result = launchPageLoad(info, pbn);
  if (result != VDO_SUCCESS) {
    distributeErrorOverQueue(result, &info->waiting);
  }
}

/**
 * Begin the process of discarding a page.
 *
 * @param cache         the page cache
 *
 * @note If no page is discardable, increments a count of deferred frees so
 *       that the next release of a page which is no longer busy will kick
 *       off another discard cycle. This is an indication that the cache is
 *       not big enough.
 *
 * @note If the selected page is not dirty, immediately allocates the page
 *       to the oldest completion waiting for a free page.
 **/
static void discardAPage(struct vdo_page_cache *cache)
{
  struct page_info *info = selectLRUPage(cache);
  if (info == NULL) {
    reportCachePressure(cache);
    return;
  }

  if (!isDirty(info)) {
    allocateFreePage(info);
    return;
  }

  ASSERT_LOG_ONLY(!isInFlight(info),
                  "page selected for discard is not in flight");

  ++cache->discardCount;
  info->writeStatus = WRITE_STATUS_DISCARD;
  launchPageSave(info);
}

/**
 * Helper used to trigger a discard so that the completion can get a different
 * page.
 *
 * @param vdoPageComp   the VDO Page completion
 **/
static void discardPageForCompletion(struct vdo_page_completion *vdoPageComp)
{
  struct vdo_page_cache *cache = vdoPageComp->cache;

  ++cache->waiterCount;

  int result = enqueueWaiter(&cache->freeWaiters, &vdoPageComp->waiter);
  if (result != VDO_SUCCESS) {
    setPersistentError(cache, "cannot enqueue waiter", result);
  }

  discardAPage(cache);
}

/**
 * Helper used to trigger a discard if the cache needs another free page.
 *
 * @param cache         the page cache
 **/
static void discardPageIfNeeded(struct vdo_page_cache *cache)
{
  if (cache->waiterCount > cache->discardCount) {
    discardAPage(cache);
  }
}

/**********************************************************************/
void advanceVDOPageCachePeriod(struct vdo_page_cache *cache,
                               SequenceNumber         period)
{
  assertOnCacheThread(cache, __func__);
  advancePeriod(cache->dirtyLists, period);
}

/**
 * Inform the cache that a write has finished (possibly with an error).
 *
 * @param info  The info structure for the page whose write just completed
 *
 * @return <code>true</code> if the page write was a discard
 **/
static bool writeHasFinished(struct page_info *info)
{
  assertOnCacheThread(info->cache, __func__);
  info->cache->outstandingWrites--;

  bool wasDiscard = (info->writeStatus == WRITE_STATUS_DISCARD);
  info->writeStatus = WRITE_STATUS_NORMAL;
  return wasDiscard;
}

/**
 * Handler for page write errors.
 *
 * @param completion  The page write vio
 **/
static void handlePageWriteError(struct vdo_completion *completion)
{
  int                    result = completion->result;
  struct page_info      *info   = completion->parent;
  struct vdo_page_cache *cache  = info->cache;

  // If we're already read-only, write failures are to be expected.
  if (result != VDO_READ_ONLY) {
    static DEFINE_RATELIMIT_STATE(errorLimiter, DEFAULT_RATELIMIT_INTERVAL,
                                  DEFAULT_RATELIMIT_BURST);

    if (__ratelimit(&errorLimiter)) {
      logError("failed to write block map page %llu", info->pbn);
    }
  }

  setInfoState(info, PS_DIRTY);
  relaxedAdd64(&cache->stats.failedWrites, 1);
  setPersistentError(cache, "cannot write page", result);

  if (!writeHasFinished(info)) {
    discardPageIfNeeded(cache);
  }

  checkForIOComplete(cache);
}

/**
 * vio callback used when a page has been written out.
 *
 * @param completion    A completion for the vio, the parent of which
 *                      is embedded in page_info.
 **/
static void pageIsWrittenOut(struct vdo_completion *completion)
{
  struct page_info      *info  = completion->parent;
  struct vdo_page_cache *cache = info->cache;

  if (cache->writeHook != NULL) {
    bool rewrite = cache->writeHook(getPageBuffer(info), cache->zone,
                                    info->context);
    if (rewrite) {
      launchWriteMetadataVIOWithFlush(info->vio, info->pbn, pageIsWrittenOut,
                                      handlePageWriteError, true, false);
      return;
    }
  }

  bool wasDiscard = writeHasFinished(info);
  bool reclaimed  = (!wasDiscard || (info->busy > 0)
                     || hasWaiters(&info->waiting));

  setInfoState(info, PS_RESIDENT);

  uint32_t reclamations = distributePageOverQueue(info, &info->waiting);
  relaxedAdd64(&cache->stats.reclaimed, reclamations);

  if (wasDiscard) {
    cache->discardCount--;
  }

  if (reclaimed) {
    discardPageIfNeeded(cache);
  } else {
    allocateFreePage(info);
  }

  checkForIOComplete(cache);
}

/**
 * Write the batch of pages which were covered by the layer flush which just
 * completed. This callback is registered in savePages().
 *
 * @param flushCompletion  The flush vio
 **/
static void writePages(struct vdo_completion *flushCompletion)
{
  struct vdo_page_cache *cache
    = ((struct page_info *) flushCompletion->parent)->cache;

  /*
   * We need to cache these two values on the stack since in the error case
   * below, it is possible for the last page info to cause the page cache to
   * get freed. Hence once we launch the last page, it may be unsafe to
   * dereference the cache [VDO-4724].
   */
  bool      hasUnflushedPages = (cache->pagesToFlush > 0);
  PageCount pagesInFlush      = cache->pagesInFlush;
  cache->pagesInFlush         = 0;
  while (pagesInFlush-- > 0) {
    struct page_info *info
      = pageInfoFromListNode(chopRingNode(&cache->outgoingList));
    if (isReadOnly(info->cache->zone->readOnlyNotifier)) {
      struct vdo_completion *completion = &info->vio->completion;
      resetCompletion(completion);
      completion->callback     = pageIsWrittenOut;
      completion->errorHandler = handlePageWriteError;
      finishCompletion(completion, VDO_READ_ONLY);
      continue;
    }
    relaxedAdd64(&info->cache->stats.pagesSaved, 1);
    launchWriteMetadataVIO(info->vio, info->pbn, pageIsWrittenOut,
                           handlePageWriteError);
  }

  if (hasUnflushedPages) {
    // If there are unflushed pages, the cache can't have been freed, so this
    // call is safe.
    savePages(cache);
  }
}

/**********************************************************************/
void releaseVDOPageCompletion(struct vdo_completion *completion)
{
  if (completion == NULL) {
    return;
  }

  struct page_info *discardInfo = NULL;
  struct vdo_page_completion *pageCompletion;
  if (completion->result == VDO_SUCCESS) {
    pageCompletion = validateCompletedPage(completion, false);
    if (--pageCompletion->info->busy == 0) {
      discardInfo = pageCompletion->info;
    }
  } else {
    // Do not check for errors if the completion was not successful.
    pageCompletion = asVDOPageCompletion(completion);
  }
  ASSERT_LOG_ONLY((pageCompletion->waiter.nextWaiter == NULL),
                  "Page being released after leaving all queues");

  struct vdo_page_cache *cache = pageCompletion->cache;
  assertOnCacheThread(cache, __func__);
  memset(pageCompletion, 0, sizeof(struct vdo_page_completion));

  if (discardInfo != NULL) {
    if (discardInfo->writeStatus == WRITE_STATUS_DEFERRED) {
      discardInfo->writeStatus = WRITE_STATUS_NORMAL;
      launchPageSave(discardInfo);
    }
    // if there are excess requests for pages (that have not already started
    // discards) we need to discard some page (which may be this one)
    discardPageIfNeeded(cache);
  }
}

/**
 * Helper function to load a page as described by a VDO Page Completion.
 *
 * @param info          the page info representing where to load the page
 * @param vdoPageComp   the VDO Page Completion describing the page
 **/
static void loadPageForCompletion(struct page_info           *info,
                                  struct vdo_page_completion *vdoPageComp)
{
  int result = enqueueWaiter(&info->waiting, &vdoPageComp->waiter);
  if (result != VDO_SUCCESS) {
    finishCompletion(&vdoPageComp->completion, result);
    return;
  }

  result = launchPageLoad(info, vdoPageComp->pbn);
  if (result != VDO_SUCCESS) {
    distributeErrorOverQueue(result, &info->waiting);
  }
}

/**********************************************************************/
void getVDOPageAsync(struct vdo_completion *completion)
{
  struct vdo_page_completion *vdoPageComp = asVDOPageCompletion(completion);
  struct vdo_page_cache      *cache       = vdoPageComp->cache;
  assertOnCacheThread(cache, __func__);

  if (vdoPageComp->writable && isReadOnly(cache->zone->readOnlyNotifier)) {
    finishCompletion(completion, VDO_READ_ONLY);
    return;
  }

  if (vdoPageComp->writable) {
    relaxedAdd64(&cache->stats.writeCount, 1);
  } else {
    relaxedAdd64(&cache->stats.readCount, 1);
  }

  struct page_info *info = vpcFindPage(cache, vdoPageComp->pbn);
  if (info != NULL) {
    // The page is in the cache already.
    if ((info->writeStatus == WRITE_STATUS_DEFERRED) || isIncoming(info)
        || (isOutgoing(info) && vdoPageComp->writable)) {
      // The page is unusable until it has finished I/O.
      relaxedAdd64(&cache->stats.waitForPage, 1);
      int result = enqueueWaiter(&info->waiting, &vdoPageComp->waiter);
      if (result != VDO_SUCCESS) {
        finishCompletion(&vdoPageComp->completion, result);
      }

      return;
    }

    if (isValid(info)) {
      // The page is usable.
      relaxedAdd64(&cache->stats.foundInCache, 1);
      if (!isPresent(info)) {
        relaxedAdd64(&cache->stats.readOutgoing, 1);
      }
      updateLru(info);
      ++info->busy;
      completeWithPage(info, vdoPageComp);
      return;
    }
    // Something horrible has gone wrong.
    ASSERT_LOG_ONLY(false, "Info found in a usable state.");
  }

  // The page must be fetched.
  info = findFreePage(cache);
  if (info != NULL) {
    relaxedAdd64(&cache->stats.fetchRequired, 1);
    loadPageForCompletion(info, vdoPageComp);
    return;
  }

  // The page must wait for a page to be discarded.
  relaxedAdd64(&cache->stats.discardRequired, 1);
  discardPageForCompletion(vdoPageComp);
}

/**********************************************************************/
void markCompletedVDOPageDirty(struct vdo_completion *completion,
                               SequenceNumber         oldDirtyPeriod,
                               SequenceNumber         newDirtyPeriod)
{
  struct vdo_page_completion *vdoPageComp = validateCompletedPage(completion,
                                                                  true);
  if (vdoPageComp == NULL) {
    return;
  }

  struct page_info *info = vdoPageComp->info;
  setInfoState(info, PS_DIRTY);
  addToDirtyLists(info->cache->dirtyLists, &info->listNode, oldDirtyPeriod,
                  newDirtyPeriod);
}

/**********************************************************************/
void requestVDOPageWrite(struct vdo_completion *completion)
{
  struct vdo_page_completion *vdoPageComp = validateCompletedPage(completion,
                                                                  true);
  if (vdoPageComp == NULL) {
    return;
  }

  struct page_info *info = vdoPageComp->info;
  setInfoState(info, PS_DIRTY);
  launchPageSave(info);
}

/**********************************************************************/
static void *dereferencePageCompletion(struct vdo_page_completion *completion)
{
  return ((completion != NULL) ? getPageBuffer(completion->info) : NULL);
}

/**********************************************************************/
const void *dereferenceReadableVDOPage(struct vdo_completion *completion)
{
  return dereferencePageCompletion(validateCompletedPage(completion, false));
}

/**********************************************************************/
void *dereferenceWritableVDOPage(struct vdo_completion *completion)
{
  return dereferencePageCompletion(validateCompletedPage(completion, true));
}

/**********************************************************************/
void *getVDOPageCompletionContext(struct vdo_completion *completion)
{
  struct vdo_page_completion *pageCompletion = asVDOPageCompletion(completion);
  struct page_info *info
    = ((pageCompletion != NULL) ? pageCompletion->info : NULL);
  return (((info != NULL) && isValid(info)) ? info->context : NULL);
}

/**********************************************************************/
void drainVDOPageCache(struct vdo_page_cache *cache)
{
  assertOnCacheThread(cache, __func__);
  ASSERT_LOG_ONLY(is_draining(&cache->zone->state),
                  "drainVDOPageCache() called during block map drain");

  if (!is_suspending(&cache->zone->state)) {
    flushDirtyLists(cache->dirtyLists);
    savePages(cache);
  }

  checkForIOComplete(cache);
}

/**********************************************************************/
int invalidateVDOPageCache(struct vdo_page_cache *cache)
{
  assertOnCacheThread(cache, __func__);

  // Make sure we don't throw away any dirty pages.
  struct page_info *info;
  for (info = cache->infos; info < cache->infos + cache->pageCount; info++) {
    int result = ASSERT(!isDirty(info), "cache must have no dirty pages");
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Reset the pageMap by re-allocating it.
  freeIntMap(&cache->pageMap);
  return makeIntMap(cache->pageCount, 0, &cache->pageMap);
}
