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
 * $Id: //eng/uds-releases/gloria/src/uds/index.c#9 $
 */

#include "index.h"

#include "hashUtils.h"
#include "indexCheckpoint.h"
#include "indexInternals.h"
#include "logger.h"

static const uint64_t NO_LAST_CHECKPOINT = UINT_MAX;

/**
 * Replay an index which was loaded from a checkpoint.
 *
 * @param index                 The index to replay
 * @param lastCheckpointChapter The number of the chapter where the
 *                              last checkpoint was made
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int replayIndexFromCheckpoint(Index *index,
                                     uint64_t lastCheckpointChapter)
{
  // Find the volume chapter boundaries
  uint64_t lowestVCN, highestVCN;
  bool isEmpty = false;
  IndexLookupMode oldLookupMode = index->volume->lookupMode;
  index->volume->lookupMode = LOOKUP_FOR_REBUILD;
  int result = findVolumeChapterBoundaries(index->volume, &lowestVCN,
                                           &highestVCN, &isEmpty);
  index->volume->lookupMode = oldLookupMode;
  if (result != UDS_SUCCESS) {
    return logFatalWithStringError(result,
                                   "cannot replay index: "
                                   "unknown volume chapter boundaries");
  }
  if (lowestVCN > highestVCN) {
    logFatal("cannot replay index: no valid chapters exist");
    return UDS_CORRUPT_COMPONENT;
  }

  if (isEmpty) {
    // The volume is empty, so the index should also be empty
    if (index->newestVirtualChapter != 0) {
      logFatal("cannot replay index from empty volume");
      return UDS_CORRUPT_COMPONENT;
    }
    return UDS_SUCCESS;
  }

  unsigned int chaptersPerVolume = index->volume->geometry->chaptersPerVolume;
  index->oldestVirtualChapter = lowestVCN;
  index->newestVirtualChapter = highestVCN + 1;
  if (index->newestVirtualChapter == lowestVCN + chaptersPerVolume) {
    // skip the chapter shadowed by the open chapter
    index->oldestVirtualChapter++;
  }

  uint64_t firstReplayChapter = lastCheckpointChapter;
  if (firstReplayChapter < index->oldestVirtualChapter) {
    firstReplayChapter = index->oldestVirtualChapter;
  }
  return replayVolume(index, firstReplayChapter);
}

/**********************************************************************/
static int loadIndex(Index *index, bool allowReplay)
{
  bool replayRequired = false;

  int result = loadIndexState(index->state, &replayRequired);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (replayRequired && !allowReplay) {
    return logErrorWithStringError(
      UDS_INDEX_NOT_SAVED_CLEANLY,
      "index not saved cleanly: open chapter missing");
  }

  uint64_t lastCheckpointChapter
    = ((index->lastCheckpoint != NO_LAST_CHECKPOINT)
       ? index->lastCheckpoint : 0);

  logInfo("loaded index from chapter %" PRIu64 " through chapter %" PRIu64,
          index->oldestVirtualChapter, lastCheckpointChapter);

  if (replayRequired) {
    result = replayIndexFromCheckpoint(index, lastCheckpointChapter);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  for (unsigned int i = 0; i < index->zoneCount; i++) {
    setActiveChapters(index->zones[i]);
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
static int rebuildIndex(Index *index)
{
  // Find the volume chapter boundaries
  uint64_t lowestVCN, highestVCN;
  bool isEmpty = false;
  IndexLookupMode oldLookupMode = index->volume->lookupMode;
  index->volume->lookupMode = LOOKUP_FOR_REBUILD;
  int result = findVolumeChapterBoundaries(index->volume, &lowestVCN,
                                           &highestVCN, &isEmpty);
  index->volume->lookupMode = oldLookupMode;
  if (result != UDS_SUCCESS) {
    return logFatalWithStringError(result,
                                   "cannot rebuild index: "
                                   "unknown volume chapter boundaries");
  }
  if (lowestVCN > highestVCN) {
    logFatal("cannot rebuild index: no valid chapters exist");
    return UDS_CORRUPT_COMPONENT;
  }

  if (isEmpty) {
    index->newestVirtualChapter = index->oldestVirtualChapter = 0;
  } else {
    unsigned int numChapters = index->volume->geometry->chaptersPerVolume;
    index->newestVirtualChapter = highestVCN + 1;
    index->oldestVirtualChapter = lowestVCN;
    if (index->newestVirtualChapter
        == (index->oldestVirtualChapter + numChapters)) {
      // skip the chapter shadowed by the open chapter
      index->oldestVirtualChapter++;
    }
  }

  if ((index->newestVirtualChapter - index->oldestVirtualChapter) >
      index->volume->geometry->chaptersPerVolume) {
    return logFatalWithStringError(UDS_CORRUPT_COMPONENT,
                                   "cannot rebuild index: "
                                   "volume chapter boundaries too large");
  }

  setMasterIndexOpenChapter(index->masterIndex, 0);
  if (isEmpty) {
    return UDS_SUCCESS;
  }

  result = replayVolume(index, index->oldestVirtualChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  for (unsigned int i = 0; i < index->zoneCount; i++) {
    setActiveChapters(index->zones[i]);
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int makeIndex(IndexLayout          *layout,
              const Configuration  *config,
              unsigned int          zoneCount,
              LoadType              loadType,
              Index               **newIndex)
{
  Index *index;
  int result = allocateIndex(layout, config, zoneCount, loadType,
                             !READ_ONLY_INDEX, &index);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "could not allocate index");
  }

  uint64_t nonce = getVolumeNonce(layout);
  result = makeMasterIndex(config, zoneCount, nonce, &index->masterIndex);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return logErrorWithStringError(result, "could not make master index");
  }

  result = addIndexStateComponent(index->state, MASTER_INDEX_INFO, NULL,
                                  index->masterIndex);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  result = addIndexStateComponent(index->state, &INDEX_PAGE_MAP_INFO,
                                  index->volume->indexPageMap, NULL);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  result = makeChapterWriter(index, &index->chapterWriter);
  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return result;
  }

  if ((loadType == LOAD_LOAD) || (loadType == LOAD_REBUILD)) {
    if (!index->existed) {
      freeIndex(index);
      return UDS_NO_INDEX;
    }
    result = loadIndex(index, loadType == LOAD_REBUILD);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "index could not be loaded");
      if (loadType == LOAD_REBUILD) {
        result = rebuildIndex(index);
        if (result != UDS_SUCCESS) {
          logErrorWithStringError(result, "index could not be rebuilt");
        }
      }
    }
  } else {
    discardIndexStateData(index->state);
  }

  if (result != UDS_SUCCESS) {
    freeIndex(index);
    return logUnrecoverable(result, "fatal error in makeIndex");
  }

  *newIndex = index;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeIndex(Index *index)
{
  if (index == NULL) {
    return;
  }
  freeChapterWriter(index->chapterWriter);

  if (index->masterIndex != NULL) {
    freeMasterIndex(index->masterIndex);
  }
  releaseIndex(index);
}

/**********************************************************************/
static int saveIndexComponents(Index *index)
{
  int result = finishCheckpointing(index);
  if (result != UDS_SUCCESS) {
    logInfo("save index failed");
    return result;
  }
  beginSave(index, false, index->newestVirtualChapter);

  result = saveIndexState(index->state);
  if (result != UDS_SUCCESS) {
    logInfo("save index failed");
    index->lastCheckpoint = index->prevCheckpoint;
  }

  return result;
}

/**********************************************************************/
int saveIndex(Index *index)
{
  // Wait for the writing chapter file to be finish.
  int result = stopChapterWriter(index->chapterWriter);
  if (result != UDS_SUCCESS) {
    // If we couldn't save the volume, the index state is useless
    discardIndexStateData(index->state);
    return result;
  }

  // Ensure that the volume is securely on storage
  result = syncRegionContents(index->volume->region);
  switch (result) {
  case UDS_SUCCESS:
  case UDS_UNSUPPORTED:
    break;
  default:
    // If we couldn't save the volume, the index state is useless
    discardIndexStateData(index->state);
    return logErrorWithStringError(result, "cannot sync volume IORegion");
  }

  return saveIndexComponents(index);
}

/**
 * Get the zone for a request.
 *
 * @param index The index
 * @param request The request
 *
 * @return The zone for the request
 **/
static IndexZone *getRequestZone(Index *index, Request *request)
{
  return index->zones[request->zoneNumber];
}

/**
 * Search an index zone. This function is only correct for LRU.
 *
 * @param zone              The index zone to query.
 * @param request           The request originating the query.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int searchIndexZone(IndexZone *zone, Request *request)
{
  MasterIndexRecord record;
  int result = getMasterIndexRecord(zone->index->masterIndex, &request->hash,
                                    &record);
  if (result != UDS_SUCCESS) {
    return result;
  }

  bool found = false;
  if (record.isFound) {
    result = getRecordFromZone(zone, request, &found, record.virtualChapter);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (found) {
      request->location = computeIndexRegion(zone, record.virtualChapter);
    }
  }

  /*
   * If a record has overflowed a chapter index in more than one chapter
   * (or overflowed in one chapter and collided with an existing record),
   * it will exist as a collision record in the master index, but we won't
   * find it in the volume. This case needs special handling.
   */
  bool     overflowRecord = (record.isFound && record.isCollision && !found);
  uint64_t chapter        = zone->newestVirtualChapter;
  if (found || overflowRecord) {
    if ((request->action == REQUEST_QUERY)
        && (!request->update || overflowRecord)) {
      /* This is a query without update, or with nothing to update */
      return UDS_SUCCESS;
    }

    if (record.virtualChapter != chapter) {
      /*
       * Update the master index to reference the new chapter for the block.
       * If the record had been deleted or dropped from the chapter index, it
       * will be back.
       */
      result = setMasterIndexRecordChapter(&record, chapter);
    } else if (request->action != REQUEST_UPDATE) {
      /* The record is already in the open chapter, so we're done */
      return UDS_SUCCESS;
    }
  } else {
    // The record wasn't in the master index, so check whether the name
    // is in a cached sparse chapter.
    if (!isMasterIndexSample(zone->index->masterIndex, &request->hash)
        && isSparse(zone->index->volume->geometry)) {
      // Passing UINT64_MAX triggers a search of the entire sparse cache.
      result = searchSparseCacheInZone(zone, request, UINT64_MAX, &found);
      if (result != UDS_SUCCESS) {
        return result;
      }

      if (found) {
        request->location = LOC_IN_SPARSE;
      }
    }

    if (request->action == REQUEST_QUERY) {
      if (!found || !request->update) {
        // This is a query without update or for a new record, so we're done.
        return UDS_SUCCESS;
      }
    }

    /*
     * Add a new entry to the master index referencing the open chapter.
     * This needs to be done both for new records, and for records from
     * cached sparse chapters.
     */
    result = putMasterIndexRecord(&record, chapter);
  }

  if (result == UDS_OVERFLOW) {
    /*
     * The master index encountered a delta list overflow.  The condition
     * was already logged. We will go on without adding the chunk to the
     * open chapter.
     */
    return UDS_SUCCESS;
  }

  if (result != UDS_SUCCESS) {
    return result;
  }

  UdsChunkData *metadata;
  if (!found || (request->action == REQUEST_UPDATE)) {
    // This is a new record or we're updating an existing record.
    metadata = &request->newMetadata;
  } else {
    // This is a duplicate, so move the record to the open chapter (for LRU).
    metadata = &request->oldMetadata;
  }
  return putRecordInZone(zone, request, metadata);
}

/**********************************************************************/
static int removeFromIndexZone(IndexZone *zone, Request *request)
{
  MasterIndexRecord record;
  int result = getMasterIndexRecord(zone->index->masterIndex, &request->hash,
                                    &record);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (!record.isFound) {
    // The name does not exist in master index, so there is nothing to remove.
    return UDS_SUCCESS;
  }

  if (!record.isCollision) {
    // Non-collision records are hints, so resolve the name in the chapter.
    bool found;
    int result = getRecordFromZone(zone, request, &found,
                                   record.virtualChapter);
    if (result != UDS_SUCCESS) {
      return result;
    }

    if (!found) {
      // The name does not exist in the chapter, so there is nothing to remove.
      return UDS_SUCCESS;
    }
  }

  request->location = computeIndexRegion(zone, record.virtualChapter);

  /*
   * Delete the master index entry for the named record only. Note that a
   * later search might later return stale advice if there is a colliding name
   * in the same chapter, but it's a very rare case (1 in 2^21).
   */
  result = removeMasterIndexRecord(&record);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // If the record is in the open chapter, we must remove it or mark it
  // deleted to avoid trouble if the record is added again later.
  if (request->location == LOC_IN_OPEN_CHAPTER) {
    bool hashExists = false;
    removeFromOpenChapter(zone->openChapter, &request->hash, &hashExists);
    result = ASSERT(hashExists, "removing record not found in open chapter");
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/**
 * Simulate the creation of a sparse cache barrier message by the triage
 * queue, and the later execution of that message in an index zone.
 *
 * If the index receiving the request is multi-zone or dense, this function
 * does nothing. This simulation is an optimization for single-zone sparse
 * indexes. It also supports unit testing of indexes without routers and
 * queues.
 *
 * @param zone     the index zone responsible for the index request
 * @param request  the index request about to be executed
 *
 * @return UDS_SUCCESS always
 **/
static int simulateIndexZoneBarrierMessage(IndexZone *zone, Request *request)
{
  // Do nothing unless this is a single-zone sparse index.
  if ((zone->index->zoneCount > 1)
      || !isSparse(zone->index->volume->geometry)) {
    return UDS_SUCCESS;
  }

  // Check if the index request is for a sampled name in a sparse chapter.
  uint64_t sparseVirtualChapter = triageIndexRequest(zone->index, request);
  if (sparseVirtualChapter == UINT64_MAX) {
    // Not indexed, not a hook, or in a chapter that is still dense, which
    // means there should be no change to the sparse chapter index cache.
    return UDS_SUCCESS;
  }

  /*
   * The triage queue would have generated and enqueued a barrier message
   * preceding this request, which we simulate by directly invoking the
   * execution hook for an equivalent message.
   */
  BarrierMessageData barrier = { .virtualChapter = sparseVirtualChapter };
  return executeSparseCacheBarrierMessage(zone, &barrier);
}

/**********************************************************************/
static int dispatchIndexZoneRequest(IndexZone *zone, Request *request)
{
  if (!request->requeued) {
    // Single-zone sparse indexes don't have a triage queue to generate cache
    // barrier requests, so see if we need to synthesize a barrier.
    int result = simulateIndexZoneBarrierMessage(zone, request);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  // Set the default location. It will be overwritten if we find the chunk.
  request->location = LOC_UNAVAILABLE;

  int result;
  switch (request->action) {
  case REQUEST_INDEX:
  case REQUEST_UPDATE:
  case REQUEST_QUERY:
    result = searchIndexZone(zone, request);
    result = logUnrecoverable(result, "searchIndexZone() failed");
    break;

  case REQUEST_DELETE:
    result = removeFromIndexZone(zone, request);
    result = logUnrecoverable(result, "removeFromIndexZone() failed");
    break;

  default:
    result = logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                       "attempted to execute invalid action:"
                                       " %d",
                                       request->action);
  }

  return result;
}

/**********************************************************************/
int dispatchIndexRequest(Index *index, Request *request)
{
  return dispatchIndexZoneRequest(getRequestZone(index, request), request);
}

/**********************************************************************/
static int rebuildIndexPageMap(Index *index, uint64_t vcn)
{
  Geometry *geometry = index->volume->geometry;
  unsigned int chapter = mapToPhysicalChapter(geometry, vcn);
  for (unsigned int indexPageNumber = 0;
       indexPageNumber < geometry->indexPagesPerChapter;
       indexPageNumber++) {
    ChapterIndexPage *chapterIndexPage;
    int result = getPage(index->volume, chapter, indexPageNumber,
                         CACHE_PROBE_INDEX_FIRST, NULL, &chapterIndexPage);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "failed to read index page %u"
                                     " in chapter %u",
                                     indexPageNumber, chapter);
    }
    unsigned int highestDeltaList
      = getChapterIndexHighestListNumber(chapterIndexPage);
    result = updateIndexPageMap(index->volume->indexPageMap, vcn, chapter,
                                indexPageNumber, highestDeltaList);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "failed to update chapter %u index page"
                                     " %u",
                                     chapter, indexPageNumber);
    }
  }
  return UDS_SUCCESS;
}

/**
 * Add an entry to the master index when rebuilding.
 *
 * @param index                The index to query.
 * @param name                 The block name of interest.
 * @param virtualChapter       The virtual chapter number to write to the
 *                             master index
 * @param willBeSparseChapter  True if this entry will be in the sparse portion
 *                             of the index at the end of rebuilding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int replayRecord(Index              *index,
                        const UdsChunkName *name,
                        uint64_t            virtualChapter,
                        bool                willBeSparseChapter)
{
  if (willBeSparseChapter && !isMasterIndexSample(index->masterIndex, name)) {
    // This entry will be in a sparse chapter after the rebuild completes,
    // and it is not a sample, so just skip over it.
    return UDS_SUCCESS;
  }

  MasterIndexRecord record;
  int result = getMasterIndexRecord(index->masterIndex, name, &record);
  if (result != UDS_SUCCESS) {
    return result;
  }

  bool updateRecord;
  if (record.isFound) {
    if (record.isCollision) {
      if (record.virtualChapter == virtualChapter) {
        /* The record is already correct, so we don't need to do anything */
        return UDS_SUCCESS;
      }
      updateRecord = true;
    } else if (record.virtualChapter == virtualChapter) {
      /*
       * There is a master index entry pointing to the current
       * chapter, but we don't know if it is for the same name as the
       * one we are currently working on or not. For now, we're just
       * going to assume that it isn't. This will create one extra
       * collision record if there was a deleted record in the current
       * chapter.
       */
      updateRecord = false;
    } else {
      /*
       * If we're rebuilding, we don't normally want to go to disk to see if
       * the record exists, since we will likely have just read the record from
       * disk (i.e. we know it's there). The exception to this is when we
       * already find an entry in the master index that has a different chapter.
       * In this case, we need to search that chapter to determine if the
       * master index entry was for the same record or a different one.
       */
      result = searchVolumePageCache(index->volume, NULL, name,
                                     record.virtualChapter, NULL,
                                     &updateRecord);
      if (result != UDS_SUCCESS) {
        return result;
      }
    }
  } else {
    updateRecord = false;
  }

  if (updateRecord) {
    /*
     * Update the master index to reference the new chapter for the block.
     * If the record had been deleted or dropped from the chapter index, it
     * will be back.
     */
    result = setMasterIndexRecordChapter(&record, virtualChapter);
  } else {
    /*
     * Add a new entry to the master index referencing the open
     * chapter. This should be done regardless of whether we are a brand
     * new record or a sparse record, i.e. one that doesn't exist in the
     * index but does on disk, since for a sparse record, we would want to
     * un-sparsify if it did exist.
     */
    result = putMasterIndexRecord(&record, virtualChapter);
  }

  if ((result == UDS_DUPLICATE_NAME) || (result == UDS_OVERFLOW)) {
    /* Ignore duplicate record and delta list overflow errors */
    return UDS_SUCCESS;
  }

  return result;
}

/**********************************************************************/
void beginSave(Index *index, bool checkpoint, uint64_t openChapterNumber)
{
  index->prevCheckpoint = index->lastCheckpoint;
  index->lastCheckpoint = ((openChapterNumber == 0)
                           ? NO_LAST_CHECKPOINT
                           : openChapterNumber - 1);

  const char *what = (checkpoint ? "checkpoint" : "save");
  logInfo("beginning %s (vcn %" PRIu64 ")", what, index->lastCheckpoint);

}

/**********************************************************************/
int replayVolume(Index *index, uint64_t fromVCN)
{
  int result;
  uint64_t uptoVCN = index->newestVirtualChapter;
  logInfo("Replaying volume from chapter %" PRIu64 " through chapter %"
          PRIu64,
          fromVCN, uptoVCN);
  setMasterIndexOpenChapter(index->masterIndex, uptoVCN);
  setMasterIndexOpenChapter(index->masterIndex, fromVCN);

  /*
   * At least two cases to deal with here!
   * - index loaded but replaying from lastCheckpoint; maybe full, maybe not
   * - index failed to load, full rebuild
   *   Starts empty, then dense-only, then dense-plus-sparse.
   *   Need to sparsify while processing individual chapters.
   */
  IndexLookupMode oldLookupMode = index->volume->lookupMode;
  index->volume->lookupMode = LOOKUP_FOR_REBUILD;
  /*
   * Go through each record page of each chapter and add the records back to
   * the master index.  This should not cause anything to be written to either
   * the open chapter or on disk volume.  Also skip the on disk chapter
   * corresponding to upto, as this would have already been
   * purged from the master index when the chapter was opened.
   *
   * Also, go through each index page for each chapter and rebuild the
   * index page map.
   */
  const Geometry *geometry = index->volume->geometry;
  uint64_t oldIPMupdate = getLastUpdate(index->volume->indexPageMap);
  for (uint64_t vcn = fromVCN; vcn < uptoVCN; ++vcn) {
    bool willBeSparseChapter = isChapterSparse(index->volume->geometry,
                                               fromVCN, uptoVCN, vcn);
    unsigned int chapter = mapToPhysicalChapter(geometry, vcn);
    setMasterIndexOpenChapter(index->masterIndex, vcn);
    result = rebuildIndexPageMap(index, vcn);
    if (result != UDS_SUCCESS) {
      index->volume->lookupMode = oldLookupMode;
      return logErrorWithStringError(result,
                                     "could not rebuild index page map for"
                                     " chapter %u",
                                     chapter);
    }

    for (unsigned int j = 0; j < geometry->recordPagesPerChapter; j++) {
      unsigned int recordPageNumber = geometry->indexPagesPerChapter + j;
      byte *recordPage;
      result = getPage(index->volume, chapter, recordPageNumber,
                       CACHE_PROBE_RECORD_FIRST, &recordPage, NULL);
      if (result != UDS_SUCCESS) {
        index->volume->lookupMode = oldLookupMode;
        return logUnrecoverable(result, "could not get page %d",
                                recordPageNumber);
      }
      for (unsigned int k = 0; k < geometry->recordsPerPage; k++) {
        const byte *nameBytes = recordPage + (k * BYTES_PER_RECORD);

        UdsChunkName name;
        memcpy(&name.name, nameBytes, UDS_CHUNK_NAME_SIZE);

        result = replayRecord(index, &name, vcn, willBeSparseChapter);
        if (result != UDS_SUCCESS) {
          char hexName[(2 * UDS_CHUNK_NAME_SIZE) + 1];
          if (chunkNameToHex(&name, hexName, sizeof(hexName)) != UDS_SUCCESS) {
            strncpy(hexName, "<unknown>", sizeof(hexName));
          }
          index->volume->lookupMode = oldLookupMode;
          return logUnrecoverable(result,
                                  "could not find block %s during rebuild",
                                  hexName);
        }
      }
    }
  }
  index->volume->lookupMode = oldLookupMode;

  // We also need to reap the chapter being replaced by the open chapter
  setMasterIndexOpenChapter(index->masterIndex, uptoVCN);

  uint64_t newIPMupdate = getLastUpdate(index->volume->indexPageMap);

  if (newIPMupdate != oldIPMupdate) {
    logInfo("replay changed index page map update from %" PRIu64 " to %" PRIu64,
            oldIPMupdate, newIPMupdate);
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
void getIndexStats(Index *index, IndexRouterStatCounters *counters)
{
  uint64_t cwAllocated = getChapterWriterMemoryAllocated(index->chapterWriter);
  // We're accessing the master index while not on a zone thread, but that's
  // safe to do when acquiring statistics.
  MasterIndexStats denseStats, sparseStats;
  getMasterIndexStats(index->masterIndex, &denseStats, &sparseStats);

  memset(counters, 0, sizeof(IndexRouterStatCounters));
  getCacheCounters(index->volume, &counters->volumeCache);

  counters->entriesIndexed   = (denseStats.recordCount
                                + sparseStats.recordCount);
  counters->memoryUsed       = ((uint64_t) denseStats.memoryAllocated
                                + (uint64_t) sparseStats.memoryAllocated
                                + (uint64_t) getCacheSize(index->volume)
                                + cwAllocated);
  counters->diskUsed         = (uint64_t) getVolumeSize(index->volume);
  counters->collisions       = (denseStats.collisionCount
                                + sparseStats.collisionCount);
  counters->entriesDiscarded = (denseStats.discardCount
                                + sparseStats.discardCount);
  counters->checkpoints      = getCheckpointCount(index->checkpoint);
}

/**********************************************************************/
void advanceActiveChapters(Index *index)
{
  index->newestVirtualChapter++;
  if (areSamePhysicalChapter(index->volume->geometry,
                             index->newestVirtualChapter,
                             index->oldestVirtualChapter)) {
    index->oldestVirtualChapter++;
  }
}

/**********************************************************************/
uint64_t triageIndexRequest(Index *index, Request *request)
{
  MasterIndexTriage triage;
  lookupMasterIndexName(index->masterIndex, &request->hash, &triage);
  if (!triage.inSampledChapter) {
    // Not indexed or not a hook.
    return UINT64_MAX;
  }

  IndexZone *zone = getRequestZone(index, request);
  if (!isZoneChapterSparse(zone, triage.virtualChapter)) {
    return UINT64_MAX;
  }

  // XXX Optimize for a common case by remembering the chapter from the most
  // recent barrier message and skipping this chapter if is it the same.

  // Return the sparse chapter number to trigger the barrier messages.
  return triage.virtualChapter;
}
