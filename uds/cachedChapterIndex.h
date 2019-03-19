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
 * $Id: //eng/uds-releases/gloria/src/uds/cachedChapterIndex.h#5 $
 */

#ifndef CACHED_CHAPTER_INDEX_H
#define CACHED_CHAPTER_INDEX_H

#include "chapterIndex.h"
#include "common.h"
#include "compiler.h"
#include "cpu.h"
#include "geometry.h"
#include "indexPageMap.h"
#include "typeDefs.h"
#include "volume.h"

/**
 * These counters are essentially fields of the CachedChapterIndex, but are
 * segregated into this structure because they are frequently modified. They
 * are grouped and aligned to keep them on different cache lines from the
 * chapter fields that are accessed far more often than they are updated.
 **/
struct __attribute__((aligned(CACHE_LINE_BYTES))) cachedIndexCounters {
  /** the total number of search hits since this chapter was cached */
  uint64_t searchHits;

  /** the total number of search misses since this chapter was cached */
  uint64_t searchMisses;

  /** the number of consecutive search misses since the last cache hit */
  uint64_t consecutiveMisses;
};
typedef struct cachedIndexCounters CachedIndexCounters;

/**
 * CachedChapterIndex is the structure for a cache entry, representing a
 * single cached chapter index in the sparse chapter index cache.
 **/
struct __attribute__((aligned(CACHE_LINE_BYTES))) cachedChapterIndex {
  /**
   * The virtual chapter number of the cached chapter index. UINT64_MAX means
   * this cache entry is unused. Must only be modified in the critical section
   * in updateSparseCache().
   **/
  uint64_t          virtualChapter;

  /*
   * This flag is mutable between cache updates, but it rarely changes and
   * is frequently accessed, so it groups with the immutable fields.
   *
   * If set, skip the chapter when searching the entire cache.  This flag is
   * just a performance optimization.  If we do not see a recent change to it,
   * it will be corrected when we pass through a memory barrier while getting
   * the next request from the queue.  So we may do one extra search of the
   * chapter index, or miss one deduplication opportunity.
   */
  bool              skipSearch;

  // These pointers are immutable during the life of the cache. The contents
  // of the arrays change when the cache entry is replaced.

  /** pointer to a cache-aligned array of ChapterIndexPages */
  ChapterIndexPage *indexPages;

  /** pointer to a cache-aligned array of raw index page data */
  byte             *pageData;

  // The cache-aligned counters change often and are placed at the end of the
  // structure to prevent false sharing with the more stable fields above.

  /*** counter values updated by the thread servicing zone zero */
  CachedIndexCounters counters;
};
typedef struct cachedChapterIndex CachedChapterIndex;

/**
 * Initialize a CachedChapterIndex, allocating the memory for the array of
 * ChapterIndexPages and the raw index page data. The chapter index will be
 * marked as unused (virtualChapter == UINT64_MAX).
 *
 * @param chapter   the chapter index cache entry to initialize
 * @param geometry  the geometry governing the volume
 **/
int initializeCachedChapterIndex(CachedChapterIndex *chapter,
                                 const Geometry     *geometry)
  __attribute__((warn_unused_result));

/**
 * Destroy a CachedChapterIndex, freeing the memory allocated for the
 * ChapterIndexPages and raw index page data.
 *
 * @param chapter   the chapter index cache entry to destroy
 **/
void destroyCachedChapterIndex(CachedChapterIndex *chapter);

/**
 * Assign a new value to the skipSearch flag of a cached chapter index.
 *
 * @param chapter     the chapter index cache entry to modify
 * @param skipSearch  the new value of the skipSearch falg
 **/
static INLINE void setSkipSearch(CachedChapterIndex *chapter, bool skipSearch)
{
  // Explicitly check if the field is set so we don't keep dirtying the memory
  // cache line on continued search hits.
  if (READ_ONCE(chapter->skipSearch) != skipSearch) {
    WRITE_ONCE(chapter->skipSearch, skipSearch);
  }
}

/**
 * Check if a cached sparse chapter index should be skipped over in the search
 * for a chunk name. Filters out unused, invalid, disabled, and irrelevant
 * cache entries.
 *
 * @param zone            the zone doing the check
 * @param chapter         the cache entry search candidate
 * @param virtualChapter  the virtualChapter containing a hook, or UINT64_MAX
 *                        if searching the whole cache for a non-hook
 *
 * @return <code>true</code> if the provided chapter index should be skipped
 **/
static INLINE bool shouldSkipChapterIndex(const IndexZone *zone,
                                          const CachedChapterIndex *chapter,
                                          uint64_t virtualChapter)
{
  // Don't search unused entries (contents undefined) or invalid entries
  // (the chapter is no longer the zone's view of the volume).
  if ((chapter->virtualChapter == UINT64_MAX)
      || (chapter->virtualChapter < zone->oldestVirtualChapter)) {
    return true;
  }

  if (virtualChapter != UINT64_MAX) {
    // If the caller specified a virtual chapter, only search the cache
    // entry containing that chapter.
    return (virtualChapter != chapter->virtualChapter);
  } else {
    // When searching the entire cache, save time by skipping over chapters
    // that have had too many consecutive misses.
    return READ_ONCE(chapter->skipSearch);
  }
}

/**
 * Cache a chapter index, reading all the index pages from the volume and
 * initializing the array of ChapterIndexPages in the cache entry to represent
 * them. The virtualChapter field of the cache entry will be set to UINT64_MAX
 * if there is any error since the remaining mutable fields will be in an
 * undefined state.
 *
 * @param chapter         the chapter index cache entry to replace
 * @param virtualChapter  the virtual chapter number of the index to read
 * @param volume          the volume containing the chapter index
 *
 * @return UDS_SUCCESS or an error code
 **/
int cacheChapterIndex(CachedChapterIndex *chapter,
                      uint64_t            virtualChapter,
                      const Volume       *volume)
  __attribute__((warn_unused_result));

/**
 * Search a single cached sparse chapter index for a chunk name, returning the
 * record page number that may contain the name.
 *
 * @param [in]  chapter        the cache entry for the chapter to search
 * @param [in]  geometry       the geometry governing the volume
 * @param [in]  indexPageMap   the index page number map for the volume
 * @param [in]  name           the chunk name to search for
 * @param [out] recordPagePtr  the record page number of a match, else
 *                             NO_CHAPTER_INDEX_ENTRY if nothing matched
 *
 * @return UDS_SUCCESS or an error code
 **/
int searchCachedChapterIndex(CachedChapterIndex *chapter,
                             const Geometry     *geometry,
                             const IndexPageMap *indexPageMap,
                             const UdsChunkName *name,
                             int                *recordPagePtr)
  __attribute__((warn_unused_result));

#endif /* CACHED_CHAPTER_INDEX_H */
