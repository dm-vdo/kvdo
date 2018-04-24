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
 * $Id: //eng/uds-releases/gloria/src/uds/cachedChapterIndex.c#2 $
 */

#include "cachedChapterIndex.h"

#include "memoryAlloc.h"

/**********************************************************************/
int initializeCachedChapterIndex(CachedChapterIndex *chapter,
                                 const Geometry     *geometry)
{
  chapter->virtualChapter  = UINT64_MAX;

  unsigned int pages = geometry->indexPagesPerChapter;
  int result = ALLOCATE(pages, ChapterIndexPage, "sparse ChapterIndexPages",
                        &chapter->indexPages);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return ALLOCATE_IO_ALIGNED(pages * geometry->bytesPerPage, byte,
                             "sparse index page data", &chapter->pageData);
}

/**********************************************************************/
void destroyCachedChapterIndex(CachedChapterIndex *chapter)
{
  FREE(chapter->indexPages);
  FREE(chapter->pageData);
}

/**********************************************************************/
int cacheChapterIndex(CachedChapterIndex *chapter,
                      uint64_t            virtualChapter,
                      const Volume       *volume)
{
  // Mark the cached chapter as unused in case the update fails midway.
  chapter->virtualChapter = UINT64_MAX;

  // Read all the page data and initialize the entire ChapterIndexPage array.
  // (It's not safe for the zone threads to do it lazily--they'll race.)
  int result = readChapterIndexFromVolume(volume, virtualChapter,
                                          chapter->pageData,
                                          chapter->indexPages);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Reset all chapter counter values to zero.
  chapter->counters.searchHits        = 0;
  chapter->counters.searchMisses      = 0;
  chapter->counters.consecutiveMisses = 0;

  // Mark the entry as valid--it's now in the cache.
  chapter->virtualChapter = virtualChapter;
  chapter->skipSearch     = false;

  return UDS_SUCCESS;
}

/**********************************************************************/
int searchCachedChapterIndex(CachedChapterIndex *chapter,
                             const Geometry     *geometry,
                             const IndexPageMap *indexPageMap,
                             const UdsChunkName *name,
                             int                *recordPagePtr)
{
  // Find the indexPageNumber in the chapter that would have the chunk name.
  unsigned int physicalChapter
    = mapToPhysicalChapter(geometry, chapter->virtualChapter);
  unsigned int indexPageNumber;
  int result = findIndexPageNumber(indexPageMap, name, physicalChapter,
                                   &indexPageNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return searchChapterIndexPage(&chapter->indexPages[indexPageNumber],
                                geometry, name, recordPagePtr);
}
