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
 * $Id: //eng/uds-releases/gloria/src/uds/sparseCache.h#2 $
 */

#ifndef SPARSE_CACHE_H
#define SPARSE_CACHE_H

#include "cacheCounters.h"
#include "geometry.h"
#include "indexZone.h"
#include "typeDefs.h"

/**
 * SparseCache is the cache of entire chapter indexes from sparse chapters
 * used for searching for chunks after all other search paths have failed. It
 * contains only complete chapter indexes; record pages from sparse chapters
 * and single index pages used for resolving hooks are kept in the volume page
 * cache.
 *
 * Searching the cache is an unsynchronized operation. Changing the contents
 * of the cache is a coordinated process requiring the coordinated
 * participation of all zone threads via the careful use of barrier messages
 * sent to all the index zones by the triage queue worker thread.
 **/
typedef struct sparseCache SparseCache;

// Bare declaration to avoid include dependency loops.
struct index;

/**
 * Allocate and initialize a sparse chapter index cache.
 *
 * @param [in]  geometry   the geometry governing the volume
 * @param [in]  capacity   the number of chapters the cache will hold
 * @param [in]  zoneCount  the number of zone threads using the cache
 * @param [out] cachePtr   a pointer in which to return the new cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeSparseCache(const Geometry  *geometry,
                    unsigned int     capacity,
                    unsigned int     zoneCount,
                    SparseCache    **cachePtr)
  __attribute__((warn_unused_result));

/**
 * Destroy and free a sparse chapter index cache.
 *
 * @param cache  the cache to free
 **/
void freeSparseCache(SparseCache *cache);

/**
 * Get the number of bytes of memory used by a sparse chapter cache.
 *
 * @param cache  the cache to measure
 **/
size_t getSparseCacheMemorySize(const SparseCache *cache);

/**
 * Get the current cache counter values from a sparse cache.
 *
 * @param cache  the cache from which to gather counters
 *
 * @return statistics from the sparse cache
 **/
CacheCounters getSparseCacheCounters(const SparseCache *cache);

/**
 * Check whether a sparse chapter index is present in the chapter cache. This
 * is only intended for use by the zone threads.
 *
 * @param cache           the cache to search for the virtual chapter
 * @param virtualChapter  the virtual chapter number of the chapter index
 * @param zoneNumber      the zone number of the calling thread
 *
 * @return <code>true</code> iff the sparse chapter index is cached
 **/
bool sparseCacheContains(SparseCache  *cache,
                         uint64_t      virtualChapter,
                         unsigned int  zoneNumber);

/**
 * Update the sparse cache to contain a chapter index.
 *
 * This function must be called by all the zone threads with the same chapter
 * numbers to correctly enter the thread barriers used to synchronize the
 * cache updates.
 *
 * @param zone            the index zone
 * @param virtualChapter  the virtual chapter number of the chapter index
 *
 * @return UDS_SUCCESS or an error code if the chapter index could not be
 *         read or decoded
 **/
int updateSparseCache(IndexZone *zone, uint64_t virtualChapter)
  __attribute__((warn_unused_result));


/**
 * Search the cached sparse chapter indexes for a chunk name, returning a
 * virtual chapter number and record page number that may contain the name.
 *
 * @param [in]     zone               the zone containing the volume, sparse
 *                                    chapter index cache and the index page
 *                                    number map
 * @param [in]     name               the chunk name to search for
 * @param [in,out] virtualChapterPtr  If <code>UINT64_MAX</code> on input,
 *                                    search all cached chapters, else search
 *                                    the specified virtual chapter, if cached.
 *                                    On output, if a match was found, set to
 *                                    the virtual chapter number of the match,
 *                                    otherwise set to UINT64_MAX on a miss.
 * @param [out]    recordPagePtr      the record page number of a match, else
 *                                    NO_CHAPTER_INDEX_ENTRY if nothing matched
 *
 * @return UDS_SUCCESS or an error code
 **/
int searchSparseCache(IndexZone          *zone,
                      const UdsChunkName *name,
                      uint64_t           *virtualChapterPtr,
                      int                *recordPagePtr)
  __attribute__((warn_unused_result));

#endif /* SPARSE_CACHE_H */
