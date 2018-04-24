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
 * $Id: //eng/uds-releases/gloria/src/uds/searchList.h#2 $
 */

#ifndef SEARCH_LIST_H
#define SEARCH_LIST_H

#include "cachedChapterIndex.h"
#include "compiler.h"
#include "stringUtils.h"
#include "typeDefs.h"

/**
 * A SearchList represents the permutations of the sparse chapter index cache
 * entry array. Those permutations express an ordering on the chapter indexes,
 * from most recently accessed to least recently accessed, which is the order
 * in which the indexes should be searched and the reverse order in which they
 * should be evicted from the cache (LRU cache replacement policy).
 *
 * Cache entries that are dead (virtualChapter == UINT64_MAX) are kept as a
 * suffix of the list, avoiding the need to even iterate over them to search,
 * and ensuring that dead entries are replaced before any live entries are
 * evicted.
 *
 * The search list is intended to be instantated for each zone thread,
 * avoiding any need for synchronization. The structure is allocated on a
 * cache boundary to avoid false sharing of memory cache lines between zone
 * threads.
 **/
typedef struct searchList {
  /** The number of cached chapter indexes and search list entries */
  uint8_t capacity;

  /** The index in the entries array of the first dead cache entry */
  uint8_t firstDeadEntry;

  /** The chapter array indexes representing the chapter search order */
  uint8_t entries[];
} SearchList;

/**
 * SearchListIterator captures the fields needed to iterate over the live
 * entries in a search list and return the CachedChapterIndex pointers that
 * the search code actually wants to deal with.
 **/
typedef struct {
  /** The search list defining the chapter search iteration order */
  SearchList         *list;

  /** The index of the next entry to return from the search list */
  unsigned int        nextEntry;

  /** The cached chapters that are referenced by the search list */
  CachedChapterIndex *chapters;
} SearchListIterator;

/**
 * Allocate and initialize a new chapter cache search list with the same
 * capacity as the cache. The index of each entry in the cache will appear
 * exactly once in the array. All the chapters in the cache are assumed to be
 * initially dead, so firstDeadEntry will be zero and no chapters will be
 * returned when the search list is iterated.
 *
 * @param [in]  capacity  the number of entries in the search list
 * @param [out] listPtr   a pointer in which to return the new search list
 **/
int makeSearchList(unsigned int   capacity,
                   SearchList   **listPtr)
  __attribute__((warn_unused_result));

/**
 * Free a search list and null out the reference to it.
 *
 * @param listPtr the reference to the search list to free
 **/
void freeSearchList(SearchList **listPtr);

/**
 * Copy the contents of one search list to another.
 *
 * @param source  the list to copy
 * @param target  the list to replace
 **/
static INLINE void copySearchList(const SearchList *source,
                                  SearchList       *target)
{
  *target = *source;
  memcpy(target->entries, source->entries, source->capacity);
}

/**
 * Prepare to iterate over the live cache entries a search list.
 *
 * @param list      the list defining the live chapters and the search order
 * @param chapters  the chapter index entries to return from getNextChapter()
 *
 * @return an iterator positioned at the start of the search list
 **/
static INLINE SearchListIterator
iterateSearchList(SearchList *list, CachedChapterIndex chapters[])
{
  SearchListIterator iterator = {
    .list      = list,
    .nextEntry = 0,
    .chapters  = chapters,
  };
  return iterator;
}

/**
 * Check if the search list iterator has another entry to return.
 *
 * @param iterator  the search list iterator
 *
 * @return <code>true</code> if getNextChapter() may be called
 **/
static INLINE bool hasNextChapter(const SearchListIterator *iterator)
{
  return (iterator->nextEntry < iterator->list->firstDeadEntry);
}

/**
 * Return a pointer to the next live chapter in the search list iteration and
 * advance the iterator. This must only be called when hasNextChapter()
 * returns <code>true</code>.
 *
 * @param iterator  the search list iterator
 *
 * @return a pointer to the next live chapter index in the search list order
 **/
static INLINE CachedChapterIndex *getNextChapter(SearchListIterator *iterator)
{
  return &iterator->chapters[iterator->list->entries[iterator->nextEntry++]];
}

/**
 * Rotate the pointers in a prefix of a search list downwards by one item,
 * pushing elements deeper into the list and moving a new chapter to the start
 * of the search list. This is the "make most recent" operation on the search
 * list.
 *
 * If the search list provided is <code>[ 0 1 2 3 4 ]</code> and the prefix
 * length is <code>4</code>, then <code>3</code> is being moved to the front.
 * The search list after the call will be <code>[ 3 0 1 2 4 ]</code> and the
 * function will return <code>3</code>.
 *
 * @param searchList    the chapter index search list to rotate
 * @param prefixLength  the length of the prefix of the list to rotate
 *
 * @return the array index of the chapter cache entry that is now at the front
 *         of the search list
 **/
static INLINE uint8_t rotateSearchList(SearchList *searchList,
                                       uint8_t     prefixLength)
{
  // Grab the value of the last entry in the list prefix.
  uint8_t mostRecent = searchList->entries[prefixLength - 1];

  if (prefixLength > 1) {
    // Push the first N-1 entries down by one entry, overwriting the entry
    // we just grabbed.
    memmove(&searchList->entries[1],
            &searchList->entries[0],
            prefixLength - 1);

    // We now have a hole at the front of the list in which we can place the
    // rotated entry.
    searchList->entries[0] = mostRecent;
  }

  // This function is also used to move a dead chapter to the front of the
  // list, in which case the suffix of dead chapters was pushed down too.
  if (searchList->firstDeadEntry < prefixLength) {
    searchList->firstDeadEntry += 1;
  }

  return mostRecent;
}

/**
 * Purge invalid cache entries, marking them as dead and moving them to the
 * end of the search list, then push any chapters that have skipSearch set
 * down so they follow all the remaining live, valid chapters in the search
 * list. This effectively sorts the search list into three regions--active,
 * skippable, and dead--while maintaining the LRU ordering that already
 * existed (a stable sort).
 *
 * This operation must only be called during the critical section in
 * updateSparseCache() since it effectively changes cache membership.
 *
 * @param searchList            the chapter index search list to purge
 * @param chapters              the chapter index cache entries
 * @param oldestVirtualChapter  the oldest virtual chapter
 **/
void purgeSearchList(SearchList               *searchList,
                     const CachedChapterIndex  chapters[],
                     uint64_t                  oldestVirtualChapter);

#endif /* SEARCH_LIST_H */
