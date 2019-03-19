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
 * $Id: //eng/uds-releases/gloria/src/uds/volume.c#12 $
 */

#include "volume.h"

#include "cacheCounters.h"
#include "chapterIndex.h"
#include "compiler.h"
#include "errors.h"
#include "featureDefs.h"
#include "geometry.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "parameter.h"
#include "permassert.h"
#include "recordPage.h"
#include "request.h"
#include "sparseCache.h"
#include "stringUtils.h"
#include "threads.h"
#include "volumeInternals.h"

enum {
  MAX_BAD_CHAPTERS    = 100,   // max number of contiguous bad chapters
  VOLUME_READ_THREADS = 2      // Number of reader threads
};

static const NumericValidationData validRange = {
  .minValue = 1,
  .maxValue = MAX_VOLUME_READ_THREADS,
};

/**********************************************************************/
static UdsParameterValue getDefaultReadThreads(void)
{
  UdsParameterValue value;
#if ENVIRONMENT
  char *env = getenv(UDS_VOLUME_READ_THREADS);
  if (env != NULL) {
    UdsParameterValue tmp = {
      .type = UDS_PARAM_TYPE_STRING,
      .value.u_string = env,
    };
    if (validateNumericRange(&tmp, &validRange, &value) == UDS_SUCCESS) {
      return value;
    }
  }
#endif // ENVIRONMENT
  value.type = UDS_PARAM_TYPE_UNSIGNED_INT;
  value.value.u_uint = VOLUME_READ_THREADS;
  return value;
}

/**********************************************************************/
int defineVolumeReadThreads(ParameterDefinition *pd)
{
  pd->validate       = validateNumericRange;
  pd->validationData = &validRange;
  pd->currentValue   = getDefaultReadThreads();
  pd->update         = NULL;
  return UDS_SUCCESS;
}

/**********************************************************************/
int formatVolume(IORegion *region, const Geometry *geometry)
{
  // Create the header page with the magic number and version.
  byte *headerPage;
  int result = ALLOCATE_IO_ALIGNED(geometry->bytesPerPage, byte,
                                   "volume header page", &headerPage);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int volumeFormatLength = encodeVolumeFormat(headerPage, geometry);
  if (volumeFormatLength > geometry->bytesPerPage) {
    FREE(headerPage);
    logWarning("Volume format header is too large");
    return EINVAL;
  }

  // Write the full page to the start of the volume file.
  result = writeToRegion(region, 0, headerPage, geometry->bytesPerPage,
                         geometry->bytesPerPage);
  FREE(headerPage);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return syncRegionContents(region);
}

/**********************************************************************/
static INLINE unsigned int mapToPageNumber(Geometry     *geometry,
                                           unsigned int  physicalPage)
{
  return ((physicalPage - 1) % geometry->pagesPerChapter);
}

/**********************************************************************/
static INLINE unsigned int mapToChapterNumber(Geometry     *geometry,
                                              unsigned int  physicalPage)
{
  return ((physicalPage - 1) / geometry->pagesPerChapter);
}

/**********************************************************************/
static INLINE bool isRecordPage(Geometry *geometry, unsigned int physicalPage)
{
  return (((physicalPage - 1) % geometry->pagesPerChapter)
          >= geometry->indexPagesPerChapter);
}

/**********************************************************************/
static INLINE unsigned int getZoneNumber(Request *request)
{
  return (request == NULL) ? 0 : request->zoneNumber;
}

/**********************************************************************/
static void waitForReadQueueNotFull(Volume *volume, Request *request)
{
  unsigned int zoneNumber = getZoneNumber(request);
  InvalidateCounter invalidateCounter = getInvalidateCounter(volume->pageCache,
                                                             zoneNumber);
  if (searchPending(invalidateCounter)) {
    // Increment the invalidate counter to avoid deadlock where the reader
    // threads cannot make progress because they are waiting on the counter
    // and the index thread cannot because the read queue is full.
    endPendingSearch(volume->pageCache, zoneNumber);
  }

  while (readQueueIsFull(volume->pageCache)) {
    logDebug("Waiting until read queue not full");
    signalCond(&volume->readThreadsCond);
    waitCond(&volume->readThreadsReadDoneCond, &volume->readThreadsMutex);
  }

  if (searchPending(invalidateCounter)) {
    // Increment again so we get back to an odd value.
    beginPendingSearch(volume->pageCache, pageBeingSearched(invalidateCounter),
                       zoneNumber);
  }
}

/**********************************************************************/
int enqueuePageRead(Volume *volume, Request *request, int physicalPage)
{
  // Don't allow new requests if we are shutting down, but make sure
  // to process any requests that are still in the pipeline.
  if ((volume->readerState & READER_STATE_EXIT) != 0) {
    logInfo("failed to queue read while shutting down");
    return UDS_SHUTTINGDOWN;
  }

  // Mark the page as queued in the volume cache, for chapter invalidation to
  // be able to cancel a read.
  // If we are unable to do this because the queues are full, flush them first
  int result;
  while ((result = enqueueRead(volume->pageCache, request, physicalPage))
         == UDS_SUCCESS) {
    logDebug("Read queues full, waiting for reads to finish");
    waitForReadQueueNotFull(volume, request);
  }

  if (result == UDS_QUEUED) {
    /* signal a read thread */
    signalCond(&volume->readThreadsCond);
  }

  return result;
}

/**********************************************************************/
static INLINE void waitToReserveReadQueueEntry(Volume       *volume,
                                               unsigned int *queuePos,
                                               UdsQueueHead *queuedRequests,
                                               unsigned int *physicalPage,
                                               bool         *invalid)
{
  while (((volume->readerState & READER_STATE_EXIT) == 0)
         && (((volume->readerState & READER_STATE_STOP) != 0)
             || !reserveReadQueueEntry(volume->pageCache, queuePos,
                                       queuedRequests, physicalPage,
                                       invalid))) {
    waitCond(&volume->readThreadsCond, &volume->readThreadsMutex);
  }
}

/**********************************************************************/
static int initChapterIndexPage(const Volume     *volume,
                                byte             *indexPage,
                                unsigned int      chapter,
                                unsigned int      indexPageNumber,
                                ChapterIndexPage *chapterIndexPage)
{
  Geometry *geometry = volume->geometry;

  int result = initializeChapterIndexPage(chapterIndexPage, geometry,
                                          indexPage, volume->nonce);
  if (volume->lookupMode == LOOKUP_FOR_REBUILD) {
    return result;
  }
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "Reading chapter index page for chapter %u"
                                   " page %u",
                                   chapter, indexPageNumber);
  }

  IndexPageBounds bounds;
  result = getListNumberBounds(volume->indexPageMap, chapter,
                               indexPageNumber, &bounds);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint64_t     ciVirtual
    = getChapterIndexVirtualChapterNumber(chapterIndexPage);
  unsigned int ciChapter = mapToPhysicalChapter(geometry, ciVirtual);
  unsigned int ciLowest  = getChapterIndexLowestListNumber(chapterIndexPage);
  unsigned int ciHighest = getChapterIndexHighestListNumber(chapterIndexPage);
  if ((chapter == ciChapter)
      && (bounds.lowestList == ciLowest)
      && (bounds.highestList == ciHighest)) {
    return UDS_SUCCESS;
  }

  logWarning("Index page map updated to %" PRIu64,
             getLastUpdate(volume->indexPageMap));
  logWarning("Page map expects that chapter %u page %u has range %u to %u, "
             "but chapter index page has chapter %" PRIu64
             " with range %u to %u",
             chapter, indexPageNumber, bounds.lowestList, bounds.highestList,
             ciVirtual, ciLowest, ciHighest);
  return ASSERT_WITH_ERROR_CODE(false,
                                UDS_CORRUPT_DATA,
                                "index page map mismatch with chapter index");
}

/**********************************************************************/
static int initializeIndexPage(const Volume *volume,
                               unsigned int  physicalPage,
                               CachedPage   *page)
{
  unsigned int chapter = mapToChapterNumber(volume->geometry, physicalPage);
  unsigned int indexPageNumber = mapToPageNumber(volume->geometry,
                                                 physicalPage);
  int result = initChapterIndexPage(volume, page->data,
                                    chapter, indexPageNumber,
                                    &page->indexPage);
  return result;
}

/**********************************************************************/
static void readThreadFunction(void *arg)
{
  Volume       *volume  = arg;
  unsigned int  queuePos;
  UdsQueueHead  queuedRequests;
  unsigned int  physicalPage;
  bool          invalid = false;

  logDebug("reader starting");
  lockMutex(&volume->readThreadsMutex);
  while (true) {
    waitToReserveReadQueueEntry(volume, &queuePos, &queuedRequests,
                                &physicalPage, &invalid);
    if ((volume->readerState & READER_STATE_EXIT) != 0) {
      break;
    }

    volume->busyReaderThreads++;

    bool recordPage = isRecordPage(volume->geometry, physicalPage);

    CachedPage *page = NULL;
    int result = UDS_SUCCESS;
    if (!invalid) {
      // Find a place to put the read queue page we reserved above.
      result = selectVictimInCache(volume->pageCache, &page);
      if (result == UDS_SUCCESS) {
        unlockMutex(&volume->readThreadsMutex);
        result = readPageToBuffer(volume, physicalPage, page->data);
        if (result != UDS_SUCCESS) {
          logWarning("Error reading page %u from volume", physicalPage);
          cancelPageInCache(volume->pageCache, physicalPage, page);
        }
        lockMutex(&volume->readThreadsMutex);
      } else {
        logWarning("Error selecting cache victim for page read");
      }

      if (result == UDS_SUCCESS) {
        if (!volume->pageCache->readQueue[queuePos].invalid) {
          if (!recordPage) {
            result = initializeIndexPage(volume, physicalPage, page);
            if (result != UDS_SUCCESS) {
              logWarning("Error initializing chapter index page");
              cancelPageInCache(volume->pageCache, physicalPage, page);
            }
          }

          if (result == UDS_SUCCESS) {
            result = putPageInCache(volume->pageCache, physicalPage, page);
            if (result != UDS_SUCCESS) {
              logWarning("Error putting page %u in cache", physicalPage);
              cancelPageInCache(volume->pageCache, physicalPage, page);
            }
          }
        } else {
          logWarning("Page %u invalidated after read", physicalPage);
          cancelPageInCache(volume->pageCache, physicalPage, page);
          invalid = true;
        }
      }
    } else {
      logDebug("Requeuing requests for invalid page");
    }

    if (invalid) {
      result = UDS_SUCCESS;
      page = NULL;
    }

    while (!STAILQ_EMPTY(&queuedRequests)) {
      Request *request = STAILQ_FIRST(&queuedRequests);
      STAILQ_REMOVE_HEAD(&queuedRequests, link);

      /*
       * If we've read in a record page, we're going to do an immediate search,
       * in an attempt to speed up processing when we requeue the request, so
       * that it doesn't have to go back into the getRecordFromZone code again.
       * However, if we've just read in an index page, we don't want to search.
       * We want the request to be processed again and getRecordFromZone to be
       * run.  We have added new fields in request to allow the index code to
       * know whether it can stop processing before getRecordFromZone is called
       * again.
       */
      if ((result == UDS_SUCCESS) && (page != NULL) && recordPage) {
        if (searchRecordPage(page->data, &request->hash, volume->geometry,
                             &request->oldMetadata)) {
          request->slLocation = LOC_IN_DENSE;
        } else {
          request->slLocation = LOC_UNAVAILABLE;
        }
        request->slLocationKnown = true;
      }

      // reflect any read failures in the request status
      request->status = result;
      restartRequest(request);
    }

    releaseReadQueueEntry(volume->pageCache, queuePos);

    volume->busyReaderThreads--;
    broadcastCond(&volume->readThreadsReadDoneCond);
  }
  unlockMutex(&volume->readThreadsMutex);
  logDebug("reader done");
}

/**********************************************************************/
static int readPageLocked(Volume        *volume,
                          Request       *request,
                          unsigned int   physicalPage,
                          bool           syncRead,
                          CachedPage   **pagePtr)
{
  syncRead |= (volume->lookupMode == LOOKUP_FOR_REBUILD)
               || (request == NULL)
               || ((request->context == NULL)
                   && (request->serverContext == NULL));

  int result = UDS_SUCCESS;

  CachedPage *page = NULL;
  if (syncRead) {
    // Find a place to put the page.
    result = selectVictimInCache(volume->pageCache, &page);
    if (result != UDS_SUCCESS) {
      logWarning("Error selecting cache victim for page read");
      return result;
    }
    result = readPageToBuffer(volume, physicalPage, page->data);
    if (result != UDS_SUCCESS) {
      logWarning("Error reading page %u from volume", physicalPage);
      cancelPageInCache(volume->pageCache, physicalPage, page);
      return result;
    }
    if (!isRecordPage(volume->geometry, physicalPage)) {
      result = initializeIndexPage(volume, physicalPage, page);
      if (result != UDS_SUCCESS) {
        if (volume->lookupMode != LOOKUP_FOR_REBUILD) {
          logWarning("Corrupt index page %u", physicalPage);
        }
        cancelPageInCache(volume->pageCache, physicalPage, page);
        return result;
      }
    }
    result = putPageInCache(volume->pageCache, physicalPage, page);
    if (result != UDS_SUCCESS) {
      logWarning("Error putting page %u in cache", physicalPage);
      cancelPageInCache(volume->pageCache, physicalPage, page);
      return result;
    }
  } else {
    result = enqueuePageRead(volume, request, physicalPage);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  *pagePtr = page;

  return UDS_SUCCESS;
}

/**********************************************************************/
int getPageLocked(Volume          *volume,
                  Request         *request,
                  unsigned int     physicalPage,
                  CacheProbeType   probeType,
                  CachedPage     **pagePtr)
{
  CachedPage *page = NULL;
  int result = getPageFromCache(volume->pageCache, physicalPage, probeType,
                                &page);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (page == NULL) {
    result = readPageLocked(volume, request, physicalPage, true, &page);
    if (result != UDS_SUCCESS) {
      return result;
    }
  } else if (getZoneNumber(request) == 0) {
    // Only 1 zone is responsible for updating LRU
    makePageMostRecent(volume->pageCache, page);
  }

  *pagePtr = page;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getPageProtected(Volume          *volume,
                     Request         *request,
                     unsigned int     physicalPage,
                     CacheProbeType   probeType,
                     CachedPage     **pagePtr)
{
  CachedPage *page = NULL;
  int result = getPageFromCache(volume->pageCache, physicalPage,
                                probeType | CACHE_PROBE_IGNORE_FAILURE,
                                &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int zoneNumber = getZoneNumber(request);
  // If we didn't find a page we need to enqueue a read for it, in which
  // case we need to grab the mutex.
  if (page == NULL) {
    endPendingSearch(volume->pageCache, zoneNumber);
    lockMutex(&volume->readThreadsMutex);

    /*
     * Do the lookup again while holding the read mutex (no longer the fast
     * case so this should be ok to repeat). We need to do this because an
     * page may have been added to the page map by the reader thread between
     * the time searched above and the time we went to actually try to enqueue
     * it below. This could result in us enqueuing another read for an page
     * which is already in the cache, which would mean we end up with two
     * entries in the cache for the same page.
     */
    result
      = getPageFromCache(volume->pageCache, physicalPage, probeType, &page);
    if (result != UDS_SUCCESS) {
      /*
       * In non-success cases (anything not UDS_SUCCESS, meaning both
       * UDS_QUEUED and "real" errors), the caller doesn't get a
       * handle on a cache page, so it can't continue the search, and
       * we don't need to prevent other threads from messing with the
       * cache.
       *
       * However, we do need to set the "search pending" flag because
       * the callers expect it to always be set on return, even if
       * they can't actually do the search.
       *
       * Doing the calls in this order ought to be faster, since we
       * let other threads have the reader thread mutex (which can
       * require a syscall) ASAP, and set the "search pending" state
       * that can block the reader thread as the last thing.
       */
      unlockMutex(&volume->readThreadsMutex);
      beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);
      return result;
    }

    // If we found the page now, we can release the mutex and proceed
    // as if this were the fast case.
    if (page != NULL) {
      /*
       * If we found a page (*pagePtr != NULL and return
       * UDS_SUCCESS), then we're telling the caller where to look for
       * the cache page, and need to switch to "reader thread
       * unlocked" and "search pending" state in careful order so no
       * other thread can mess with the data before our caller gets to
       * look at it.
       */
      beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);
      unlockMutex(&volume->readThreadsMutex);
    }
  }

  if (page == NULL) {
    result = readPageLocked(volume, request, physicalPage, false, &page);
    if (result != UDS_SUCCESS) {
      /*
       * This code path is used frequently in the UDS_QUEUED case, so
       * the performance gain from unlocking first, while "search
       * pending" mode is off, turns out to be significant in some
       * cases.
       */
      unlockMutex(&volume->readThreadsMutex);
      beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);
      return result;
    }

    // See above re: ordering requirement.
    beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);
    unlockMutex(&volume->readThreadsMutex);
  } else {
    if (getZoneNumber(request) == 0 ) {
      // Only 1 zone is responsible for updating LRU
      makePageMostRecent(volume->pageCache, page);
    }
  }

  *pagePtr = page;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getPage(Volume            *volume,
            unsigned int       chapter,
            unsigned int       pageNumber,
            CacheProbeType     probeType,
            byte             **dataPtr,
            ChapterIndexPage **indexPagePtr)
{
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, pageNumber);

  lockMutex(&volume->readThreadsMutex);
  CachedPage *page = NULL;
  int result = getPageLocked(volume, NULL, physicalPage, probeType, &page);
  unlockMutex(&volume->readThreadsMutex);

  if (dataPtr != NULL) {
    *dataPtr = (page != NULL) ? page->data : NULL;
  }
  if (indexPagePtr != NULL) {
    *indexPagePtr = (page != NULL) ? &page->indexPage : NULL;
  }
  return result;
}

/**
 * Search for a chunk name in a cached index page or chapter index, returning
 * the record page number from a chapter index match.
 *
 * @param volume           the volume containing the index page to search
 * @param request          the request originating the search (may be NULL for
 *                         a direct query from volume replay)
 * @param name             the name of the block or chunk
 * @param chapter          the chapter to search
 * @param indexPageNumber  the index page number of the page to search
 * @param recordPageNumber pointer to return the chapter record page number
 *                         (value will be NO_CHAPTER_INDEX_ENTRY if the name
 *                         was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int searchCachedIndexPage(Volume             *volume,
                                 Request            *request,
                                 const UdsChunkName *name,
                                 unsigned int        chapter,
                                 unsigned int        indexPageNumber,
                                 int                *recordPageNumber)
{
  unsigned int zoneNumber = getZoneNumber(request);
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, indexPageNumber);

  /*
   * Make sure the invalidate counter is updated before we try and read from
   * the page map.  This prevents this thread from reading a page in the
   * page map which has already been marked for invalidation by the reader
   * thread, before the reader thread has noticed that the invalidateCounter
   * has been incremented.
   */
  beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);

  CachedPage *page = NULL;
  int result = getPageProtected(volume, request, physicalPage,
                                cacheProbeType(request, true), &page);
  if (result != UDS_SUCCESS) {
    endPendingSearch(volume->pageCache, zoneNumber);
    return result;
  }

  result
    = ASSERT_LOG_ONLY(searchPending(getInvalidateCounter(volume->pageCache,
                                                         zoneNumber)),
                      "Search is pending for zone %u", zoneNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = searchChapterIndexPage(&page->indexPage, volume->geometry, name,
                                  recordPageNumber);
  endPendingSearch(volume->pageCache, zoneNumber);
  return result;
}

/**********************************************************************/
int searchCachedRecordPage(Volume             *volume,
                           Request            *request,
                           const UdsChunkName *name,
                           unsigned int        chapter,
                           int                 recordPageNumber,
                           UdsChunkData       *duplicate,
                           bool               *found)
{
  *found = false;

  if (recordPageNumber == NO_CHAPTER_INDEX_ENTRY) {
    // No record for that name can exist in the chapter.
    return UDS_SUCCESS;
  }

  Geometry *geometry = volume->geometry;
  int result = ASSERT(((recordPageNumber >= 0)
                       && ((unsigned int) recordPageNumber
                           < geometry->recordPagesPerChapter)),
                      "0 <= %d <= %u",
                      recordPageNumber, geometry->recordPagesPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int pageNumber = geometry->indexPagesPerChapter + recordPageNumber;

  unsigned int zoneNumber = getZoneNumber(request);
  int physicalPage
    = mapToPhysicalPage(volume->geometry, chapter, pageNumber);

  /*
   * Make sure the invalidate counter is updated before we try and read from
   * the page map. This prevents this thread from reading a page in the page
   * map which has already been marked for invalidation by the reader thread,
   * before the reader thread has noticed that the invalidateCounter has been
   * incremented.
   */
  beginPendingSearch(volume->pageCache, physicalPage, zoneNumber);

  CachedPage *recordPage;
  result = getPageProtected(volume, request, physicalPage,
                            cacheProbeType(request, false), &recordPage);
  if (result != UDS_SUCCESS) {
    endPendingSearch(volume->pageCache, zoneNumber);
    return result;
  }

  if (searchRecordPage(recordPage->data, name, geometry, duplicate)) {
    *found = true;
  }
  endPendingSearch(volume->pageCache, zoneNumber);
  return UDS_SUCCESS;
}

/**********************************************************************/
int readChapterIndexFromVolume(const Volume     *volume,
                               uint64_t          virtualChapter,
                               byte              pageData[],
                               ChapterIndexPage  indexPages[])
{
  unsigned int physicalChapter
    = mapToPhysicalChapter(volume->geometry, virtualChapter);
  int result = readChapterIndexToBuffer(volume, physicalChapter, pageData);
  if (result != UDS_SUCCESS) {
    return result;
  }

  for (unsigned int i = 0; i < volume->geometry->indexPagesPerChapter; i++) {
    byte *indexPage = &pageData[i * volume->geometry->bytesPerPage];
    result = initChapterIndexPage(volume, indexPage, physicalChapter,
                                  i, &indexPages[i]);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int searchVolumePageCache(Volume             *volume,
                          Request            *request,
                          const UdsChunkName *name,
                          uint64_t            virtualChapter,
                          UdsChunkData       *metadata,
                          bool               *found)
{
  unsigned int physicalChapter
    = mapToPhysicalChapter(volume->geometry, virtualChapter);
  unsigned int indexPageNumber;
  int result = findIndexPageNumber(volume->indexPageMap, name, physicalChapter,
                                   &indexPageNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  int recordPageNumber;
  result = searchCachedIndexPage(volume, request, name, physicalChapter,
                                 indexPageNumber, &recordPageNumber);
  if (result == UDS_SUCCESS) {
    result = searchCachedRecordPage(volume, request, name, physicalChapter,
                                    recordPageNumber, metadata, found);
  }

  return result;
}

/**********************************************************************/
int forgetChapter(Volume             *volume,
                  uint64_t            virtualChapter,
                  InvalidationReason  reason)
{
  logDebug("forgetting chapter %" PRIu64, virtualChapter);
  unsigned int physicalChapter
    = mapToPhysicalChapter(volume->geometry, virtualChapter);
  lockMutex(&volume->readThreadsMutex);
  int result
    = invalidatePageCacheForChapter(volume->pageCache, physicalChapter,
                                    volume->geometry->pagesPerChapter,
                                    reason);
  unlockMutex(&volume->readThreadsMutex);
  return result;
}

/**********************************************************************/
static int writeScratchPage(Volume *volume, off_t *offset)
{
  int result = writeToRegion(volume->region, *offset, volume->scratchPage,
                             volume->geometry->bytesPerPage,
                             volume->geometry->bytesPerPage);
  *offset += volume->geometry->bytesPerPage;
  return result;
}

/**********************************************************************/
void updateVolumeSize(Volume *volume, off_t size)
{
  if (volume->volumeSize < size) {
    volume->volumeSize = size;
  }
}

/**
 * Donate index page data to the page cache for an index page that was just
 * written to the volume. The index page data is expected to be in the
 * volume's scratch page. The caller must already hold the reader thread
 * mutex.
 *
 * @param volume           the volume
 * @param physicalChapter  the physical chapter number of the index page
 * @param indexPageNumber  the chapter page number of the index page
 **/
static int donateIndexPageLocked(Volume       *volume,
                                 unsigned int  physicalChapter,
                                 unsigned int  indexPageNumber)
{
  unsigned int physicalPage
    = mapToPhysicalPage(volume->geometry, physicalChapter, indexPageNumber);

  // Find a place to put the page.
  CachedPage *page = NULL;
  int result = selectVictimInCache(volume->pageCache, &page);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Copy the scratch page containing the index page bytes to the cache page.
  memcpy(page->data, volume->scratchPage, volume->geometry->bytesPerPage);

  result = initChapterIndexPage(volume, page->data, physicalChapter,
                                indexPageNumber, &page->indexPage);
  if (result != UDS_SUCCESS) {
    logWarning("Error initialize chapter index page");
    cancelPageInCache(volume->pageCache, physicalPage, page);
    return result;
  }

  result = putPageInCache(volume->pageCache, physicalPage, page);
  if (result != UDS_SUCCESS) {
    logWarning("Error putting page %u in cache", physicalPage);
    cancelPageInCache(volume->pageCache, physicalPage, page);
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int writeIndexPages(Volume            *volume,
                    off_t             chapterOffset,
                    OpenChapterIndex  *chapterIndex,
                    byte             **pages)
{
  Geometry *geometry = volume->geometry;
  unsigned int physicalChapterNumber
    = mapToPhysicalChapter(geometry, chapterIndex->virtualChapterNumber);
  unsigned int deltaListNumber = 0;
  // The first chapter index page is written at the start of the chapter.
  off_t pageOffset = chapterOffset;

  for (unsigned int indexPageNumber = 0;
       indexPageNumber < geometry->indexPagesPerChapter;
       indexPageNumber++) {
    // Pack as many delta lists into the scratch page as will fit.
    unsigned int listsPacked;
    bool lastPage = ((indexPageNumber + 1) == geometry->indexPagesPerChapter);
    int result = packOpenChapterIndexPage(chapterIndex, volume->nonce,
                                          volume->scratchPage,
                                          deltaListNumber, lastPage,
                                          &listsPacked);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result, "failed to pack index page");
    }

    // Write the scratch page to the volume as the next chapter index page.
    result = writeScratchPage(volume, &pageOffset);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "failed to write chapter index page");
    }

    if (pages != NULL) {
      memcpy(pages[indexPageNumber], volume->scratchPage,
             geometry->bytesPerPage);
    }

    // Tell the index page map the list number of the last delta list that was
    // packed into the index page.
    if (listsPacked == 0) {
      logInfo("no delta lists packed on chapter %u page %u",
              physicalChapterNumber, indexPageNumber);
    } else {
      deltaListNumber += listsPacked;
    }
    result = updateIndexPageMap(volume->indexPageMap,
                                chapterIndex->virtualChapterNumber,
                                physicalChapterNumber,
                                indexPageNumber, deltaListNumber - 1);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "failed to update index page map");
    }

    // Donate the page data for the index page to the page cache.
    lockMutex(&volume->readThreadsMutex);
    result = donateIndexPageLocked(volume, physicalChapterNumber,
                                   indexPageNumber);
    unlockMutex(&volume->readThreadsMutex);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return ASSERT((pageOffset == (chapterOffset + geometry->recordPageOffset)),
                "unexpected page offset");
  return UDS_SUCCESS;
}

/**********************************************************************/
int writeRecordPages(Volume                *volume,
                     off_t                  chapterOffset,
                     const UdsChunkRecord   records[],
                     byte                 **pages)
{
  Geometry *geometry = volume->geometry;
  off_t pageOffset = chapterOffset + geometry->recordPageOffset;
  // The record array from the open chapter is 1-based.
  const UdsChunkRecord *nextRecord = &records[1];

  for (unsigned int recordPageNumber = 0;
       recordPageNumber < geometry->recordPagesPerChapter;
       recordPageNumber++) {
    // Sort the next page of records and copy them to the scratch page
    // as a binary tree stored in heap order.
    int result = encodeRecordPage(volume, nextRecord);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to encode record page %u",
                                       recordPageNumber);
    }
    nextRecord += geometry->recordsPerPage;

    // Write the scratch page to the volume as the next record page.
    result = writeScratchPage(volume, &pageOffset);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result, "failed to write record page %u",
                                       recordPageNumber);
    }

    if (pages != NULL) {
      memcpy(pages[recordPageNumber], volume->scratchPage,
             geometry->bytesPerPage);
    }
  }

  return ASSERT((pageOffset
                 == (chapterOffset + (off_t) geometry->bytesPerChapter)),
                "unexpected page offset");
}

/**********************************************************************/
int writeChapter(Volume                 *volume,
                 OpenChapterIndex       *chapterIndex,
                 const UdsChunkRecord    records[])
{
  // Determine the position of the virtual chapter in the volume file.
  Geometry *geometry = volume->geometry;
  unsigned int physicalChapterNumber
    = mapToPhysicalChapter(geometry, chapterIndex->virtualChapterNumber);
  int physicalPage = mapToPhysicalPage(geometry, physicalChapterNumber, 0);
  off_t chapterOffset = (off_t) physicalPage * (off_t) geometry->bytesPerPage;

  // Pack and write the delta chapter index pages to the volume.
  int result = writeIndexPages(volume, chapterOffset, chapterIndex, NULL);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Sort and write the record pages to the volume.
  result = writeRecordPages(volume, chapterOffset, records, NULL);
  if (result != UDS_SUCCESS) {
    return result;
  }
  updateVolumeSize(volume, chapterOffset + geometry->bytesPerChapter);
  return UDS_SUCCESS;
}

/**********************************************************************/
off_t getVolumeSize(Volume *volume)
{
  return volume->volumeSize;
}

/**********************************************************************/
size_t getCacheSize(Volume *volume)
{
  size_t size = getPageCacheSize(volume->pageCache);
  if (isSparse(volume->geometry)) {
    size += getSparseCacheMemorySize(volume->sparseCache);
  }
  return size;
}

/**********************************************************************/
void getCacheCounters(Volume *volume, CacheCounters *counters)
{
  getPageCacheCounters(volume->pageCache, counters);
  if (isSparse(volume->geometry)) {
    CacheCounters sparseCounters = getSparseCacheCounters(volume->sparseCache);
    addCacheCounters(counters, &sparseCounters);
  }
}

/**********************************************************************/
off_t offsetForChapter(const Geometry *geometry,
                       unsigned int    chapter)
{
  return geometry->bytesPerPage +                       // header page
    (((off_t) chapter) * geometry->bytesPerChapter);    // chapter span
}

/**********************************************************************/
static int probeChapter(Volume       *volume,
                        unsigned int  chapterNumber,
                        uint64_t     *virtualChapterNumber)
{
  const Geometry *geometry = volume->geometry;
  unsigned int expectedListNumber = 0;
  uint64_t lastVCN = UINT64_MAX;

  for (unsigned int i = 0; i < geometry->indexPagesPerChapter; ++i) {
    ChapterIndexPage *page;
    int result = getPage(volume, chapterNumber, i, CACHE_PROBE_INDEX_FIRST,
                         NULL, &page);
    if (result != UDS_SUCCESS) {
      return result;
    }

    uint64_t vcn = getChapterIndexVirtualChapterNumber(page);
    if (lastVCN == UINT64_MAX) {
      lastVCN = vcn;
    } else if (vcn != lastVCN) {
      logError("inconsistent chapter %u index page %u: expected vcn %"
               PRIu64 ", got vcn %" PRIu64,
               chapterNumber, i, lastVCN, vcn);
      return UDS_CORRUPT_COMPONENT;
    }

    if (expectedListNumber != getChapterIndexLowestListNumber(page)) {
      logError("inconsistent chapter %u index page %u: expected list number %u"
               ", got list number %u",
               chapterNumber, i, expectedListNumber,
               getChapterIndexLowestListNumber(page));
      return UDS_CORRUPT_COMPONENT;
    }
    expectedListNumber = getChapterIndexHighestListNumber(page) + 1;

    result = validateChapterIndexPage(page, geometry);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  if (lastVCN == UINT64_MAX) {
    logError("no chapter %u virtual chapter number determined", chapterNumber);
    return UDS_CORRUPT_COMPONENT;
  }
  if (chapterNumber != lastVCN % geometry->chaptersPerVolume) {
    logError("chapter %u vcn %" PRIu64 " is out of phase (%u)",
             chapterNumber, lastVCN, geometry->chaptersPerVolume);
    return UDS_CORRUPT_COMPONENT;
  }
  *virtualChapterNumber = lastVCN;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int probeWrapper(void         *aux,
                        unsigned int  chapterNumber,
                        uint64_t     *virtualChapterNumber)
{
  Volume *volume = aux;
  int result = probeChapter(volume, chapterNumber, virtualChapterNumber);
  if ((result == UDS_CORRUPT_COMPONENT) || (result == UDS_CORRUPT_DATA)) {
    *virtualChapterNumber = UINT64_MAX;
    return UDS_SUCCESS;
  }
  return result;
}

/**********************************************************************/
static int findRealEndOfVolume(Volume       *volume,
                               unsigned int  limit,
                               unsigned int *limitPtr)
{
  /*
   * Start checking from the end of the volume. As long as we hit corrupt
   * data, start skipping larger and larger amounts until we find real data.
   * If we find real data, reduce the span and try again until we find
   * the exact boundary.
   */
  unsigned int span = 1;
  unsigned int tries = 0;
  while (limit > 0) {
    unsigned int chapter = (span > limit) ? 0 : limit - span;
    uint64_t vcn = 0;
    int result = probeChapter(volume, chapter, &vcn);
    if (result == UDS_SUCCESS) {
      if (span == 1) {
        break;
      }
      span /= 2;
      tries = 0;
    } else if (result == UDS_CORRUPT_COMPONENT) {
      limit = chapter;
      if (++tries > 1) {
        span *= 2;
      }
    } else {
      return logErrorWithStringError(result, "cannot determine end of volume");
    }
  }

  if (limitPtr != NULL) {
    *limitPtr = limit;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int findVolumeChapterBoundaries(Volume   *volume,
                                uint64_t *lowestVCN,
                                uint64_t *highestVCN,
                                bool     *isEmpty)
{
  const Geometry *geometry = volume->geometry;

  off_t volSize = 0;

  int result = getRegionDataSize(volume->region, &volSize);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (volSize < (off_t) (geometry->bytesPerPage + geometry->bytesPerChapter)) {
    if (volSize == (off_t) (geometry->bytesPerPage)) {
      *lowestVCN = 0;
      *highestVCN = 0;
      *isEmpty = true;
      return UDS_SUCCESS;
    }

    return logErrorWithStringError(UDS_CORRUPT_COMPONENT,
                                   "invalid volume size (%" PRIu64 " bytes)",
                                   (uint64_t) volSize);
  }

  // determine highest present physical chapter number

  unsigned int chapterLimit = (unsigned int)
    ((volSize - geometry->bytesPerPage) / geometry->bytesPerChapter);

  logDebug("volume size %" PRIu64 " bytes, %u chapters",
           (uint64_t) volSize, chapterLimit);

  if (chapterLimit > geometry->chaptersPerVolume) {
    logWarning("excess chapters detected, expected no more than %u",
               geometry->chaptersPerVolume);
    chapterLimit = geometry->chaptersPerVolume;
  }

  result = findRealEndOfVolume(volume, chapterLimit, &chapterLimit);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "cannot find end of volume");
  }

  if (chapterLimit == 0) {
    *lowestVCN = 0;
    *highestVCN = 0;
    *isEmpty = true;
    return UDS_SUCCESS;
  }

  *isEmpty = false;
  return findVolumeChapterBoundariesImpl(chapterLimit, MAX_BAD_CHAPTERS,
                                         lowestVCN, highestVCN, probeWrapper,
                                         volume);
}

/**********************************************************************/
int findVolumeChapterBoundariesImpl(unsigned int  chapterLimit,
                                    unsigned int  maxBadChapters,
                                    uint64_t     *lowestVCN,
                                    uint64_t     *highestVCN,
                                    int (*probeFunc)(void         *aux,
                                                     unsigned int  chapter,
                                                     uint64_t     *vcn),
                                    void *aux)
{
  if (chapterLimit == 0) {
    *lowestVCN = 0;
    *highestVCN = 0;
    return UDS_SUCCESS;
  }

  /*
   * This method assumes there is at most one run of contiguous bad chapters
   * caused by unflushed writes. Either the bad spot is at the beginning and
   * end, or somewhere in the middle. Wherever it is, the highest and lowest
   * VCNs are adjacent to it. Otherwise the volume is cleanly saved and
   * somewhere in the middle of it the highest VCN immediately preceeds the
   * lowest one.
   */

  uint64_t firstVCN = UINT64_MAX;

  // doesn't matter if this results in a bad spot (UINT64_MAX)
  int result = (*probeFunc)(aux, 0, &firstVCN);
  if (result != UDS_SUCCESS) {
    return UDS_SUCCESS;
  }

  /*
   * Binary search for end of the discontinuity in the monotonically
   * increasing virtual chapter numbers; bad spots are treated as a span of
   * UINT64_MAX values. In effect we're searching for the index of the
   * smallest value less than firstVCN. In the case we go off the end it means
   * that chapter 0 has the lowest vcn.
   */

  unsigned int leftChapter = 0;
  unsigned int rightChapter = chapterLimit;

  while (leftChapter < rightChapter) {
    unsigned int chapter = (leftChapter + rightChapter) / 2;
    uint64_t probeVCN;

    result = (*probeFunc)(aux, chapter, &probeVCN);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (firstVCN <= probeVCN) {
      leftChapter = chapter + 1;
    } else {
      rightChapter = chapter;
    }
  }

  uint64_t lowest = UINT64_MAX;
  uint64_t highest = UINT64_MAX;

  result = ASSERT(leftChapter == rightChapter, "leftChapter == rightChapter");
  if (result != UDS_SUCCESS) {
    return result;
  }

  leftChapter %= chapterLimit;  // in case we're at the end

  // At this point, leftChapter is the chapter with the lowest virtual chapter
  // number.

  result = (*probeFunc)(aux, leftChapter, &lowest);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((lowest != UINT64_MAX), "invalid lowest chapter");
  if (result != UDS_SUCCESS) {
    return result;
  }

  // We now circularly scan backwards, moving over any bad chapters until we
  // find the chapter with the highest vcn (the first good chapter we
  // encounter).

  unsigned int badChapters = 0;

  for (;;) {
    rightChapter = (rightChapter + chapterLimit - 1) % chapterLimit;
    result = (*probeFunc)(aux, rightChapter, &highest);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (highest != UINT64_MAX) {
      break;
    }
    if (++badChapters >= maxBadChapters) {
      logError("too many bad chapters in volume: %u", badChapters);
      return UDS_CORRUPT_COMPONENT;
    }
  }

  *lowestVCN = lowest;
  *highestVCN = highest;
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeVolume(const Configuration  *config,
               IndexLayout          *layout,
               unsigned int          readQueueMaxSize,
               unsigned int          zoneCount,
               Volume              **newVolume)
{
  unsigned int volumeReadThreads;

  UdsParameterValue value;
  if ((udsGetParameter(UDS_VOLUME_READ_THREADS, &value) == UDS_SUCCESS) &&
      (value.type == UDS_PARAM_TYPE_UNSIGNED_INT)) {
    volumeReadThreads = value.value.u_uint;
  } else {
    volumeReadThreads = VOLUME_READ_THREADS;
  }

  if (readQueueMaxSize <= volumeReadThreads) {
    logError("Number of read threads must be smaller than read queue");
    return UDS_INVALID_ARGUMENT;
  }

  Volume *volume;

  int result = allocateVolume(config, layout, readQueueMaxSize, zoneCount,
                              !READ_ONLY_VOLUME, &volume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initMutex(&volume->readThreadsMutex);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = initCond(&volume->readThreadsReadDoneCond);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = initCond(&volume->readThreadsCond);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  // Start the reader threads.  If this allocation succeeds, freeVolume knows
  // that it needs to try and stop those threads.
  result = ALLOCATE(volumeReadThreads, Thread, "reader threads",
                    &volume->readerThreads);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  for (unsigned int i = 0; i < volumeReadThreads; i++) {
    result = createThread(readThreadFunction, (void *) volume, "reader",
                          &volume->readerThreads[i]);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return result;
    }
    // We only stop as many threads as actually got started.
    volume->numReadThreads = i + 1;
  }

  *newVolume = volume;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeVolume(Volume *volume)
{
  if (volume == NULL) {
    return;
  }

  // If readerThreads is NULL, then we haven't set up the reader threads.
  if (volume->readerThreads != NULL) {
    // Stop the reader threads.  It is ok if there aren't any of them.
    lockMutex(&volume->readThreadsMutex);
    volume->readerState |= READER_STATE_EXIT;
    broadcastCond(&volume->readThreadsCond);
    unlockMutex(&volume->readThreadsMutex);
    for (unsigned int i = 0; i < volume->numReadThreads; i++) {
      joinThreads(volume->readerThreads[i]);
    }
    FREE(volume->readerThreads);
    volume->readerThreads = NULL;
  }

  if (volume->region != NULL) {
    int result = syncAndCloseRegion(&volume->region, "index volume");
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result,
                              "error closing volume, releasing anyway");
    }
  }

  destroyCond(&volume->readThreadsCond);
  destroyCond(&volume->readThreadsReadDoneCond);
  destroyMutex(&volume->readThreadsMutex);
  freeIndexPageMap(volume->indexPageMap);
  freePageCache(volume->pageCache);
  freeRadixSorter(volume->radixSorter);
  freeSparseCache(volume->sparseCache);
  FREE(volume->geometry);
  FREE(volume->recordPointers);
  FREE(volume->scratchPage);
  FREE(volume);
}
