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
 * $Id: //eng/uds-releases/gloria/src/uds/indexPageMap.c#3 $
 */

#include "indexPageMap.h"

#include "buffer.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "indexComponent.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"
#include "threads.h"
#include "uds.h"

static int readIndexPageMap(ReadPortal *portal);
static int writeIndexPageMap(IndexComponent *component,
                             BufferedWriter *writer,
                             unsigned int    zone);

static const byte INDEX_PAGE_MAP_MAGIC[] = "ALBIPM02";
enum {
  INDEX_PAGE_MAP_MAGIC_LENGTH = sizeof(INDEX_PAGE_MAP_MAGIC) - 1,
};

const IndexComponentInfo INDEX_PAGE_MAP_INFO = {
  .kind        = RL_KIND_INDEX_PAGE_MAP,
  .name        = "index page map",
  .fileName    = "page_map",
  .saveOnly    = false,
  .chapterSync = true,
  .multiZone   = false,
  .loader      = readIndexPageMap,
  .saver       = writeIndexPageMap,
  .incremental = NULL,
};

/*****************************************************************************/
static INLINE size_t numEntries(const Geometry *geometry)
{
  return geometry->chaptersPerVolume * (geometry->indexPagesPerChapter - 1);
}

/*****************************************************************************/
int makeIndexPageMap(const Geometry *geometry, IndexPageMap **mapPtr)
{
  unsigned int deltaListsPerChapter = geometry->deltaListsPerChapter;
  int result
    = ASSERT_WITH_ERROR_CODE(((deltaListsPerChapter - 1) <= UINT16_MAX),
                             UDS_BAD_STATE,
                             "delta lists per chapter (%u) is too large",
                             deltaListsPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexPageMap *map;
  result = ALLOCATE(1, IndexPageMap, "Index Page Map", &map);
  if (result != UDS_SUCCESS) {
    return result;
  }

  map->geometry = geometry;

  result = ALLOCATE(numEntries(geometry),
                    IndexPageMapEntry,
                    "Index Page Map Entries",
                    &map->entries);
  if (result != UDS_SUCCESS) {
    freeIndexPageMap(map);
    return result;
  }

  *mapPtr = map;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeIndexPageMap(IndexPageMap *map)
{
  if (map != NULL) {
    FREE(map->entries);
    FREE(map);
  }
}

/*****************************************************************************/
uint64_t getLastUpdate(const IndexPageMap *map)
{
  return map->lastUpdate;
}

/*****************************************************************************/
int updateIndexPageMap(IndexPageMap   *map,
                       uint64_t        virtualChapterNumber,
                       unsigned int    chapterNumber,
                       unsigned int    indexPageNumber,
                       unsigned int    deltaListNumber)
{
  const Geometry *geometry = map->geometry;
  if ((virtualChapterNumber < map->lastUpdate)
      || (virtualChapterNumber > map->lastUpdate + 1)) {
    logWarning("unexpected index page map update, jumping from %" PRIu64
               " to %" PRIu64,
               map->lastUpdate, virtualChapterNumber);
  }
  map->lastUpdate = virtualChapterNumber;

  if (chapterNumber >= geometry->chaptersPerVolume) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "chapter number %u exceeds maximum %u",
      chapterNumber, geometry->chaptersPerVolume - 1);
  }
  if (indexPageNumber >= geometry->indexPagesPerChapter) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "index page number %u exceeds maximum %u",
      indexPageNumber, geometry->indexPagesPerChapter - 1);
  }
  if (deltaListNumber >= geometry->deltaListsPerChapter) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "delta list number %u exceeds maximum %u",
      deltaListNumber, geometry->deltaListsPerChapter - 1);
  }

  if (indexPageNumber == (geometry->indexPagesPerChapter - 1)) {
    /*
     * There is no entry for the last index page of a chapter since its entry
     * would always be geometry->deltaListsPerChapter - 1.
     */
    return UDS_SUCCESS;
  }

  size_t slot
    = (chapterNumber * (geometry->indexPagesPerChapter - 1)) + indexPageNumber;
  map->entries[slot] = (IndexPageMapEntry) deltaListNumber;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int findIndexPageNumber(const IndexPageMap *map,
                        const UdsChunkName *name,
                        unsigned int        chapterNumber,
                        unsigned int       *indexPageNumberPtr)
{
  const Geometry *geometry = map->geometry;
  if (chapterNumber >= geometry->chaptersPerVolume) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "chapter number %u exceeds maximum %u",
      chapterNumber, geometry->chaptersPerVolume - 1);
  }

  unsigned int deltaListNumber = hashToChapterDeltaList(name, geometry);
  unsigned int slot = (chapterNumber * (geometry->indexPagesPerChapter - 1));
  unsigned int limit = slot + (geometry->indexPagesPerChapter - 1);
  unsigned int indexPageNumber = 0;
  for (; slot < limit; indexPageNumber++, slot++) {
    if (deltaListNumber <= map->entries[slot]) {
      break;
    }
  }

  // This should be a clear post-condition of the loop above, but just in case
  // it's not obvious, the check is cheap.
  int result = ASSERT((indexPageNumber < geometry->indexPagesPerChapter),
                      "index page number too large");
  if (result != UDS_SUCCESS) {
    return result;
  }

  *indexPageNumberPtr = indexPageNumber;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getListNumberBounds(const IndexPageMap *map,
                        unsigned int        chapterNumber,
                        unsigned int        indexPageNumber,
                        IndexPageBounds    *bounds)
{
  const Geometry *geometry = map->geometry;
  int result = ASSERT((chapterNumber < geometry->chaptersPerVolume),
                      "chapter number is valid");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((indexPageNumber < geometry->indexPagesPerChapter),
                  "index page number is valid");
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int slot = chapterNumber * (geometry->indexPagesPerChapter - 1);
  bounds->lowestList = ((indexPageNumber == 0)
                        ? 0
                        : map->entries[slot + indexPageNumber - 1] + 1);
  bounds->highestList = ((indexPageNumber == geometry->indexPagesPerChapter - 1)
                         ? geometry->deltaListsPerChapter - 1
                         : map->entries[slot + indexPageNumber]);

  return UDS_SUCCESS;
}

/*****************************************************************************/
size_t indexPageMapSize(const Geometry *geometry)
{
  return sizeof(IndexPageMapEntry) * numEntries(geometry);
}

/*****************************************************************************/
static int writeIndexPageMap(IndexComponent *component,
                             BufferedWriter *writer,
                             unsigned int    zone)
{
  int result = ASSERT((zone == 0), "unimplemented zone %d", zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  IndexPageMap *map = indexComponentData(component);

  Buffer *buffer;
  result = makeBuffer(INDEX_PAGE_MAP_MAGIC_LENGTH + sizeof(map->lastUpdate),
                      &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putBytes(buffer, INDEX_PAGE_MAP_MAGIC_LENGTH, INDEX_PAGE_MAP_MAGIC);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, map->lastUpdate);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(writer, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot write index page map header");
  }
  result = makeBuffer(indexPageMapSize(map->geometry), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result
    = putUInt16LEsIntoBuffer(buffer, numEntries(map->geometry), map->entries);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(writer, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot write index page map data");
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
uint64_t computeIndexPageMapSaveSize(const Geometry *geometry)
{
  return indexPageMapSize(geometry) +
    INDEX_PAGE_MAP_MAGIC_LENGTH + sizeof(((IndexPageMap *) 0)->lastUpdate);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeIndexPageMap(Buffer *buffer, IndexPageMap *map)
{
  int result = getUInt64LEFromBuffer(buffer, &map->lastUpdate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt16LEsFromBuffer(buffer, numEntries(map->geometry),
                                  map->entries);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           bufferLength(buffer) - contentLength(buffer),
                           bufferLength(buffer));
  return result;
}

/*****************************************************************************/
static int readIndexPageMap(ReadPortal *portal)
{
  IndexPageMap *map = componentDataForPortal(portal);

  BufferedReader *reader = NULL;

  int result = getBufferedReaderForPortal(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = verifyBufferedData(reader, INDEX_PAGE_MAP_MAGIC,
                              INDEX_PAGE_MAP_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "bad index page map saved magic");
  }

  Buffer *buffer;
  result
    = makeBuffer(sizeof(map->lastUpdate) + indexPageMapSize(map->geometry),
                 &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = readFromBufferedReader(reader, getBufferContents(buffer),
                                  bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    logErrorWithStringError(result, "cannot read index page map data");
    return result;
  }
  result = resetBufferEnd(buffer, bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = decodeIndexPageMap(buffer, map);
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  logDebug("read index page map, last update %" PRIu64, map->lastUpdate);
  return UDS_SUCCESS;
}

/*****************************************************************************/
int dumpIndexPageMap(const IndexPageMap *map, unsigned int chapterNumber)
{
  const Geometry *geometry = map->geometry;
  if (chapterNumber >= geometry->chaptersPerVolume) {
    return logErrorWithStringError(
      UDS_INVALID_ARGUMENT, "chapter number %u exceeds maximum %u",
      chapterNumber, geometry->chaptersPerVolume - 1);
  }

  unsigned int slot = (chapterNumber * (geometry->indexPagesPerChapter - 1));
  unsigned int limit = slot + (geometry->indexPagesPerChapter - 1);
  unsigned int indexPageNumber = 0;
  unsigned int deltaList = 0;
  logInfo("index page map last update %" PRIu64, map->lastUpdate);
  for (; slot < limit; indexPageNumber++, slot++) {
    logInfo("chapter %u index page %u lists %u-%u",
            chapterNumber, slot, deltaList, map->entries[slot]);
    deltaList = map->entries[slot] + 1;
  }
  logInfo("chapter %u index page %u lists %u-%u",
          chapterNumber, slot, deltaList, geometry->deltaListsPerChapter);
  return UDS_SUCCESS;
}
