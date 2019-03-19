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
 * $Id: //eng/uds-releases/gloria/src/uds/indexZone.c#3 $
 */

#include "indexZone.h"

#include "errors.h"
#include "index.h"
#include "indexCheckpoint.h"
#include "indexRouter.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "request.h"
#include "sparseCache.h"
#include "uds.h"

/**********************************************************************/
int makeIndexZone(struct index *index, unsigned int zoneNumber, bool readOnly)
{
  IndexZone *zone;
  int result = ALLOCATE(1, IndexZone, "index zone", &zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeOpenChapter(index->volume->geometry, index->zoneCount,
                           &zone->openChapter);
  if (result != UDS_SUCCESS) {
    freeIndexZone(zone);
    return result;
  }

  if (!readOnly) {
    result = makeOpenChapter(index->volume->geometry, index->zoneCount,
                             &zone->writingChapter);
    if (result != UDS_SUCCESS) {
      freeIndexZone(zone);
      return result;
    }
  }

  zone->index              = index;
  zone->id                 = zoneNumber;
  index->zones[zoneNumber] = zone;

  return UDS_SUCCESS;
}

/**********************************************************************/
void freeIndexZone(IndexZone *zone)
{
  if (zone == NULL) {
    return;
  }

  freeOpenChapter(zone->openChapter);
  freeOpenChapter(zone->writingChapter);
  FREE(zone);
}

/**********************************************************************/
bool isZoneChapterSparse(const IndexZone *zone,
                         uint64_t         virtualChapter)
{
  return isChapterSparse(zone->index->volume->geometry,
                         zone->oldestVirtualChapter,
                         zone->newestVirtualChapter,
                         virtualChapter);
}

/**********************************************************************/
void setActiveChapters(IndexZone *zone)
{
  zone->oldestVirtualChapter = zone->index->oldestVirtualChapter;
  zone->newestVirtualChapter = zone->index->newestVirtualChapter;
}

/**
 * Swap the open and writing chapters after blocking until there are no active
 * chapter writers on the index.
 *
 * @param zone  The zone swapping chapters
 *
 * @return UDS_SUCCESS or a return code
 **/
static int swapOpenChapter(IndexZone *zone)
{
  // Wait for any currently writing chapter to complete
  int result = finishPreviousChapter(zone->index->chapterWriter,
                                     zone->newestVirtualChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Swap the writing and open chapters
  OpenChapterZone *tempChapter = zone->openChapter;
  zone->openChapter            = zone->writingChapter;
  zone->writingChapter         = tempChapter;
  return UDS_SUCCESS;
}

/**
 * Advance to a new open chapter, and forget the oldest chapter in the
 * index if necessary.
 *
 * @param zone                 The zone containing the chapter to reap
 *
 * @return UDS_SUCCESS or an error code
 **/
static int reapOldestChapter(IndexZone *zone)
{
  Index *index = zone->index;
  unsigned int chaptersPerVolume = index->volume->geometry->chaptersPerVolume;
  int result
    = ASSERT(((zone->newestVirtualChapter - zone->oldestVirtualChapter)
              <= chaptersPerVolume),
             "newest (%" PRIu64 ") and oldest (%" PRIu64 ") virtual chapters "
             "less than or equal to chapters per volume (%u)",
             zone->newestVirtualChapter, zone->oldestVirtualChapter,
             chaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }

  setMasterIndexZoneOpenChapter(index->masterIndex, zone->id,
                                zone->newestVirtualChapter);
  return UDS_SUCCESS;
}

/**********************************************************************/
int executeSparseCacheBarrierMessage(IndexZone          *zone,
                                     BarrierMessageData *barrier)
{
  /*
   * Check if the chapter index for the virtual chapter is already in the
   * cache, and if it's not, rendezvous with the other zone threads to add the
   * chapter index to the sparse index cache.
   */
  return updateSparseCache(zone, barrier->virtualChapter);
}

/**
 * Handle notification that some other zone has closed its open chapter. If
 * the chapter that was closed is still the open chapter for this zone,
 * close it now in order to minimize skew.
 *
 * @param zone          The zone receiving the notification
 * @param chapterClosed The notification
 *
 * @return UDS_SUCCESS or an error code
 **/
static int handleChapterClosed(IndexZone                *zone,
                               ChapterClosedMessageData *chapterClosed)
{
  if (zone->newestVirtualChapter == chapterClosed->virtualChapter) {
    return openNextChapter(zone, NULL);
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int dispatchIndexZoneControlRequest(Request *request)
{
  ZoneMessage *message = &request->zoneMessage;
  IndexZone *zone = message->index->zones[request->zoneNumber];

  switch (request->action) {
  case REQUEST_SPARSE_CACHE_BARRIER:
    return executeSparseCacheBarrierMessage(zone, &message->data.barrier);

  case REQUEST_ANNOUNCE_CHAPTER_CLOSED:
    return handleChapterClosed(zone, &message->data.chapterClosed);

  default:
    return ASSERT_FALSE("valid control message type: %d", request->action);
  }
}

/**
 * Announce the closure of the current open chapter to the other zones.
 *
 * @param request       The request which caused the chapter to close
 *                      (may be NULL)
 * @param zone          The zone which first closed the chapter
 * @param closedChapter The chapter which was closed
 *
 * @return UDS_SUCCESS or an error code
 **/
static int announceChapterClosed(Request   *request,
                                 IndexZone *zone,
                                 uint64_t   closedChapter)
{
  IndexRouter *router = ((request != NULL) ? request->router : NULL);

  ZoneMessage zoneMessage = {
    .index = zone->index,
    .data  = {
      .chapterClosed = { .virtualChapter = closedChapter }
    }
  };

  for (unsigned int i = 0; i < zone->index->zoneCount; i++) {
    if (zone->id == i) {
      continue;
    }
    int result;
    if (router != NULL) {
      result = launchZoneControlMessage(REQUEST_ANNOUNCE_CHAPTER_CLOSED,
                                        zoneMessage, i, router);
    } else {
      // We're in a test which doesn't have zone queues, so we can just
      // call the message function directly.
      result = handleChapterClosed(zone->index->zones[i],
                                   &zoneMessage.data.chapterClosed);
    }
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int openNextChapter(IndexZone *zone, Request *request)
{
  logDebug("closing chapter %" PRIu64 " of zone %d after %u entries (%u short)",
           zone->newestVirtualChapter, zone->id, zone->openChapter->size,
           zone->openChapter->capacity - zone->openChapter->size);

  int result = swapOpenChapter(zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  uint64_t closedChapter = zone->newestVirtualChapter++;
  result = reapOldestChapter(zone);
  if (result != UDS_SUCCESS) {
    return logUnrecoverable(result, "reapOldestChapter failed");
  }

  resetOpenChapter(zone->openChapter);

  // begin, continue, or finish the checkpoint processing
  // moved above startClosingChapter because some of the
  // checkpoint processing now done by the chapter writer thread
  result = processCheckpointing(zone->index,
                                zone->id,
                                zone->newestVirtualChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int finishedZones = startClosingChapter(zone->index->chapterWriter,
                                                   zone->id,
                                                   zone->writingChapter);
  if ((finishedZones == 1) && (zone->index->zoneCount > 1)) {
    // This is the first zone of a multi-zone index to close this chapter,
    // so inform the other zones in order to control zone skew.
    result = announceChapterClosed(request, zone, closedChapter);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  // If the chapter being opened won't overwrite the oldest chapter, we're
  // done.
  if (!areSamePhysicalChapter(zone->index->volume->geometry,
                              zone->newestVirtualChapter,
                              zone->oldestVirtualChapter)) {
    return UDS_SUCCESS;
  }

  uint64_t victim = zone->oldestVirtualChapter++;
  if (finishedZones < zone->index->zoneCount) {
    // We are not the last zone to close the chapter, so we're done
    return UDS_SUCCESS;
  }

  /*
   * We are the last zone to close the chapter, so clean up the cache. That
   * it is safe to let the last thread out of the previous chapter to do this
   * relies on the fact that although the new open chapter shadows the oldest
   * chapter in the cache, until we write the new open chapter to disk, we'll
   * never look for it in the cache.
   */
  return forgetChapter(zone->index->volume, victim, INVALIDATION_EXPIRE);
}

/**********************************************************************/
IndexRegion computeIndexRegion(const IndexZone *zone,
                               uint64_t         virtualChapter)
{
  if (virtualChapter == zone->newestVirtualChapter) {
    return LOC_IN_OPEN_CHAPTER;
  }
  return (isZoneChapterSparse(zone, virtualChapter)
          ? LOC_IN_SPARSE : LOC_IN_DENSE);
}

/**********************************************************************/
int getRecordFromZone(IndexZone *zone,
                      Request   *request,
                      bool      *found,
                      uint64_t   virtualChapter)
{
  if (virtualChapter == zone->newestVirtualChapter) {
    searchOpenChapter(zone->openChapter, &request->hash,
                      &request->oldMetadata, found);
    return UDS_SUCCESS;
  }

  if ((zone->newestVirtualChapter > 0)
      && (virtualChapter == (zone->newestVirtualChapter - 1))
      && (zone->writingChapter->size > 0)) {
    // Only search the writing chapter if it is full, else look on disk.
    searchOpenChapter(zone->writingChapter, &request->hash,
                      &request->oldMetadata, found);
    return UDS_SUCCESS;
  }

  // The slow lane thread has determined the location previously. We don't need
  // to search again. Just return the location.
  if (request->slLocationKnown) {
    *found = request->slLocation != LOC_UNAVAILABLE;
    return UDS_SUCCESS;
  }

  Volume *volume = zone->index->volume;
  if (isZoneChapterSparse(zone, virtualChapter)
      && sparseCacheContains(volume->sparseCache, virtualChapter,
                             request->zoneNumber)) {
    // The named chunk, if it exists, is in a sparse chapter that is cached,
    // so just run the chunk through the sparse chapter cache search.
    return searchSparseCacheInZone(zone, request, virtualChapter, found);
  }

  return searchVolumePageCache(volume, request, &request->hash, virtualChapter,
                               &request->oldMetadata, found);
}

/**********************************************************************/
int putRecordInZone(IndexZone          *zone,
                    Request            *request,
                    const UdsChunkData *metadata)
{
  unsigned int remaining;
  int result = putOpenChapter(zone->openChapter, &request->hash, metadata,
                              &remaining);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (remaining == 0) {
    return openNextChapter(zone, request);
  }

  return UDS_SUCCESS;
}

/**************************************************************************/
int searchSparseCacheInZone(IndexZone *zone,
                            Request   *request,
                            uint64_t   virtualChapter,
                            bool      *found)
{
  int recordPageNumber;
  int result = searchSparseCache(zone, &request->hash, &virtualChapter,
                                 &recordPageNumber);
  if ((result != UDS_SUCCESS) || (virtualChapter == UINT64_MAX)) {
    return result;
  }

  Volume *volume = zone->index->volume;
  // XXX map to physical chapter and validate. It would be nice to just pass
  // the virtual in to the slow lane, since it's tracking invalidations.
  unsigned int chapter
    = mapToPhysicalChapter(volume->geometry, virtualChapter);

  return searchCachedRecordPage(volume, request, &request->hash, chapter,
                                recordPageNumber, &request->oldMetadata,
                                found);
}
