/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SPARSE_CACHE_H
#define SPARSE_CACHE_H

#include "geometry.h"
#include "type-defs.h"

/**
 * sparse_cache is the cache of entire chapter indexes from sparse chapters
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
struct sparse_cache;

/* Bare declaration to avoid include dependency loops. */
struct index_zone;

/**
 * Allocate and initialize a sparse chapter index cache.
 *
 * @param [in]  geometry    the geometry governing the volume
 * @param [in]  capacity    the number of chapters the cache will hold
 * @param [in]  zone_count  the number of zone threads using the cache
 * @param [out] cache_ptr   a pointer in which to return the new cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_sparse_cache(const struct geometry *geometry,
				   unsigned int capacity,
				   unsigned int zone_count,
				   struct sparse_cache **cache_ptr);

/**
 * Destroy and free a sparse chapter index cache.
 *
 * @param cache  the cache to free
 **/
void free_sparse_cache(struct sparse_cache *cache);

/**
 * Get the number of bytes of memory used by a sparse chapter cache.
 *
 * @param cache  the cache to measure
 **/
size_t get_sparse_cache_memory_size(const struct sparse_cache *cache);


/**
 * Check whether a sparse chapter index is present in the chapter cache. This
 * is only intended for use by the zone threads.
 *
 * @param cache            the cache to search for the virtual chapter
 * @param virtual_chapter  the virtual chapter number of the chapter index
 * @param zone_number      the zone number of the calling thread
 *
 * @return <code>true</code> iff the sparse chapter index is cached
 **/
bool sparse_cache_contains(struct sparse_cache *cache,
			   uint64_t virtual_chapter,
			   unsigned int zone_number);

/**
 * Update the sparse cache to contain a chapter index.
 *
 * This function must be called by all the zone threads with the same chapter
 * numbers to correctly enter the thread barriers used to synchronize the
 * cache updates.
 *
 * @param zone             the index zone
 * @param virtual_chapter  the virtual chapter number of the chapter index
 *
 * @return UDS_SUCCESS or an error code if the chapter index could not be
 *         read or decoded
 **/
int __must_check update_sparse_cache(struct index_zone *zone,
				     uint64_t virtual_chapter);

/**
 * Mark every chapter in the cache as invalid.
 *
 * Note that sparse_cache_contains() does and must still return true for
 * entries in the cache after this call, but those entries will not be
 * searched and their data will be invalid.
 *
 * @param cache  the cache to invalidate
 **/
void invalidate_sparse_cache(struct sparse_cache *cache);

/**
 * Search the cached sparse chapter indexes for a chunk name, returning a
 * virtual chapter number and record page number that may contain the name.
 *
 * @param [in]     zone                 the zone containing the volume, sparse
 *                                      chapter index cache and the index page
 *                                      number map
 * @param [in]     name                 the chunk name to search for
 * @param [in,out] virtual_chapter_ptr  If <code>UINT64_MAX</code> on input,
 *                                      search all cached chapters, else search
 *                                      the specified virtual chapter, if
 *                                      cached.
 *                                      On output, if a match was found, set to
 *                                      the virtual chapter number of the
 *                                      match, otherwise set to UINT64_MAX on
 *                                      a miss.
 * @param [out]    record_page_ptr      the record page number of a match, else
 *                                      NO_CHAPTER_INDEX_ENTRY if nothing
 *                                      matched
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check search_sparse_cache(struct index_zone *zone,
				     const struct uds_chunk_name *name,
				     uint64_t *virtual_chapter_ptr,
				     int *record_page_ptr);

#endif /* SPARSE_CACHE_H */
