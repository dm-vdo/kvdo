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
 * $Id: //eng/uds-releases/krusty/src/uds/sparseCache.c#18 $
 */

/**
 * The sparse chapter index cache is implemented as a simple array of cache
 * entries. Since the cache is small (seven chapters by default), searching
 * for a specific virtual chapter is implemented as a linear search. The cache
 * replacement policy is least-recently-used (LRU). Again, size of the cache
 * allows the LRU order to be maintained by shifting entries in an array list.
 *
 * The most important property of this cache is the absence of synchronization
 * for read operations. Safe concurrent access to the cache by the zone
 * threads is controlled by the triage queue and the barrier requests it
 * issues to the zone queues. The set of cached chapters does not and must not
 * change between the carefully coordinated calls to update_sparse_cache() from
 * the zone threads.
 *
 * The critical invariant for that coordination is the cache membership must
 * not change between those updates; the calls to sparse_cache_contains() from
 * the zone threads must all receive the same results for any virtual chapter
 * number. To ensure that critical invariant, state changes such as "that
 * virtual chapter is no longer in the volume" and "skip searching that
 * chapter because it has had too many cache misses" are represented
 * separately from the cache membership information (the virtual chapter
 * number).
 *
 * As a result of this invariant, we have the guarantee that every zone thread
 * will call update_sparse_cache() once and exactly once to request a chapter
 * that is not in the cache, and the serialization of the barrier requests
 * from the triage queue ensures they will all request the same chapter
 * number. This means the only synchronization we need can be provided by a
 * pair of thread barriers used only in the update_sparse_cache() call,
 * providing a critical section where a single zone thread can drive the cache
 * update while all the other zone threads are known to be blocked, waiting in
 * the second barrier. Outside that critical section, all the zone threads
 * implicitly hold a shared lock. Inside it, the "captain" (the thread that
 * was uniquely flagged when passing through the first barrier) holds an
 * exclusive lock. No other threads may access or modify the cache, except for
 * accessing cache statistics and similar queries.
 *
 * Cache statistics must only be modified by a single thread, conventionally
 * the zone zero thread. All fields that might be frequently updated by that
 * thread are kept in separate cache-aligned structures so they will not cause
 * cache contention via "false sharing" with the fields that are frequently
 * accessed by all of the zone threads.
 *
 * LRU order is kept independently by each zone thread, and each zone uses its
 * own list for searching and cache membership queries. The zone zero list is
 * used to decide which chapter to evict when the cache is updated, and its
 * search list is copied to the other threads at that time.
 *
 * The virtual chapter number field of the cache entry is the single field
 * indicating whether a chapter is a member of the cache or not. The value
 * <code>UINT64_MAX</code> is used to represent a null, undefined, or wildcard
 * chapter number. When present in the virtual chapter number field
 * cached_chapter_index, it indicates that the cache entry is dead, and all
 * the other fields of that entry (other than immutable pointers to cache
 * memory) are undefined and irrelevant. Any cache entry that is not marked as
 * dead is fully defined and a member of the cache--sparse_cache_contains()
 * must always return true for any virtual chapter number that appears in any
 * of the cache entries.
 *
 * A chapter index that is a member of the cache may be marked for different
 * treatment (disabling search) between calls to update_sparse_cache() in two
 * different ways. When a chapter falls off the end of the volume, its virtual
 * chapter number will be less that the oldest virtual chapter number. Since
 * that chapter is no longer part of the volume, there's no point in continuing
 * to search that chapter index. Once invalidated, that virtual chapter will
 * still be considered a member of the cache, but it will no longer be searched
 * for matching chunk names.
 *
 * The second mechanism for disabling search is the heuristic based on keeping
 * track of the number of consecutive search misses in a given chapter index.
 * Once that count exceeds a threshold, the skip_search flag will be set to
 * true, causing the chapter to be skipped in the fallback search of the
 * entire cache, but still allowing it to be found when searching for a hook
 * in that specific chapter. Finding a hook will clear the skip_search flag,
 * once again allowing the non-hook searches to use the cache entry. Again,
 * regardless of the state of the skip_search flag, the virtual chapter must
 * still considered to be a member of the cache for sparse_cache_contains().
 *
 * Barrier requests and the sparse chapter index cache are also described in
 *
 * https://intranet.permabit.com/wiki/Chapter_Index_Cache_supports_concurrent_access
 *
 * and in a message to the albireo mailing list on 5/28/2011 titled "true
 * barriers with a hook resolution queue".
 **/

#include "sparseCache.h"

#include "cachedChapterIndex.h"
#include "chapterIndex.h"
#include "common.h"
#include "index.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "searchList.h"
#include "threads.h"
#include "zone.h"

enum {
	/** The number of consecutive search misses that will disable searching
	 */
	SKIP_SEARCH_THRESHOLD = 20000,

	/** a named constant to use when identifying zone zero */
	ZONE_ZERO = 0
};

/**
 * These counter values are essentially fields of the sparse_cache, but are
 * segregated into this structure because they are frequently modified. We
 * group them and align them to keep them on different cache lines from the
 * cache fields that are accessed far more often than they are updated.
 **/
struct sparse_cache_counters {
	/** the total number of virtual chapter probes that succeeded */
	uint64_t chapter_hits;

	/** the total number of virtual chapter probes that failed */
	uint64_t chapter_misses;

	/** the total number of cache searches that found a possible match */
	uint64_t search_hits;

	/** the total number of cache searches that found no matches */
	uint64_t search_misses;

	/** the number of cache entries that fell off the end of the volume */
	uint64_t invalidations;

	/** the number of cache entries that were evicted while still valid */
	uint64_t evictions;
} __attribute__((aligned(CACHE_LINE_BYTES)));

/**
 * This is the private structure definition of a sparse_cache.
 **/
struct sparse_cache {
	/** the number of cache entries, which is the size of the chapters
	 * array */
	unsigned int capacity;

	/** the number of zone threads using the cache */
	unsigned int zone_count;

	/** the geometry governing the volume */
	const struct geometry *geometry;

	/** the number of search misses in zone zero that will disable
	 * searching */
	unsigned int skip_search_threshold;

	/** pointers to the cache-aligned chapter search order for each zone */
	struct search_list *search_lists[MAX_ZONES];

	/** the thread barriers used to synchronize the zone threads for update
	 */
	Barrier begin_cache_update;
	Barrier end_cache_update;

	/** frequently-updated counter fields (cache-aligned) */
	struct sparse_cache_counters counters;

	/** the counted array of chapter index cache entries (cache-aligned) */
	struct cached_chapter_index chapters[];
};

/**
 * Initialize a sparse chapter index cache.
 *
 * @param cache      the sparse cache to initialize
 * @param geometry   the geometry governing the volume
 * @param capacity   the number of chapters the cache will hold
 * @param zone_count  the number of zone threads using the cache
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check initialize_sparse_cache(struct sparse_cache *cache,
						const struct geometry *geometry,
						unsigned int capacity,
						unsigned int zone_count)
{
	cache->geometry = geometry;
	cache->capacity = capacity;
	cache->zone_count = zone_count;

	// Scale down the skip threshold by the number of zones since we count
	// the chapter search misses only in zone zero.
	cache->skip_search_threshold = (SKIP_SEARCH_THRESHOLD / zone_count);

	int result = initializeBarrier(&cache->begin_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = initializeBarrier(&cache->end_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	unsigned int i;
	for (i = 0; i < capacity; i++) {
		result = initialize_cached_chapter_index(&cache->chapters[i],
							 geometry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	// Allocate each zone's independent LRU order.
	for (i = 0; i < zone_count; i++) {
		result = make_search_list(capacity, &cache->search_lists[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int make_sparse_cache(const struct geometry *geometry,
		      unsigned int capacity,
		      unsigned int zone_count,
		      struct sparse_cache **cache_ptr)
{
	unsigned int bytes =
		(sizeof(struct sparse_cache) +
		 (capacity * sizeof(struct cached_chapter_index)));

	struct sparse_cache *cache;
	int result = allocateCacheAligned(bytes, "sparse cache", &cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result =
		initialize_sparse_cache(cache, geometry, capacity, zone_count);
	if (result != UDS_SUCCESS) {
		free_sparse_cache(cache);
		return result;
	}

	*cache_ptr = cache;
	return UDS_SUCCESS;
}

/**********************************************************************/
size_t get_sparse_cache_memory_size(const struct sparse_cache *cache)
{
	// Count the delta_index_page as cache memory, but ignore all other
	// overhead.
	size_t page_size = (sizeof(struct delta_index_page) +
			    cache->geometry->bytes_per_page);
	size_t chapter_size =
		(page_size * cache->geometry->index_pages_per_chapter);
	return (cache->capacity * chapter_size);
}

/**
 * Update counters to reflect a chapter access hit and clear the skip_search
 * flag on the chapter, if set.
 *
 * @param cache      the cache to update
 * @param chapter    the cache entry to update
 **/
static void score_chapter_hit(struct sparse_cache *cache,
			      struct cached_chapter_index *chapter)
{
	cache->counters.chapter_hits += 1;
	set_skip_search(chapter, false);
}

/**
 * Update counters to reflect a chapter access miss.
 *
 * @param cache      the cache to update
 **/
static void score_chapter_miss(struct sparse_cache *cache)
{
	cache->counters.chapter_misses += 1;
}

/**
 * Check if the cache entry that is about to be replaced is already dead, and
 * if it's not, add to tally of evicted or invalidated cache entries.
 *
 * @param zone       the zone used to find the oldest chapter
 * @param cache      the cache to update
 * @param chapter    the cache entry about to be replaced
 **/
static void score_eviction(struct index_zone *zone,
			   struct sparse_cache *cache,
			   struct cached_chapter_index *chapter)
{
	if (chapter->virtual_chapter == UINT64_MAX) {
		return;
	}
	if (chapter->virtual_chapter < zone->oldest_virtual_chapter) {
		cache->counters.invalidations += 1;
	} else {
		cache->counters.evictions += 1;
	}
}

/**
 * Update counters to reflect a cache search hit. This bumps the hit
 * count, clears the miss count, and clears the skip_search flag.
 *
 * @param cache      the cache to update
 * @param chapter    the cache entry to update
 **/
static void score_search_hit(struct sparse_cache *cache,
			     struct cached_chapter_index *chapter)
{
	cache->counters.search_hits += 1;
	chapter->counters.search_hits += 1;
	chapter->counters.consecutive_misses = 0;
	set_skip_search(chapter, false);
}

/**
 * Update counters to reflect a cache search miss. This bumps the consecutive
 * miss count, and if it goes over skip_search_threshold, sets the skip_search
 * flag on the chapter.
 *
 * @param cache      the cache to update
 * @param chapter    the cache entry to update
 **/
static void score_search_miss(struct sparse_cache *cache,
			      struct cached_chapter_index *chapter)
{
	cache->counters.search_misses += 1;
	chapter->counters.search_misses += 1;
	chapter->counters.consecutive_misses += 1;
	if (chapter->counters.consecutive_misses >
	    cache->skip_search_threshold) {
		set_skip_search(chapter, true);
	}
}

/**********************************************************************/
void free_sparse_cache(struct sparse_cache *cache)
{
	if (cache == NULL) {
		return;
	}

	unsigned int i;
	for (i = 0; i < cache->zone_count; i++) {
		free_search_list(&cache->search_lists[i]);
	}

	for (i = 0; i < cache->capacity; i++) {
		struct cached_chapter_index *chapter = &cache->chapters[i];
		destroy_cached_chapter_index(chapter);
	}

	destroyBarrier(&cache->begin_cache_update);
	destroyBarrier(&cache->end_cache_update);
	FREE(cache);
}


/**********************************************************************/
bool sparse_cache_contains(struct sparse_cache *cache,
			   uint64_t virtual_chapter,
			   unsigned int zone_number)
{
	/*
	 * The correctness of the barriers depends on the invariant that
	 * between calls to update_sparse_cache(), the answers this function
	 * returns must never vary--the result for a given chapter must be
	 * identical across zones. That invariant must be maintained even if
	 * the chapter falls off the end of the volume, or if searching it is
	 * disabled because of too many search misses.
	 */

	// Get the chapter search order for this zone thread.
	struct search_list_iterator iterator =
		iterate_search_list(cache->search_lists[zone_number],
				    cache->chapters);
	while (has_next_chapter(&iterator)) {
		struct cached_chapter_index *chapter =
			get_next_chapter(&iterator);
		if (virtual_chapter == chapter->virtual_chapter) {
			if (zone_number == ZONE_ZERO) {
				score_chapter_hit(cache, chapter);
			}

			// Move the chapter to the front of the search list.
			rotate_search_list(iterator.list, iterator.next_entry);
			return true;
		}
	}

	// The specified virtual chapter isn't cached.
	if (zone_number == ZONE_ZERO) {
		score_chapter_miss(cache);
	}
	return false;
}

/**********************************************************************/
int update_sparse_cache(struct index_zone *zone, uint64_t virtual_chapter)
{
	const struct index *index = zone->index;
	struct sparse_cache *cache = index->volume->sparseCache;

	// If the chapter is already in the cache, we don't need to do a thing
	// except update the search list order, which this check does.
	if (sparse_cache_contains(cache, virtual_chapter, zone->id)) {
		return UDS_SUCCESS;
	}

	// Wait for every zone thread to have reached its corresponding barrier
	// request and invoked this function before starting to modify the
	// cache.
	enterBarrier(&cache->begin_cache_update, NULL);

	/*
	 * This is the start of the critical section: the zone zero thread is
	 * captain, effectively holding an exclusive lock on the sparse cache.
	 * All the other zone threads must do nothing between the two barriers.
	 * They will wait at the end_cache_update barrier for the captain to
	 * finish the update.
	 */

	int result = UDS_SUCCESS;
	if (zone->id == ZONE_ZERO) {
		// Purge invalid chapters from the LRU search list.
		struct search_list *zone_zero_list =
			cache->search_lists[ZONE_ZERO];
		purge_search_list(zone_zero_list,
				  cache->chapters,
				  zone->oldest_virtual_chapter);

		// First check that the desired chapter is still in the volume.
		// If it's not, the hook fell out of the index and there's
		// nothing to do for it.
		if (virtual_chapter >= index->oldest_virtual_chapter) {
			// Evict the least recently used live chapter, or
			// replace a dead cache entry, all by rotating the the
			// last list entry to the front.
			struct cached_chapter_index *victim =
				&cache->chapters[rotate_search_list(zone_zero_list,
								    cache->capacity)];

			// Check if the victim is already dead, and if it's
			// not, add to the tally of evicted or invalidated
			// cache entries.
			score_eviction(zone, cache, victim);

			// Read the index page bytes and initialize the page
			// array.
			result = cache_chapter_index(victim, virtual_chapter,
						     index->volume);
		}

		// Copy the new search list state to all the other zone threads
		// so they'll get the result of pruning and see the new
		// chapter.
		unsigned int z;
		for (z = 1; z < cache->zone_count; z++) {
			copy_search_list(zone_zero_list,
					 cache->search_lists[z]);
		}
	}

	// This is the end of the critical section. All cache invariants must
	// have been restored--it will be shared/read-only again beyond the
	// barrier.

	enterBarrier(&cache->end_cache_update, NULL);
	return result;
}


/**********************************************************************/
int search_sparse_cache(struct index_zone *zone,
			const struct uds_chunk_name *name,
			uint64_t *virtual_chapter_ptr,
			int *record_page_ptr)
{
	Volume *volume = zone->index->volume;
	struct sparse_cache *cache = volume->sparseCache;
	unsigned int zone_number = zone->id;
	// If the caller did not specify a virtual chapter, search the entire
	// cache.
	bool search_all = (*virtual_chapter_ptr == UINT64_MAX);
	unsigned int chapters_searched = 0;

	// Get the chapter search order for this zone thread, searching the
	// chapters from most recently hit to least recently hit.
	struct search_list_iterator iterator =
		iterate_search_list(cache->search_lists[zone_number],
				    cache->chapters);
	while (has_next_chapter(&iterator)) {
		struct cached_chapter_index *chapter =
			get_next_chapter(&iterator);

		// Skip chapters no longer cached, or that have too many search
		// misses.
		if (should_skip_chapter_index(zone, chapter,
					      *virtual_chapter_ptr)) {
			continue;
		}

		int result =
			search_cached_chapter_index(chapter,
						    cache->geometry,
						    volume->indexPageMap,
						    name,
						    record_page_ptr);
		if (result != UDS_SUCCESS) {
			return result;
		}
		chapters_searched += 1;

		// Did we find an index entry for the name?
		if (*record_page_ptr != NO_CHAPTER_INDEX_ENTRY) {
			if (zone_number == ZONE_ZERO) {
				score_search_hit(cache, chapter);
			}

			// Move the chapter to the front of the search list.
			rotate_search_list(iterator.list, iterator.next_entry);

			// Return a matching entry as soon as it is found. It
			// might be a false collision that has a true match in
			// another chapter, but that's a very rare case and not
			// worth the extra search cost or complexity.
			*virtual_chapter_ptr = chapter->virtual_chapter;
			return UDS_SUCCESS;
		}

		if (zone_number == ZONE_ZERO) {
			score_search_miss(cache, chapter);
		}

		if (!search_all) {
			// We just searched the virtual chapter the caller
			// specified and there was no match, so we're done.
			break;
		}
	}

	// The name was not found in the cache.
	*record_page_ptr = NO_CHAPTER_INDEX_ENTRY;
	return UDS_SUCCESS;
}
