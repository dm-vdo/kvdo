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
 * $Id: //eng/uds-releases/krusty/src/uds/indexPageMap.c#14 $
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

static int readIndexPageMap(struct read_portal *portal);
static int writeIndexPageMap(struct index_component *component,
                             struct buffered_writer *writer,
                             unsigned int    zone);

static const byte INDEX_PAGE_MAP_MAGIC[] = "ALBIPM02";
enum {
  INDEX_PAGE_MAP_MAGIC_LENGTH = sizeof(INDEX_PAGE_MAP_MAGIC) - 1,
};

const struct index_component_info INDEX_PAGE_MAP_INFO = {
  .kind        = RL_KIND_INDEX_PAGE_MAP,
  .name        = "index page map",
  .saveOnly    = false,
  .chapterSync = true,
  .multiZone   = false,
  .ioStorage   = true,
  .loader      = readIndexPageMap,
  .saver       = writeIndexPageMap,
  .incremental = NULL,
};

/*****************************************************************************/
static INLINE size_t numEntries(const struct geometry *geometry)
{
  return geometry->chapters_per_volume
           * (geometry->index_pages_per_chapter - 1);
}

/*****************************************************************************/
int makeIndexPageMap(const struct geometry  *geometry,
                     struct index_page_map **mapPtr)
{
  unsigned int deltaListsPerChapter = geometry->delta_lists_per_chapter;
  int result
    = ASSERT_WITH_ERROR_CODE(((deltaListsPerChapter - 1) <= UINT16_MAX),
                             UDS_BAD_STATE,
                             "delta lists per chapter (%u) is too large",
                             deltaListsPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  struct index_page_map *map;
  result = ALLOCATE(1, struct index_page_map, "Index Page Map", &map);
  if (result != UDS_SUCCESS) {
    return result;
  }

  map->geometry = geometry;

  result = ALLOCATE(numEntries(geometry),
                    index_page_map_entry_t,
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
void freeIndexPageMap(struct index_page_map *map)
{
  if (map != NULL) {
    FREE(map->entries);
    FREE(map);
  }
}

/*****************************************************************************/
uint64_t getLastUpdate(const struct index_page_map *map)
{
  return map->lastUpdate;
}

/*****************************************************************************/
int updateIndexPageMap(struct index_page_map *map,
                       uint64_t               virtualChapterNumber,
                       unsigned int           chapterNumber,
                       unsigned int           indexPageNumber,
                       unsigned int           deltaListNumber)
{
  const struct geometry *geometry = map->geometry;
  if ((virtualChapterNumber < map->lastUpdate)
      || (virtualChapterNumber > map->lastUpdate + 1)) {
    // if the lastUpdate is 0, this is likely to be normal because we are
    // replaying the volume
    if (map->lastUpdate != 0) {
      logWarning("unexpected index page map update, jumping from %" PRIu64
                 " to %llu",
                 map->lastUpdate, virtualChapterNumber);
    }
  }
  map->lastUpdate = virtualChapterNumber;

  if (chapterNumber >= geometry->chapters_per_volume) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
    				   "chapter number %u exceeds maximum %u",
    				   chapterNumber,
    				   geometry->chapters_per_volume - 1);
  }
  if (indexPageNumber >= geometry->index_pages_per_chapter) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
    				   "index page number %u exceeds maximum %u",
      				   indexPageNumber,
      				   geometry->index_pages_per_chapter - 1);
  }
  if (deltaListNumber >= geometry->delta_lists_per_chapter) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
    				   "delta list number %u exceeds maximum %u",
    				   deltaListNumber,
    				   geometry->delta_lists_per_chapter - 1);
  }

  if (indexPageNumber == (geometry->index_pages_per_chapter - 1)) {
    /*
     * There is no entry for the last index page of a chapter since its entry
     * would always be geometry->delta_lists_per_chapter - 1.
     */
    return UDS_SUCCESS;
  }

  size_t slot
    = (chapterNumber * (geometry->index_pages_per_chapter - 1))
        + indexPageNumber;
  map->entries[slot] = (index_page_map_entry_t) deltaListNumber;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int findIndexPageNumber(const struct index_page_map *map,
                        const struct uds_chunk_name *name,
                        unsigned int                 chapterNumber,
                        unsigned int                *indexPageNumberPtr)
{
  const struct geometry *geometry = map->geometry;
  if (chapterNumber >= geometry->chapters_per_volume) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
    				   "chapter number %u exceeds maximum %u",
    				   chapterNumber,
    				   geometry->chapters_per_volume - 1);
  }

  unsigned int deltaListNumber = hash_to_chapter_delta_list(name, geometry);
  unsigned int slot
    = (chapterNumber * (geometry->index_pages_per_chapter - 1));
  unsigned int limit = slot + (geometry->index_pages_per_chapter - 1);
  unsigned int indexPageNumber = 0;
  for (; slot < limit; indexPageNumber++, slot++) {
    if (deltaListNumber <= map->entries[slot]) {
      break;
    }
  }

  // This should be a clear post-condition of the loop above, but just in case
  // it's not obvious, the check is cheap.
  int result = ASSERT((indexPageNumber < geometry->index_pages_per_chapter),
                      "index page number too large");
  if (result != UDS_SUCCESS) {
    return result;
  }

  *indexPageNumberPtr = indexPageNumber;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getListNumberBounds(const struct index_page_map *map,
                        unsigned int                 chapterNumber,
                        unsigned int                 indexPageNumber,
                        struct index_page_bounds    *bounds)
{
  const struct geometry *geometry = map->geometry;
  int result = ASSERT((chapterNumber < geometry->chapters_per_volume),
                      "chapter number is valid");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT((indexPageNumber < geometry->index_pages_per_chapter),
                  "index page number is valid");
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int slot = chapterNumber * (geometry->index_pages_per_chapter - 1);
  bounds->lowestList = ((indexPageNumber == 0)
                        ? 0
                        : map->entries[slot + indexPageNumber - 1] + 1);
  bounds->highestList = ((indexPageNumber
                            == geometry->index_pages_per_chapter - 1)
                         ? geometry->delta_lists_per_chapter - 1
                         : map->entries[slot + indexPageNumber]);

  return UDS_SUCCESS;
}

/*****************************************************************************/
size_t indexPageMapSize(const struct geometry *geometry)
{
  return sizeof(index_page_map_entry_t) * numEntries(geometry);
}

/*****************************************************************************/
static int writeIndexPageMap(struct index_component *component,
                             struct buffered_writer *writer,
                             unsigned int            zone)
{
  int result = ASSERT((zone == 0), "unimplemented zone %d", zone);
  if (result != UDS_SUCCESS) {
    return result;
  }

  struct index_page_map *map = indexComponentData(component);

  struct buffer *buffer;
  result = make_buffer(INDEX_PAGE_MAP_MAGIC_LENGTH + sizeof(map->lastUpdate),
                       &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_bytes(buffer, INDEX_PAGE_MAP_MAGIC_LENGTH, INDEX_PAGE_MAP_MAGIC);
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = put_uint64_le_into_buffer(buffer, map->lastUpdate);
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
                                    content_length(buffer));
  free_buffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot write index page map header");
  }
  result = make_buffer(indexPageMapSize(map->geometry), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result
    = put_uint16_les_into_buffer(buffer, numEntries(map->geometry), map->entries);
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
                                    content_length(buffer));
  free_buffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot write index page map data");
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
uint64_t computeIndexPageMapSaveSize(const struct geometry *geometry)
{
  return indexPageMapSize(geometry) +
    INDEX_PAGE_MAP_MAGIC_LENGTH +
    sizeof(((struct index_page_map *) 0)->lastUpdate);
}

/**********************************************************************/
static int __must_check
decodeIndexPageMap(struct buffer *buffer, struct index_page_map *map)
{
  int result = get_uint64_le_from_buffer(buffer, &map->lastUpdate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint16_les_from_buffer(buffer, numEntries(map->geometry),
                                      map->entries);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           buffer_length(buffer) - content_length(buffer),
                           buffer_length(buffer));
  return result;
}

/*****************************************************************************/
static int readIndexPageMap(struct read_portal *portal)
{
  struct index_page_map *map = indexComponentData(portal->component);

  struct buffered_reader *reader = NULL;

  int result = getBufferedReaderForPortal(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = verify_buffered_data(reader, INDEX_PAGE_MAP_MAGIC,
                                INDEX_PAGE_MAP_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "bad index page map saved magic");
  }

  struct buffer *buffer;
  result
    = make_buffer(sizeof(map->lastUpdate) + indexPageMapSize(map->geometry),
                  &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = read_from_buffered_reader(reader, get_buffer_contents(buffer),
                                     buffer_length(buffer));
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    logErrorWithStringError(result, "cannot read index page map data");
    return result;
  }
  result = reset_buffer_end(buffer, buffer_length(buffer));
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = decodeIndexPageMap(buffer, map);
  free_buffer(&buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  logDebug("read index page map, last update %llu", map->lastUpdate);
  return UDS_SUCCESS;
}
