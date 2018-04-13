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
 * $Id: //eng/uds-releases/gloria/src/uds/chapterIndex.c#1 $
 */

#include "chapterIndex.h"

#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"


/**********************************************************************/
int makeOpenChapterIndex(OpenChapterIndex **openChapterIndex,
                         const Geometry    *geometry)
{

  int result = ALLOCATE(1, OpenChapterIndex, "open chapter index",
                        openChapterIndex);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // The delta index will rebalance delta lists when memory gets tight, so
  // give the chapter index one extra page.
  size_t memorySize
    = (geometry->indexPagesPerChapter + 1) * geometry->bytesPerPage;
  (*openChapterIndex)->geometry = geometry;
  result = initializeDeltaIndex(&(*openChapterIndex)->deltaIndex, 1,
                                geometry->deltaListsPerChapter,
                                geometry->chapterMeanDelta,
                                geometry->chapterPayloadBits, memorySize);
  if (result != UDS_SUCCESS) {
    FREE(*openChapterIndex);
    *openChapterIndex = NULL;
  }
  return result;
}

/**********************************************************************/
void freeOpenChapterIndex(OpenChapterIndex *openChapterIndex)
{
  if (openChapterIndex == NULL) {
    return;
  }


  uninitializeDeltaIndex(&openChapterIndex->deltaIndex);
  FREE(openChapterIndex);
}

/**********************************************************************/
void emptyOpenChapterIndex(OpenChapterIndex *openChapterIndex,
                           uint64_t          virtualChapterNumber)
{
  emptyDeltaIndex(&openChapterIndex->deltaIndex);
  openChapterIndex->virtualChapterNumber = virtualChapterNumber;
}

/**
 * Check whether a delta list entry reflects a successful search for a given
 * address.
 *
 * @param entry    the delta list entry from the search
 * @param address  the address of the desired entry
 *
 * @return <code>true</code> iff the address was found
 **/
static INLINE bool wasEntryFound(const DeltaIndexEntry *entry,
                                 unsigned int           address)
{
  return (!entry->atEnd && (entry->key == address));
}

/**********************************************************************/
int putOpenChapterIndexRecord(OpenChapterIndex   *openChapterIndex,
                              const UdsChunkName *name,
                              unsigned int        pageNumber)
{
  const Geometry *geometry = openChapterIndex->geometry;
  int result
    = ASSERT_WITH_ERROR_CODE(pageNumber < geometry->recordPagesPerChapter,
                             UDS_INVALID_ARGUMENT,
                             "Page number within chapter (%u) exceeds"
                             " the maximum value %u",
                             pageNumber, geometry->recordPagesPerChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  DeltaIndexEntry entry;
  unsigned int address = hashToChapterDeltaAddress(name, geometry);
  result = getDeltaIndexEntry(&openChapterIndex->deltaIndex,
                              hashToChapterDeltaList(name, geometry),
                              address, name->name, false, &entry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  bool found = wasEntryFound(&entry, address);
  result = ASSERT_WITH_ERROR_CODE(!(found && entry.isCollision),
                                  UDS_BAD_STATE,
                                  "Chunk appears more than once in chapter %"
                                  PRIu64,
                                  openChapterIndex->virtualChapterNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return putDeltaIndexEntry(&entry, address, pageNumber,
                            (found ? name->name : NULL));
}

/**********************************************************************/
int packOpenChapterIndexPage(OpenChapterIndex *openChapterIndex,
                             uint64_t          volumeNonce,
                             byte             *memory,
                             unsigned int      firstList,
                             bool              lastPage,
                             unsigned int     *numLists)
{
  DeltaIndex *deltaIndex = &openChapterIndex->deltaIndex;
  const Geometry *geometry = openChapterIndex->geometry;
  unsigned int removals = 0;
  for (;;) {
    int result = packDeltaIndexPage(deltaIndex, volumeNonce, memory,
                                    geometry->bytesPerPage,
                                    openChapterIndex->virtualChapterNumber,
                                    firstList, numLists);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if ((firstList + *numLists) == geometry->deltaListsPerChapter) {
      // All lists are packed
      break;
    } else if (*numLists == 0) {
      // The next delta list does not fit on a page.  This delta list will
      // be removed.
    } else if (lastPage) {
      /*
       * This is the last page and there are lists left unpacked, but all of
       * the remaining lists must fit on the page. Find a list that contains
       * entries and remove the entire list. Try the first list that does not
       * fit. If it is empty, we will select the last list that already fits
       * and has any entries.
       */
    } else {
      // This page is done
      break;
    }
    if (removals == 0) {
      DeltaIndexStats stats;
      getDeltaIndexStats(deltaIndex, &stats);
      logWarning("The chapter index for chapter %" PRIu64
                 " contains %ld entries with %ld collisions",
                 openChapterIndex->virtualChapterNumber,
                 stats.recordCount, stats.collisionCount);
    }
    DeltaIndexEntry entry;
    int listNumber = *numLists;
    do {
      if (listNumber < 0) {
        return UDS_OVERFLOW;
      }
      result = startDeltaIndexSearch(deltaIndex, firstList + listNumber--,
                                     0, false, &entry);
      if (result != UDS_SUCCESS) {
        return result;
      }
      result = nextDeltaIndexEntry(&entry);
      if (result != UDS_SUCCESS) {
        return result;
      }
    } while (entry.atEnd);
    do {
      result = removeDeltaIndexEntry(&entry);
      if (result != UDS_SUCCESS) {
        return result;
      }
      removals++;
    } while (!entry.atEnd);
  }
  if (removals > 0) {
    logWarning("To avoid chapter index page overflow in chapter %" PRIu64
               ", %u entries were removed from the chapter index",
               openChapterIndex->virtualChapterNumber, removals);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int getOpenChapterIndexSize(OpenChapterIndex *openChapterIndex)
{
  DeltaIndexStats stats;
  getDeltaIndexStats(&openChapterIndex->deltaIndex, &stats);
  return stats.recordCount;
}

/**********************************************************************/
size_t getOpenChapterIndexMemoryAllocated(OpenChapterIndex *openChapterIndex)
{
  DeltaIndexStats stats;
  getDeltaIndexStats(&openChapterIndex->deltaIndex, &stats);
  return stats.memoryAllocated + sizeof(OpenChapterIndex);
}

/**********************************************************************/
int initializeChapterIndexPage(ChapterIndexPage *chapterIndexPage,
                               const Geometry   *geometry,
                               byte             *indexPage,
                               uint64_t          volumeNonce)
{
  return initializeDeltaIndexPage(&chapterIndexPage->deltaIndex,
                                  &chapterIndexPage->deltaMemory,
                                  volumeNonce,
                                  geometry->chapterMeanDelta,
                                  geometry->chapterPayloadBits,
                                  indexPage, geometry->bytesPerPage);
}

/**********************************************************************/
int validateChapterIndexPage(const ChapterIndexPage *chapterIndexPage,
                             const Geometry         *geometry)
{
  const DeltaIndex *deltaIndex = &chapterIndexPage->deltaIndex;
  unsigned int first = getDeltaIndexLowestListNumber(deltaIndex);
  unsigned int last = getDeltaIndexHighestListNumber(deltaIndex);
  // We walk every delta list from start to finish.
  for (unsigned int listNumber = first; listNumber <= last; listNumber++) {
    DeltaIndexEntry entry;
    int result = startDeltaIndexSearch(deltaIndex, listNumber - first, 0, true,
                                       &entry);
    if (result != UDS_SUCCESS) {
      return result;
    }
    for (;;) {
      result = nextDeltaIndexEntry(&entry);
      if (result != UDS_SUCCESS) {
        if (result == UDS_CORRUPT_DATA) {
          // A random bit stream is highly likely to arrive here when we go
          // past the end of the delta list
          return UDS_CORRUPT_COMPONENT;
        }
        return result;
      }
      if (entry.atEnd) {
        break;
      }
      // Also make sure that the record page field contains a plausible value
      if (getDeltaEntryValue(&entry) >= geometry->recordPagesPerChapter) {
        // Do not log this as an error.  It happens in normal operation when
        // we are doing a rebuild but haven't written the entire volume once.
        return UDS_CORRUPT_COMPONENT;
      }
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int searchChapterIndexPage(ChapterIndexPage   *chapterIndexPage,
                           const Geometry     *geometry,
                           const UdsChunkName *name,
                           int                *recordPagePtr)
{
  DeltaIndex *deltaIndex = &chapterIndexPage->deltaIndex;
  unsigned int address = hashToChapterDeltaAddress(name, geometry);
  unsigned int deltaListNumber = hashToChapterDeltaList(name, geometry);
  unsigned int subListNumber
    = deltaListNumber - getDeltaIndexLowestListNumber(deltaIndex);
  DeltaIndexEntry entry;
  int result = getDeltaIndexEntry(deltaIndex, subListNumber, address,
                                  name->name, true, &entry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (wasEntryFound(&entry, address)) {
    *recordPagePtr = getDeltaEntryValue(&entry);
  } else {
    *recordPagePtr = NO_CHAPTER_INDEX_ENTRY;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
uint64_t getChapterIndexVirtualChapterNumber(const ChapterIndexPage *page)
{
  return getDeltaIndexVirtualChapterNumber(&page->deltaIndex);
}

/**********************************************************************/
unsigned int getChapterIndexLowestListNumber(const ChapterIndexPage *page)
{
  return getDeltaIndexLowestListNumber(&page->deltaIndex);
}

/**********************************************************************/
unsigned int getChapterIndexHighestListNumber(const ChapterIndexPage *page)
{
  return getDeltaIndexHighestListNumber(&page->deltaIndex);
}
