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
 * $Id: //eng/uds-releases/gloria/src/uds/searchList.c#2 $
 */

#include "searchList.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"

/**********************************************************************/
int makeSearchList(unsigned int   capacity,
                   SearchList   **listPtr)
{
  if (capacity == 0) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "search list must have entries");
  }
  if (capacity > UINT8_MAX) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                  "search list capacity must fit in 8 bits");
  }

  unsigned int bytes = (sizeof(SearchList) + (capacity * sizeof(uint8_t)));
  SearchList *list;
  int result = allocateCacheAligned(bytes, "search list", &list);
  if (result != UDS_SUCCESS) {
    return result;
  }

  list->capacity       = capacity;
  list->firstDeadEntry = 0;

  // Fill in the indexes of the chapter index cache entries. These will be
  // only ever be permuted as the search list is used.
  for (uint8_t i = 0; i < capacity; i++) {
    list->entries[i] = i;
  }

  *listPtr = list;
  return UDS_SUCCESS;
}

/**********************************************************************/
void freeSearchList(SearchList **listPtr)
{
  FREE(*listPtr);
  *listPtr = NULL;
}

/**********************************************************************/
void purgeSearchList(SearchList               *searchList,
                     const CachedChapterIndex  chapters[],
                     uint64_t                  oldestVirtualChapter)
{
  if (searchList->firstDeadEntry == 0) {
    // There are no live entries in the list to purge.
    return;
  }

  // Partition the previously-alive entries in the list into three temporary
  // lists, keeping the current LRU search order within each list.
  uint8_t alive[searchList->firstDeadEntry];
  uint8_t skipped[searchList->firstDeadEntry];
  uint8_t dead[searchList->firstDeadEntry];
  unsigned int nextAlive   = 0;
  unsigned int nextSkipped = 0;
  unsigned int nextDead    = 0;

  for (int i = 0; i < searchList->firstDeadEntry; i++) {
    uint8_t entry = searchList->entries[i];
    const CachedChapterIndex *chapter = &chapters[entry];
    if ((chapter->virtualChapter < oldestVirtualChapter)
        || (chapter->virtualChapter == UINT64_MAX)) {
      dead[nextDead++] = entry;
    } else if (chapter->skipSearch) {
      skipped[nextSkipped++] = entry;
    } else {
      alive[nextAlive++] = entry;
    }
  }

  // Copy the temporary lists back to the search list so we wind up with
  // [ alive, alive, skippable, new-dead, new-dead, old-dead, old-dead ]
  unsigned int size = 0;
  memcpy(&searchList->entries[size], alive, nextAlive);
  size += nextAlive;

  memcpy(&searchList->entries[size], skipped, nextSkipped);
  size += nextSkipped;

  memcpy(&searchList->entries[size], dead, nextDead);
  // The first dead entry is now the start of the copied dead list.
  searchList->firstDeadEntry = size;
}
