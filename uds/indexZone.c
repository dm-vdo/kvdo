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
 * $Id: //eng/uds-releases/krusty/src/uds/indexZone.c#9 $
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
int makeIndexZone(struct index *index, unsigned int zoneNumber)
{
  struct index_zone *zone;
  int result = ALLOCATE(1, struct index_zone, "index zone", &zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = makeOpenChapter(index->volume->geometry, index->zone_count,
                           &zone->openChapter);
  if (result != UDS_SUCCESS) {
    freeIndexZone(zone);
    return result;
  }

  result = makeOpenChapter(index->volume->geometry, index->zone_count,
                           &zone->writingChapter);
  if (result != UDS_SUCCESS) {
    freeIndexZone(zone);
    return result;
  }

  zone->index              = index;
  zone->id                 = zoneNumber;
  index->zones[zoneNumber] = zone;

  return UDS_SUCCESS;
}

/**********************************************************************/
void freeIndexZone(struct index_zone *zone)
{
  if (zone == NULL) {
    return;
  }

  freeOpenChapter(zone->openChapter);
  freeOpenChapter(zone->writingChapter);
  FREE(zone);
}

/**********************************************************************/
bool isZoneChapterSparse(const struct index_zone *zone,
                         uint64_t                 virtualChapter)
{
  return is_chapter_sparse(zone->index->volume->geometry,
                           zone->oldestVirtualChapter,
                           zone->newestVirtualChapter,
                           virtualChapter);
}

/**********************************************************************/
void setActiveChapters(struct index_zone *zone)
{
  zone->oldestVirtualChapter = zone->index->oldest_virtual_chapter;
  zone->newestVirtualChapter = zone->index->newest_virtual_chapter;
}

/**
 * Swap the open and writing chapters after blocking until there are no active
 * chapter writers on the index.
 *
 * @param zone  The zone swapping chapters
 *
 * @return UDS_SUCCESS or a return code
 **/
static int swapOpenChapter(struct index_zone *zone)
{
  // Wait for any currently writing chapter to complete
  int result = finish_previous_chapter(zone->index->chapter_writer,
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
static int reapOldestChapter(struct index_zone *zone)
{
  struct index *index = zone->index;
  unsigned int chaptersPerVolume
    = index->volume->geometry->chapters_per_volume;
  int result
    = ASSERT(((zone->newestVirtualChapter - zone->oldestVirtualChapter)
              <= chaptersPerVolume),
             "newest (%llu) and oldest (%llu) virtual chapters "
             "less than or equal to chapters per volume (%u)",
             zone->newestVirtualChapter, zone->oldestVirtualChapter,
             chaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }

  setMasterIndexZoneOpenChapter(index->master_index, zone->id,
                                zone->newestVirtualChapter);
  return UDS_SUCCESS;
}

/**********************************************************************/
int executeSparseCacheBarrierMessage(struct index_zone  *zone,
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
static int handleChapterClosed(struct index_zone        *zone,
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
  struct index_zone *zone = message->index->zones[request->zoneNumber];

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
static int announceChapterClosed(Request           *request,
                                 struct index_zone *zone,
                                 uint64_t           closedChapter)
{
  struct index_router *router = ((request != NULL) ? request->router : NULL);

  ZoneMessage zoneMessage = {
    .index = zone->index,
    .data  = {
      .chapterClosed = { .virtualChapter = closedChapter }
    }
  };

  unsigned int i;
  for (i = 0; i < zone->index->zone_count; i++) {
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
int openNextChapter(struct index_zone *zone, Request *request)
{
  logDebug("closing chapter %llu of zone %d after %u entries (%u short)",
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
  // moved above start_closing_chapter because some of the
  // checkpoint processing now done by the chapter writer thread
  result = process_checkpointing(zone->index,
                                 zone->id,
                                 zone->newestVirtualChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int finishedZones
    = start_closing_chapter(zone->index->chapter_writer, zone->id,
                            zone->writingChapter);
  if ((finishedZones == 1) && (zone->index->zone_count > 1)) {
    // This is the first zone of a multi-zone index to close this chapter,
    // so inform the other zones in order to control zone skew.
    result = announceChapterClosed(request, zone, closedChapter);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  // If the chapter being opened won't overwrite the oldest chapter, we're
  // done.
  if (!are_same_physical_chapter(zone->index->volume->geometry,
                                 zone->newestVirtualChapter,
                                 zone->oldestVirtualChapter)) {
    return UDS_SUCCESS;
  }

  uint64_t victim = zone->oldestVirtualChapter++;
  if (finishedZones < zone->index->zone_count) {
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
IndexRegion computeIndexRegion(const struct index_zone *zone,
                               uint64_t                 virtualChapter)
{
  if (virtualChapter == zone->newestVirtualChapter) {
    return LOC_IN_OPEN_CHAPTER;
  }
  return (isZoneChapterSparse(zone, virtualChapter)
          ? LOC_IN_SPARSE : LOC_IN_DENSE);
}

/**********************************************************************/
int getRecordFromZone(struct index_zone *zone,
                      Request           *request,
                      bool              *found,
                      uint64_t           virtualChapter)
{
  if (virtualChapter == zone->newestVirtualChapter) {
    searchOpenChapter(zone->openChapter, &request->chunkName,
                      &request->oldMetadata, found);
    return UDS_SUCCESS;
  }

  if ((zone->newestVirtualChapter > 0)
      && (virtualChapter == (zone->newestVirtualChapter - 1))
      && (zone->writingChapter->size > 0)) {
    // Only search the writing chapter if it is full, else look on disk.
    searchOpenChapter(zone->writingChapter, &request->chunkName,
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

  return searchVolumePageCache(volume, request, &request->chunkName,
                               virtualChapter, &request->oldMetadata, found);
}

/**********************************************************************/
int putRecordInZone(struct index_zone           *zone,
                    Request                     *request,
                    const struct uds_chunk_data *metadata)
{
  unsigned int remaining;
  int result = putOpenChapter(zone->openChapter, &request->chunkName, metadata,
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
int searchSparseCacheInZone(struct index_zone *zone,
                            Request           *request,
                            uint64_t           virtualChapter,
                            bool              *found)
{
  int recordPageNumber;
  int result = searchSparseCache(zone, &request->chunkName, &virtualChapter,
                                 &recordPageNumber);
  if ((result != UDS_SUCCESS) || (virtualChapter == UINT64_MAX)) {
    return result;
  }

  Volume *volume = zone->index->volume;
  // XXX map to physical chapter and validate. It would be nice to just pass
  // the virtual in to the slow lane, since it's tracking invalidations.
  unsigned int chapter
    = map_to_physical_chapter(volume->geometry, virtualChapter);

  return searchCachedRecordPage(volume, request, &request->chunkName, chapter,
                                recordPageNumber, &request->oldMetadata,
                                found);
}
