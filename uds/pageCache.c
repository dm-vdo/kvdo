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
 * $Id: //eng/uds-releases/gloria/src/uds/pageCache.c#9 $
 */

#include "pageCache.h"

#include "atomicDefs.h"
#include "cacheCounters.h"
#include "chapterIndex.h"
#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "recordPage.h"
#include "stringUtils.h"
#include "threads.h"
#include "zone.h"

/**********************************************************************/
int assertPageInCache(PageCache *cache, CachedPage *page)
{
  int result = ASSERT((page->physicalPage < cache->numIndexEntries),
                      "physicalPage %u is valid (< %u)",
                      page->physicalPage, cache->numIndexEntries);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint16_t pageIndex = cache->index[page->physicalPage];
  return ASSERT((pageIndex < cache->numCacheEntries)
                && (&cache->cache[pageIndex] == page),
                "page is at expected location in cache");
}

/**
 * Clear a cache page.  Note: this does not clear readPending - a read could
 * still be pending and the read thread needs to be able to proceed and restart
 * the requests regardless. This page will still be marked invalid, but it
 * won't get reused (see getLeastRecentPage()) until the readPending flag
 * is cleared. This is a valid case, e.g. the chapter gets forgotten and
 * replaced with a new one in LRU.  Restarting the requests will lead them to
 * not find the records in the MI.
 *
 * @param cache   the cache
 * @param page    the cached page to clear
 *
 **/
static void clearPage(PageCache *cache, CachedPage *page)
{
  page->physicalPage = cache->numIndexEntries;
  WRITE_ONCE(page->lastUsed, 0);
}

/**
 * Get a page from the cache, but with no stats
 *
 * @param cache        the cache
 * @param physicalPage the physical page to get
 * @param queueIndex   the index of the page in the read queue if
 *                     queued, -1 otherwise
 * @param pagePtr      a pointer to hold the page
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int getPageNoStats(PageCache     *cache,
                          unsigned int   physicalPage,
                          int           *queueIndex,
                          CachedPage   **pagePtr)
{
  /*
   * ASSERTION: We are either a zone thread holding a searchPendingCounter,
   *            or we are any thread holding the readThreadsMutex.
   *
   * Holding only a searchPendingCounter is the most frequent case.
   */

  int result = ASSERT((physicalPage < cache->numIndexEntries),
                      "physical page %u is invalid", physicalPage);
  if (result != UDS_SUCCESS) {
    return result;
  }

  /*
   * It would be unlikely that the compiler turns the usage of indexValue into
   * two reads of cache->index, but it would be possible and very bad if those
   * reads did not return the same bits.
   */
  uint16_t indexValue = READ_ONCE(cache->index[physicalPage]);
  bool     queued     = (indexValue & VOLUME_CACHE_QUEUED_FLAG) != 0;
  uint16_t index      = indexValue & ~VOLUME_CACHE_QUEUED_FLAG;

  if (!queued && (index < cache->numCacheEntries)) {
    *pagePtr = &cache->cache[index];
    /*
     * We have acquired access to the cached page, but unless we hold the
     * readThreadsMutex, we need a read memory barrier now.  The corresponding
     * write memory barrier is in putPageInCache.
     */
    smp_rmb();
  } else {
    *pagePtr = NULL;
  }
  if (queueIndex != NULL) {
    *queueIndex = queued ? index : -1;
  }
  return UDS_SUCCESS;
}

/**
 * Wait for all pending searches on a page in the cache to complete
 *
 * @param cache         the page cache
 * @param physicalPage  the page to check searches on
 **/
static void waitForPendingSearches(PageCache *cache, unsigned int physicalPage)
{
  /*
   * We hold the readThreadsMutex.  We are waiting for threads that do not hold
   * the readThreadsMutex.  Those threads have "locked" their targeted page by
   * setting the searchPendingCounter.  The corresponding write memory barrier
   * is in beginPendingSearch.
   */
  smp_mb();

  InvalidateCounter initialCounters[cache->zoneCount];
  for (unsigned int i = 0; i < cache->zoneCount; i++) {
    initialCounters[i] = getInvalidateCounter(cache, i);
  }
  for (unsigned int i = 0; i < cache->zoneCount; i++) {
    if (searchPending(initialCounters[i])
        && (pageBeingSearched(initialCounters[i]) == physicalPage)) {
      // There is an active search using the physical page.
      // We need to wait for the search to finish.
      while (initialCounters[i] == getInvalidateCounter(cache, i)) {
        yieldScheduler();
      }
    }
  }
}

/**
 * Invalidate a cache page
 *
 * @param cache   the cache
 * @param page    the cached page
 * @param reason  the reason for invalidation, for stats
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int invalidatePageInCache(PageCache          *cache,
                                 CachedPage         *page,
                                 InvalidationReason  reason)
{
  // We hold the readThreadsMutex.
  if (page == NULL) {
    return UDS_SUCCESS;
  }

  if (page->physicalPage != cache->numIndexEntries) {
    switch (reason) {
    case INVALIDATION_EVICT:
      cache->counters.evictions++;
      break;
    case INVALIDATION_EXPIRE:
      cache->counters.expirations++;
      break;
    default:
      break;
    }

    if (reason != INVALIDATION_ERROR) {
      int result = assertPageInCache(cache, page);
      if (result != UDS_SUCCESS) {
        return result;
      }
    }

    WRITE_ONCE(cache->index[page->physicalPage], cache->numCacheEntries);
    waitForPendingSearches(cache, page->physicalPage);
  }

  clearPage(cache, page);

  return UDS_SUCCESS;
}

/**********************************************************************/
int findInvalidateAndMakeLeastRecent(PageCache          *cache,
                                     unsigned int        physicalPage,
                                     QueuedRead         *readQueue,
                                     InvalidationReason  reason,
                                     bool                mustFind)
{
  // We hold the readThreadsMutex.
  if (cache == NULL) {
    return UDS_SUCCESS;
  }

  CachedPage *page;
  int queuedIndex = -1;
  int result
    = getPageNoStats(cache, physicalPage,
                     ((readQueue != NULL) ? &queuedIndex : NULL), &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (page == NULL) {
    result = ASSERT(!mustFind, "found page");
    if (result != UDS_SUCCESS) {
      return result;
    }

    if (queuedIndex > -1) {
      logDebug("setting pending read to invalid");
      readQueue[queuedIndex].invalid = true;
    }
    return UDS_SUCCESS;
  }

  // Invalidate the page and unmap it from the cache.
  result = invalidatePageInCache(cache, page, reason);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Move the cached page to the least recently used end of the list
  // so it will be replaced before any page with valid data.
  WRITE_ONCE(page->lastUsed, 0);

  return UDS_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int initializePageCache(PageCache      *cache,
                               const Geometry *geometry,
                               unsigned int    chaptersInCache,
                               unsigned int    readQueueMaxSize,
                               unsigned int    zoneCount)
{
  cache->geometry  = geometry;
  cache->numIndexEntries = geometry->pagesPerVolume + 1;
  cache->numCacheEntries = chaptersInCache * geometry->recordPagesPerChapter;
  cache->readQueueMaxSize = readQueueMaxSize;
  cache->zoneCount = zoneCount;
  atomic64_set(&cache->clock, 1);

  int result = ALLOCATE(readQueueMaxSize, QueuedRead,
                        "volume read queue", &cache->readQueue);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ALLOCATE(cache->zoneCount, SearchPendingCounter,
                    "Volume Cache Zones", &cache->searchPendingCounters);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((cache->numCacheEntries <= VOLUME_CACHE_MAX_ENTRIES),
                  "requested cache size, %u, within limit %u",
                  cache->numCacheEntries, VOLUME_CACHE_MAX_ENTRIES);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ALLOCATE(cache->numIndexEntries, uint16_t, "page cache index",
                    &cache->index);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Initialize index values to invalid values.
  for (unsigned int i = 0; i < cache->numIndexEntries; i++) {
    cache->index[i] = cache->numCacheEntries;
  }

  result = ALLOCATE(cache->numCacheEntries, CachedPage,
                    "page cache cache", &cache->cache);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned long dataSize = geometry->bytesPerPage * cache->numCacheEntries;
  result = ALLOCATE_IO_ALIGNED(dataSize, byte, "cache page data",
                               &cache->data);
  if (result != UDS_SUCCESS) {
    return result;
  }

  for (unsigned int i = 0; i < cache->numCacheEntries; i++) {
    CachedPage *page = &cache->cache[i];
    page->data = cache->data + (i * cache->geometry->bytesPerPage);
    clearPage(cache, page);
  }

  return UDS_SUCCESS;
}

/*********************************************************************/
int makePageCache(const Geometry  *geometry,
                  unsigned int     chaptersInCache,
                  unsigned int     readQueueMaxSize,
                  unsigned int     zoneCount,
                  PageCache      **cachePtr)
{
  if (chaptersInCache < 1) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "cache size must be"
                                     " at least one chapter");
  }
  if (readQueueMaxSize <= 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "read queue max size must be"
                                     " greater than 0");
  }
  if (zoneCount < 1) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cache must have at least one zone");
  }

  PageCache *cache;
  int result = ALLOCATE(1, PageCache, "volume cache", &cache);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initializePageCache(cache, geometry, chaptersInCache,
                               readQueueMaxSize, zoneCount);
  if (result != UDS_SUCCESS) {
    freePageCache(cache);
    return result;
  }

  *cachePtr = cache;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freePageCache(PageCache *cache)
{
  if (cache == NULL) {
    return;
  }
  FREE(cache->index);
  FREE(cache->data);
  FREE(cache->cache);
  FREE(cache->searchPendingCounters);
  FREE(cache->readQueue);
  FREE(cache);
}

/**********************************************************************/
int invalidatePageCacheForChapter(PageCache          *cache,
                                  unsigned int        chapter,
                                  unsigned int        pagesPerChapter,
                                  InvalidationReason  reason)
{
  // We hold the readThreadsMutex.
  if ((cache == NULL) || (cache->cache == NULL)) {
    return UDS_SUCCESS;
  }

  int result;
  for (unsigned int i = 0; i < pagesPerChapter; i++) {
    unsigned int physicalPage = 1 + (pagesPerChapter * chapter) + i;
    result = findInvalidateAndMakeLeastRecent(cache, physicalPage,
                                              cache->readQueue,
                                              reason, false);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/*********************************************************************/
void makePageMostRecent(PageCache *cache, CachedPage *page)
{
  // ASSERTION: We are either a zone thread holding a searchPendingCounter,
  //            or we are any thread holding the readThreadsMutex.
  if (atomic64_read(&cache->clock) != READ_ONCE(page->lastUsed)) {
    WRITE_ONCE(page->lastUsed, atomic64_inc_return(&cache->clock));
  }
}

/**
 * Get the least recent valid page from the cache.
 *
 * @param cache    the cache
 * @param pagePtr  a pointer to hold the new page (will be set to NULL
 *                 if the page was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int getLeastRecentPage(PageCache *cache, CachedPage **pagePtr)
{
  // We hold the readThreadsMutex.
  int oldestIndex = 0;
  // Our first candidate is any page that does have a pending read.  We ensure
  // above that there are more entries than read threads, so there must be one.
  for (unsigned int i = 0;; i++) {
    if (i >= cache->numCacheEntries) {
      // This should never happen.
      return ASSERT(false, "oldest page is not NULL");
    }
    if (!cache->cache[i].readPending) {
      oldestIndex = i;
      break;
    }
  }
  // Now find the least recently used page that does not have a pending read.
  for (unsigned int i = 0; i < cache->numCacheEntries; i++) {
    if (!cache->cache[i].readPending
        && (READ_ONCE(cache->cache[i].lastUsed)
            <= READ_ONCE(cache->cache[oldestIndex].lastUsed))) {
      oldestIndex = i;
    }
  }
  *pagePtr = &cache->cache[oldestIndex];
  return UDS_SUCCESS;
}

/***********************************************************************/
int getPageFromCache(PageCache     *cache,
                     unsigned int   physicalPage,
                     int            probeType,
                     CachedPage   **pagePtr)
{
  // ASSERTION: We are in a zone thread.
  // ASSERTION: We holding a searchPendingCounter or the readThreadsMutex.
  if (cache == NULL) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "cannot get page with NULL cache");
  }

  // Get the cache page from the index
  CachedPage *page;
  int queueIndex = -1;
  int result = getPageNoStats(cache, physicalPage, &queueIndex, &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  CacheResultKind cacheResult = ((page != NULL)
                                 ? CACHE_RESULT_HIT
                                 : ((queueIndex != -1)
                                    ? CACHE_RESULT_QUEUED
                                    : CACHE_RESULT_MISS));
  incrementCacheCounter(&cache->counters, probeType, cacheResult);

  if (pagePtr != NULL) {
    *pagePtr = page;
  }
  return UDS_SUCCESS;
}

/***********************************************************************/
int enqueueRead(PageCache *cache, Request *request, unsigned int physicalPage)
{
  // We hold the readThreadsMutex.
  uint16_t first = cache->readQueueFirst;
  uint16_t last  = cache->readQueueLast;
  uint16_t next  = (last + 1) % cache->readQueueMaxSize;
  uint16_t readQueuePos;

  if ((cache->index[physicalPage] & VOLUME_CACHE_QUEUED_FLAG) == 0) {
    /* Not seen before, add this to the read queue and mark it as queued */
    if (next == first) {
      /* queue is full */
      return UDS_SUCCESS;
    }
    /* fill the read queue entry */
    cache->readQueue[last].physicalPage = physicalPage;
    cache->readQueue[last].invalid = false;

    /* point the cache index to it */
    readQueuePos = last;
    WRITE_ONCE(cache->index[physicalPage],
               readQueuePos | VOLUME_CACHE_QUEUED_FLAG);
    STAILQ_INIT(&cache->readQueue[readQueuePos].queueHead);
    /* bump the last pointer */
    cache->readQueueLast = next;
  } else {
    /* It's already queued, just add on to it */
    readQueuePos = cache->index[physicalPage] & ~VOLUME_CACHE_QUEUED_FLAG;
  }

  int result = ASSERT((readQueuePos < cache->readQueueMaxSize),
                      "queue is not overfull");
  if (result != UDS_SUCCESS) {
    return result;
  }

  STAILQ_INSERT_TAIL(&cache->readQueue[readQueuePos].queueHead, request, link);
  return UDS_QUEUED;
}

/***********************************************************************/
bool reserveReadQueueEntry(PageCache    *cache,
                           unsigned int *queuePos,
                           UdsQueueHead *queuedRequests,
                           unsigned int *physicalPage,
                           bool         *invalid)
{
  // We hold the readThreadsMutex.
  uint16_t lastRead = cache->readQueueLastRead;

  // No items to dequeue
  if (lastRead == cache->readQueueLast) {
    return false;
  }

  unsigned int pageNo    = cache->readQueue[lastRead].physicalPage;
  bool         isInvalid = cache->readQueue[lastRead].invalid;

  uint16_t indexValue = cache->index[pageNo];
  bool     queued     = (indexValue & VOLUME_CACHE_QUEUED_FLAG) != 0;

  // ALB-1429 ... need to check to see if its still queued before resetting
  if (isInvalid && queued) {
    // invalidate cache index slot
    WRITE_ONCE(cache->index[pageNo], cache->numCacheEntries);
  }

  // If a sync read has taken this page, set invalid to true so we don't
  // overwrite, we simply just requeue requests.
  if (!queued) {
    isInvalid = true;
  }

  cache->readQueue[lastRead].reserved = true;

  *queuePos                = lastRead;
  *queuedRequests          = cache->readQueue[lastRead].queueHead;
  *physicalPage            = pageNo;
  *invalid                 = isInvalid;
  cache->readQueueLastRead = (lastRead  + 1) % cache->readQueueMaxSize;

  return true;
}

/************************************************************************/
void releaseReadQueueEntry(PageCache *cache, unsigned int queuePos)
{
  // We hold the readThreadsMutex.
  cache->readQueue[queuePos].reserved = false;

  uint16_t lastRead = cache->readQueueLastRead;

  // Move the readQueueFirst pointer along when we can
  while ((cache->readQueueFirst != lastRead)
         && (!cache->readQueue[cache->readQueueFirst].reserved)) {
    cache->readQueueFirst =
      (cache->readQueueFirst + 1) % cache->readQueueMaxSize;
  }
}

/***********************************************************************/
int selectVictimInCache(PageCache   *cache,
                        CachedPage **pagePtr)
{
  // We hold the readThreadsMutex.
  if (cache == NULL) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "cannot put page in NULL cache");
  }

  CachedPage *page = NULL;
  int result = getLeastRecentPage(cache, &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((page != NULL), "least recent page was not NULL");
  if (result != UDS_SUCCESS) {
    return result;
  }

  // If the page is currently being pointed to by the page map, clear
  // it from the page map, and update cache stats
  if (page->physicalPage != cache->numIndexEntries) {
    cache->counters.evictions++;
    WRITE_ONCE(cache->index[page->physicalPage], cache->numCacheEntries);
    waitForPendingSearches(cache, page->physicalPage);
  }

  page->readPending = true;

  *pagePtr = page;

  return UDS_SUCCESS;
}

/***********************************************************************/
int putPageInCache(PageCache    *cache,
                   unsigned int  physicalPage,
                   CachedPage   *page)
{
  // We hold the readThreadsMutex.
  if (cache == NULL) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "cannot complete page in NULL cache");
  }

  int result = ASSERT((page != NULL), "page to install exists");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((page->readPending),
                  "page to install has a pending read");
  if (result != UDS_SUCCESS) {
    return result;
  }

  clearPage(cache, page);

  page->physicalPage = physicalPage;

  // Figure out the index into the cache array using pointer arithmetic
  uint16_t value = page - cache->cache;
  result = ASSERT((value < cache->numCacheEntries), "cache index is valid");
  if (result != UDS_SUCCESS) {
    return result;
  }

  makePageMostRecent(cache, page);

  page->readPending = false;

  /*
   * We hold the readThreadsMutex, but we must have a write memory barrier
   * before making the CachedPage available to the readers that do not hold the
   * mutex.  The corresponding read memory barrier is in getPageNoStats.
   */
  smp_wmb();

  // Point the page map to the new page. Will clear queued flag
  WRITE_ONCE(cache->index[physicalPage], value);

  return UDS_SUCCESS;
}

/***********************************************************************/
void cancelPageInCache(PageCache    *cache,
                       unsigned int  physicalPage,
                       CachedPage   *page)
{
  // We hold the readThreadsMutex.
  if (cache == NULL) {
    logWarning("cannot cancel page in NULL cache");
    return;
  }

  int result = ASSERT((page != NULL), "page to install exists");
  if (result != UDS_SUCCESS) {
    return;
  }

  result = ASSERT((page->readPending),
                  "page to install has a pending read");
  if (result != UDS_SUCCESS) {
    return;
  }

  clearPage(cache, page);
  page->readPending = false;

  // Clear the page map for the new page. Will clear queued flag
  WRITE_ONCE(cache->index[physicalPage], cache->numCacheEntries);
}

/**********************************************************************/
size_t getPageCacheSize(PageCache *cache)
{
  if (cache == NULL) {
    return 0;
  }
  return ((cache->geometry->bytesPerPage + sizeof(ChapterIndexPage))
          * cache->numCacheEntries);
}

/**********************************************************************/
void getPageCacheCounters(PageCache *cache, CacheCounters *counters)
{
  *counters = cache->counters;
}
