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
 * $Id: //eng/uds-releases/gloria/src/uds/indexPageMap.c#1 $
 */

#include "indexPageMap.h"

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

static const byte INDEX_PAGE_MAP_MAGIC[] = "ALBIPM";
static const char INDEX_PAGE_MAP_VERSION_FORMAT[] = "%02u";
enum {
  INDEX_PAGE_MAP_MAGIC_LENGTH = sizeof(INDEX_PAGE_MAP_MAGIC) - 1,
  INDEX_PAGE_MAP_VERSION_LENGTH = 2,
  INDEX_PAGE_MAP_HEADER_LENGTH =
    INDEX_PAGE_MAP_MAGIC_LENGTH + INDEX_PAGE_MAP_VERSION_LENGTH
};
static const byte INDEX_PAGE_MAP_V1 = 1;
static const byte INDEX_PAGE_MAP_V2 = 2;

/*
 * Version 1 of the IPM has no magic or header at all, and did not store
 * last update field.
 *
 * Version 2 includes the magic and version, followed by the last update field,
 * followed by the array contents
 */

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
  char buf[INDEX_PAGE_MAP_HEADER_LENGTH + sizeof(map->lastUpdate)];
  memcpy(buf, INDEX_PAGE_MAP_MAGIC, INDEX_PAGE_MAP_MAGIC_LENGTH);
  result = fixedSprintf(__func__,
                        buf + INDEX_PAGE_MAP_MAGIC_LENGTH,
                        INDEX_PAGE_MAP_VERSION_LENGTH + 1,
                        UDS_INVALID_ARGUMENT,
                        INDEX_PAGE_MAP_VERSION_FORMAT, INDEX_PAGE_MAP_V2);
  if (result != UDS_SUCCESS) {
    return result;
  }

  memcpy(buf + INDEX_PAGE_MAP_HEADER_LENGTH,
         &map->lastUpdate, sizeof(map->lastUpdate));

  result = writeToBufferedWriter(writer, buf, sizeof(buf));
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot write index page map header");
  }
  result = writeToBufferedWriter(writer, map->entries,
                                 indexPageMapSize(map->geometry));
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
    INDEX_PAGE_MAP_HEADER_LENGTH + sizeof(((IndexPageMap *) 0)->lastUpdate);
}

/**
 * Extract the header information for the Index Page Map, verifying
 * the version as required.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  geometry        The index geometry.
 * @param [out] version         The version of the index page map.
 * @param [out] readerPtr       The buffered reader used to read the version.
 *
 * @return UDS_SUCCESS or an error code, particularly UDS_CORRUPT_COMPONENT.
 **/
static int retrieveFileVersion(ReadPortal      *portal,
                               const Geometry  *geometry,
                               byte            *version,
                               BufferedReader **readerPtr)
{
  off_t size = 0;
  int result = getComponentSizeForPortal(portal, 0, &size);
  if (result != UDS_SUCCESS) {
    logError("could not determine index page map info size");
    return result;
  }
  BufferedReader *reader = NULL;
  result = getBufferedReaderForPortal(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t ipmSize = indexPageMapSize(geometry);
  if ((off_t) ipmSize == size) {
    if (version != NULL) {
      *version = INDEX_PAGE_MAP_V1;
    }
    if (readerPtr != NULL) {
      *readerPtr = reader;
    }
    return UDS_SUCCESS;
  }

  result = verifyBufferedData(reader, INDEX_PAGE_MAP_MAGIC,
                              INDEX_PAGE_MAP_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "bad index page map saved magic");
  }
  char buf[INDEX_PAGE_MAP_VERSION_LENGTH + 1];
  result = readFromBufferedReader(reader, buf, INDEX_PAGE_MAP_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot read index page saved version");
  }
  buf[INDEX_PAGE_MAP_VERSION_LENGTH] = '\0';
  unsigned long vers;
  result = stringToUnsignedLong(buf, &vers);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(UDS_CORRUPT_COMPONENT,
                                   "invalid index page save version %s", buf);
  }
  if (vers != INDEX_PAGE_MAP_V2) {
    return logErrorWithStringError(UDS_CORRUPT_COMPONENT,
                                   "unexpected index page save version %lu",
                                   vers);
  }
  if (version != NULL) {
    *version = INDEX_PAGE_MAP_V2;
  }
  if (readerPtr != NULL) {
    *readerPtr = reader;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int readIndexPageMap(ReadPortal *portal)
{
  IndexPageMap *map = componentDataForPortal(portal);

  byte version = 0;
  BufferedReader *reader = NULL;
  int result = retrieveFileVersion(portal, map->geometry, &version, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (version == INDEX_PAGE_MAP_V1) {
    map->lastUpdate = 0;
  } else {
    result = readFromBufferedReader(reader, &map->lastUpdate,
                                    sizeof(map->lastUpdate));
    if (result != UDS_SUCCESS) {
      return result;
    }
    logInfo("read index page map, last update %" PRIu64, map->lastUpdate);
  }
  result = readFromBufferedReader(reader, map->entries,
                                  indexPageMapSize(map->geometry));
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot read index page map data");
  }
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
