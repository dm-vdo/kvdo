// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
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
 **/

#include "sparse-cache.h"

#include "chapter-index.h"
#include "common.h"
#include "config.h"
#include "index.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds-threads.h"

enum {
	/** The number of consecutive search misses that will disable searching
	 */
	SKIP_SEARCH_THRESHOLD = 20000,

	/** a named constant to use when identifying zone zero */
	ZONE_ZERO = 0
};

/**
 * These counters are essentially fields of the struct cached_chapter_index,
 * but are segregated into this structure because they are frequently modified.
 * They are grouped and aligned to keep them on different cache lines from the
 * chapter fields that are accessed far more often than they are updated.
 **/
struct __attribute__((aligned(CACHE_LINE_BYTES))) cached_index_counters {
	/** the total number of search hits since this chapter was cached */
	uint64_t search_hits;

	/** the total number of search misses since this chapter was cached */
	uint64_t search_misses;

	/** the number of consecutive search misses since the last cache hit */
	uint64_t consecutive_misses;
};

/**
 * struct cached_chapter_index is the structure for a cache entry, representing
 * a single cached chapter index in the sparse chapter index cache.
 **/
struct __attribute__((aligned(CACHE_LINE_BYTES))) cached_chapter_index {
	/*
	 * The virtual chapter number of the cached chapter index. UINT64_MAX
	 * means this cache entry is unused. Must only be modified in the
	 * critical section in updateSparseCache().
	 */
	uint64_t virtual_chapter;

	/* The number of index pages in a chapter */
	unsigned int index_pages_count;

	/*
	 * This flag is mutable between cache updates, but it rarely changes
	 * and is frequently accessed, so it groups with the immutable fields.
	 *
	 * If set, skip the chapter when searching the entire cache.  This flag
	 * is just a performance optimization.  If we do not see a recent
	 * change to it, it will be corrected when we pass through a memory
	 * barrier while getting the next request from the queue.  So we may do
	 * one extra search of the chapter index, or miss one deduplication
	 * opportunity.
	 */
	bool skip_search;

	/*
	 * These pointers are immutable during the life of the cache. The
	 * contents of the arrays change when the cache entry is replaced.
	 */

	/* pointer to a cache-aligned array of ChapterIndexPages */
	struct delta_index_page *index_pages;

	/* pointer to an array of volume pages containing the index pages */
	struct volume_page *volume_pages;

	/*
	 * The cache-aligned counters change often and are placed at the end of
	 * the structure to prevent false sharing with the more stable fields
	 * above.
	 */

	/* counter values updated by the thread servicing zone zero */
	struct cached_index_counters counters;
};

/**
 * A search_list represents the permutations of the sparse chapter index cache
 * entry array. Those permutations express an ordering on the chapter indexes,
 * from most recently accessed to least recently accessed, which is the order
 * in which the indexes should be searched and the reverse order in which they
 * should be evicted from the cache (LRU cache replacement policy).
 *
 * Cache entries that are dead (virtual_chapter == UINT64_MAX) are kept as a
 * suffix of the list, avoiding the need to even iterate over them to search,
 * and ensuring that dead entries are replaced before any live entries are
 * evicted.
 *
 * The search list is intended to be instantated for each zone thread,
 * avoiding any need for synchronization. The structure is allocated on a
 * cache boundary to avoid false sharing of memory cache lines between zone
 * threads.
 **/
struct search_list {
	/** The number of cached chapter indexes and search list entries */
	uint8_t capacity;

	/** The index in the entries array of the first dead cache entry */
	uint8_t first_dead_entry;

	/** The chapter array indexes representing the chapter search order */
	uint8_t entries[];
};

/**
 * search_list_iterator captures the fields needed to iterate over the live
 * entries in a search list and return the struct cached_chapter_index pointers
 * that the search code actually wants to deal with.
 **/
struct search_list_iterator {
	/** The search list defining the chapter search iteration order */
	struct search_list *list;

	/** The index of the next entry to return from the search list */
	unsigned int next_entry;

	/** The cached chapters that are referenced by the search list */
	struct cached_chapter_index *chapters;
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
	struct barrier begin_cache_update;
	struct barrier end_cache_update;

	/** frequently-updated counter fields (cache-aligned) */
	struct sparse_cache_counters counters;

	/** the counted array of chapter index cache entries (cache-aligned) */
	struct cached_chapter_index chapters[];
};

/**
 * Initialize a struct cached_chapter_index, allocating the memory for the
 * array of ChapterIndexPages and the raw index page data. The chapter index
 * will be marked as unused (virtual_chapter == UINT64_MAX).
 *
 * @param chapter   the chapter index cache entry to initialize
 * @param geometry  the geometry governing the volume
 **/
static int __must_check
initialize_cached_chapter_index(struct cached_chapter_index *chapter,
				const struct geometry *geometry)
{
	int result;
	unsigned int i;

	chapter->virtual_chapter = UINT64_MAX;
	chapter->index_pages_count = geometry->index_pages_per_chapter;

	result = UDS_ALLOCATE(chapter->index_pages_count,
			      struct delta_index_page,
			      __func__,
			      &chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(chapter->index_pages_count,
			      struct volume_page,
			      "sparse index volume pages",
			      &chapter->volume_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < chapter->index_pages_count; i++) {
		result = initialize_volume_page(geometry->bytes_per_page,
						&chapter->volume_pages[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**
 * Allocate and initialize a new chapter cache search list with the same
 * capacity as the cache. The index of each entry in the cache will appear
 * exactly once in the array. All the chapters in the cache are assumed to be
 * initially dead, so first_dead_entry will be zero and no chapters will be
 * returned when the search list is iterated.
 *
 * @param [in]  capacity  the number of entries in the search list
 * @param [out] list_ptr  a pointer in which to return the new search list
 **/
static int __must_check make_search_list(unsigned int capacity,
					 struct search_list **list_ptr)
{
	struct search_list *list;
	unsigned int bytes;
	uint8_t i;
	int result;

	if (capacity == 0) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list must have entries");
	}
	if (capacity > UINT8_MAX) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list capacity must fit in 8 bits");
	}

	/*
	 * We need three temporary entry arrays for purge_search_list().
	 * Allocate them contiguously with the main array.
	 */
	bytes = sizeof(struct search_list) + (4 * capacity * sizeof(uint8_t));
	result = uds_allocate_cache_aligned(bytes, "search list", &list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	list->capacity = capacity;
	list->first_dead_entry = 0;

	/*
	 * Fill in the indexes of the chapter index cache entries. These will
	 * be only ever be permuted as the search list is used.
	 */
	for (i = 0; i < capacity; i++) {
		list->entries[i] = i;
	}

	*list_ptr = list;
	return UDS_SUCCESS;
}

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
	unsigned int i;
	int result;

	cache->geometry = geometry;
	cache->capacity = capacity;
	cache->zone_count = zone_count;

	/*
	 * Scale down the skip threshold by the number of zones since we count
	 * the chapter search misses only in zone zero.
	 */
	cache->skip_search_threshold = (SKIP_SEARCH_THRESHOLD / zone_count);

	result = uds_initialize_barrier(&cache->begin_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = uds_initialize_barrier(&cache->end_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	for (i = 0; i < capacity; i++) {
		result = initialize_cached_chapter_index(&cache->chapters[i],
							 geometry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	/* Allocate each zone's independent LRU order. */
	for (i = 0; i < zone_count; i++) {
		result = make_search_list(capacity, &cache->search_lists[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

int make_sparse_cache(const struct geometry *geometry,
		      unsigned int capacity,
		      unsigned int zone_count,
		      struct sparse_cache **cache_ptr)
{
	unsigned int bytes =
		(sizeof(struct sparse_cache) +
		 (capacity * sizeof(struct cached_chapter_index)));

	struct sparse_cache *cache;
	int result = uds_allocate_cache_aligned(bytes, "sparse cache", &cache);

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

size_t get_sparse_cache_memory_size(const struct sparse_cache *cache)
{
	/*
	 * Count the delta_index_page as cache memory, but ignore all other
	 * overhead.
	 */
	size_t page_size = (sizeof(struct delta_index_page) +
			    cache->geometry->bytes_per_page);
	size_t chapter_size =
		(page_size * cache->geometry->index_pages_per_chapter);
	return (cache->capacity * chapter_size);
}

/**
 * Assign a new value to the skip_search flag of a cached chapter index.
 *
 * @param chapter      the chapter index cache entry to modify
 * @param skip_search  the new value of the skip_search falg
 **/
static INLINE void set_skip_search(struct cached_chapter_index *chapter,
				   bool skip_search)
{
	/*
	 * Explicitly check if the field is set so we don't keep dirtying the
	 * memory cache line on continued search hits.
	 */
	if (READ_ONCE(chapter->skip_search) != skip_search) {
		WRITE_ONCE(chapter->skip_search, skip_search);
	}
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

/**
 * Destroy a cached_chapter_index, freeing the memory allocated for the
 * ChapterIndexPages and raw index page data.
 *
 * @param chapter   the chapter index cache entry to destroy
 **/
static void destroy_cached_chapter_index(struct cached_chapter_index *chapter)
{
	if (chapter->volume_pages != NULL) {
		unsigned int i;

		for (i = 0; i < chapter->index_pages_count; i++) {
			destroy_volume_page(&chapter->volume_pages[i]);
		}
	}
	UDS_FREE(chapter->index_pages);
	UDS_FREE(chapter->volume_pages);
}

void free_sparse_cache(struct sparse_cache *cache)
{
	unsigned int i;

	if (cache == NULL) {
		return;
	}

	for (i = 0; i < cache->zone_count; i++) {
		UDS_FREE(UDS_FORGET(cache->search_lists[i]));
	}

	for (i = 0; i < cache->capacity; i++) {
		struct cached_chapter_index *chapter = &cache->chapters[i];

		destroy_cached_chapter_index(chapter);
	}

	uds_destroy_barrier(&cache->begin_cache_update);
	uds_destroy_barrier(&cache->end_cache_update);
	UDS_FREE(cache);
}


/**
 * Prepare to iterate over the live cache entries a search list.
 *
 * @param list      the list defining the live chapters and the search order
 * @param chapters  the chapter index entries to return from get_next_chapter()
 *
 * @return an iterator positioned at the start of the search list
 **/
static INLINE struct search_list_iterator
iterate_search_list(struct search_list *list,
		    struct cached_chapter_index chapters[])
{
	struct search_list_iterator iterator = {
		.list = list,
		.next_entry = 0,
		.chapters = chapters,
	};
	return iterator;
}

/**
 * Check if the search list iterator has another entry to return.
 *
 * @param iterator  the search list iterator
 *
 * @return <code>true</code> if get_next_chapter() may be called
 **/
static INLINE bool
has_next_chapter(const struct search_list_iterator *iterator)
{
	return (iterator->next_entry < iterator->list->first_dead_entry);
}

/**
 * Return a pointer to the next live chapter in the search list iteration and
 * advance the iterator. This must only be called when has_next_chapter()
 * returns <code>true</code>.
 *
 * @param iterator  the search list iterator
 *
 * @return a pointer to the next live chapter index in the search list order
 **/
static INLINE struct cached_chapter_index *
get_next_chapter(struct search_list_iterator *iterator)
{
	return &iterator->chapters[iterator->list
					   ->entries[iterator->next_entry++]];
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
 * @param search_list    the chapter index search list to rotate
 * @param prefix_length  the length of the prefix of the list to rotate
 *
 * @return the array index of the chapter cache entry that is now at the front
 *         of the search list
 **/
static INLINE uint8_t rotate_search_list(struct search_list *search_list,
					 uint8_t prefix_length)
{
	/* Grab the value of the last entry in the list prefix. */
	uint8_t most_recent = search_list->entries[prefix_length - 1];

	if (prefix_length > 1) {
		/*
		 * Push the first N-1 entries down by one entry, overwriting
		 * the entry we just grabbed.
		 */
		memmove(&search_list->entries[1],
			&search_list->entries[0],
			prefix_length - 1);

		/*
		 * We now have a hole at the front of the list in which we can
		 * place the rotated entry.
		 */
		search_list->entries[0] = most_recent;
	}

	/*
	 * This function is also used to move a dead chapter to the front of
	 * the list, in which case the suffix of dead chapters was pushed down
	 * too.
	 */
	if (search_list->first_dead_entry < prefix_length) {
		search_list->first_dead_entry += 1;
	}

	return most_recent;
}

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

	/* Get the chapter search order for this zone thread. */
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

			/* Move the chapter to the front of the search list. */
			rotate_search_list(iterator.list, iterator.next_entry);
			return true;
		}
	}

	/* The specified virtual chapter isn't cached. */
	if (zone_number == ZONE_ZERO) {
		score_chapter_miss(cache);
	}
	return false;
}

/**
 * Purge invalid cache entries, marking them as dead and moving them to the
 * end of the search list, then push any chapters that have skip_search set
 * down so they follow all the remaining live, valid chapters in the search
 * list. This effectively sorts the search list into three regions--active,
 * skippable, and dead--while maintaining the LRU ordering that already
 * existed (a stable sort).
 *
 * This operation must only be called during the critical section in
 * update_sparse_cache() since it effectively changes cache membership.
 *
 * @param search_list             the chapter index search list to purge
 * @param chapters                the chapter index cache entries
 * @param oldest_virtual_chapter  the oldest virtual chapter
 **/
static void purge_search_list(struct search_list *search_list,
			      const struct cached_chapter_index chapters[],
			      uint64_t oldest_virtual_chapter)
{
	uint8_t *entries, *alive, *skipped, *dead;
	unsigned int next_alive, next_skipped, next_dead;
	int i;

	if (search_list->first_dead_entry == 0) {
		/* There are no live entries in the list to purge. */
		return;
	}

	/*
	 * Partition the previously-alive entries in the list into three
	 * temporary lists, keeping the current LRU search order within each
	 * list. The element array was allocated with enough space for all four
	 * lists.
	 */
	entries = &search_list->entries[0];
	alive = &entries[search_list->capacity];
	skipped = &alive[search_list->capacity];
	dead = &skipped[search_list->capacity];
	next_alive = next_skipped = next_dead = 0;

	for (i = 0; i < search_list->first_dead_entry; i++) {
		uint8_t entry = entries[i];
		const struct cached_chapter_index *chapter = &chapters[entry];

		if ((chapter->virtual_chapter < oldest_virtual_chapter) ||
		    (chapter->virtual_chapter == UINT64_MAX)) {
			dead[next_dead++] = entry;
		} else if (chapter->skip_search) {
			skipped[next_skipped++] = entry;
		} else {
			alive[next_alive++] = entry;
		}
	}

	/*
	 * Copy the temporary lists back to the search list so we wind up with
	 * [ alive, alive, skippable, new-dead, new-dead, old-dead, old-dead ]
	 */
	memcpy(entries, alive, next_alive);
	entries += next_alive;

	memcpy(entries, skipped, next_skipped);
	entries += next_skipped;

	memcpy(entries, dead, next_dead);
	/* The first dead entry is now the start of the copied dead list. */
	search_list->first_dead_entry = (next_alive + next_skipped);
}

/**
 * Cache a chapter index, reading all the index pages from the volume and
 * initializing the array of ChapterIndexPages in the cache entry to represent
 * them. The virtual_chapter field of the cache entry will be set to UINT64_MAX
 * if there is any error since the remaining mutable fields will be in an
 * undefined state.
 *
 * @param chapter          the chapter index cache entry to replace
 * @param virtual_chapter  the virtual chapter number of the index to read
 * @param volume           the volume containing the chapter index
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
cache_chapter_index(struct cached_chapter_index *chapter,
		    uint64_t virtual_chapter,
		    const struct volume *volume)
{
	int result;
	/* Mark the cached chapter as unused in case the update fails midway. */
	chapter->virtual_chapter = UINT64_MAX;

	/*
	 * Read all the page data and initialize the entire delta_index_page
	 * array. (It's not safe for the zone threads to do it lazily--they'll
	 * race.)
	 */
	result = read_chapter_index_from_volume(volume,
						virtual_chapter,
						chapter->volume_pages,
						chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Reset all chapter counter values to zero. */
	chapter->counters.search_hits = 0;
	chapter->counters.search_misses = 0;
	chapter->counters.consecutive_misses = 0;

	/* Mark the entry as valid--it's now in the cache. */
	chapter->virtual_chapter = virtual_chapter;
	chapter->skip_search = false;

	return UDS_SUCCESS;
}

/**
 * Copy the contents of one search list to another.
 *
 * @param source  the list to copy
 * @param target  the list to replace
 **/
static INLINE void copy_search_list(const struct search_list *source,
				    struct search_list *target)
{
	*target = *source;
	memcpy(target->entries, source->entries, source->capacity);
}

int update_sparse_cache(struct index_zone *zone, uint64_t virtual_chapter)
{
	int result = UDS_SUCCESS;
	const struct uds_index *index = zone->index;
	struct sparse_cache *cache = index->volume->sparse_cache;

	/*
	 * If the chapter is already in the cache, we don't need to do a thing
	 * except update the search list order, which this check does.
	 */
	if (sparse_cache_contains(cache, virtual_chapter, zone->id)) {
		return UDS_SUCCESS;
	}

	/*
	 * Wait for every zone thread to have reached its corresponding barrier
	 * request and invoked this function before starting to modify the
	 * cache.
	 */
	uds_enter_barrier(&cache->begin_cache_update, NULL);

	/*
	 * This is the start of the critical section: the zone zero thread is
	 * captain, effectively holding an exclusive lock on the sparse cache.
	 * All the other zone threads must do nothing between the two barriers.
	 * They will wait at the end_cache_update barrier for the captain to
	 * finish the update.
	 */

	if (zone->id == ZONE_ZERO) {
		unsigned int z;
		/* Purge invalid chapters from the LRU search list. */
		struct search_list *zone_zero_list =
			cache->search_lists[ZONE_ZERO];
		purge_search_list(zone_zero_list,
				  cache->chapters,
				  zone->oldest_virtual_chapter);

		/*
		 * First check that the desired chapter is still in the volume.
		 * If it's not, the hook fell out of the index and there's
		 * nothing to do for it.
		 */
		if (virtual_chapter >= index->oldest_virtual_chapter) {
			/*
			 * Evict the least recently used live chapter, or
			 * replace a dead cache entry, all by rotating the the
			 * last list entry to the front.
			 */
			struct cached_chapter_index *victim =
				&cache->chapters[rotate_search_list(zone_zero_list,
								    cache->capacity)];

			/*
			 * Check if the victim is already dead, and if it's
			 * not, add to the tally of evicted or invalidated
			 * cache entries.
			 */
			score_eviction(zone, cache, victim);

			/*
			 * Read the index page bytes and initialize the page
			 * array.
			 */
			result = cache_chapter_index(victim, virtual_chapter,
						     index->volume);
		}

		/*
		 * Copy the new search list state to all the other zone threads
		 * so they'll get the result of pruning and see the new
		 * chapter.
		 */
		for (z = 1; z < cache->zone_count; z++) {
			copy_search_list(zone_zero_list,
					 cache->search_lists[z]);
		}
	}

	/*
	 * This is the end of the critical section. All cache invariants must
	 * have been restored--it will be shared/read-only again beyond the
	 * barrier.
	 */

	uds_enter_barrier(&cache->end_cache_update, NULL);
	return result;
}

/**
 * Release the all cached page data for a cached_chapter_index.
 *
 * @param chapter  the chapter index cache entry to release
 **/
static void release_cached_chapter_index(struct cached_chapter_index *chapter)
{
	if (chapter->volume_pages != NULL) {
		unsigned int i;

		for (i = 0; i < chapter->index_pages_count; i++) {
			release_volume_page(&chapter->volume_pages[i]);
		}
	}
}

void invalidate_sparse_cache(struct sparse_cache *cache)
{
	unsigned int i;

	if (cache == NULL) {
		return;
	}
	for (i = 0; i < cache->capacity; i++) {
		struct cached_chapter_index *chapter = &cache->chapters[i];

		chapter->virtual_chapter = UINT64_MAX;
		release_cached_chapter_index(chapter);
	}
}

/**
 * Check if a cached sparse chapter index should be skipped over in the search
 * for a chunk name. Filters out unused, invalid, disabled, and irrelevant
 * cache entries.
 *
 * @param zone             the zone doing the check
 * @param chapter          the cache entry search candidate
 * @param virtual_chapter  the virtual_chapter containing a hook, or UINT64_MAX
 *                         if searching the whole cache for a non-hook
 *
 * @return <code>true</code> if the provided chapter index should be skipped
 **/
static INLINE bool
should_skip_chapter_index(const struct index_zone *zone,
		          const struct cached_chapter_index *chapter,
		          uint64_t virtual_chapter)
{
	/*
	 * Don't search unused entries (contents undefined) or invalid entries
	 * (the chapter is no longer the zone's view of the volume).
	 */
	if ((chapter->virtual_chapter == UINT64_MAX) ||
	    (chapter->virtual_chapter < zone->oldest_virtual_chapter)) {
		return true;
	}

	if (virtual_chapter != UINT64_MAX) {
		/*
		 * If the caller specified a virtual chapter, only search the
		 * cache entry containing that chapter.
		 */
		return (virtual_chapter != chapter->virtual_chapter);
	} else {
		/*
		 * When searching the entire cache, save time by skipping over
		 * chapters that have had too many consecutive misses.
		 */
		return READ_ONCE(chapter->skip_search);
	}
}

/**
 * Search a single cached sparse chapter index for a chunk name, returning the
 * record page number that may contain the name.
 *
 * @param [in]  chapter          the cache entry for the chapter to search
 * @param [in]  geometry         the geometry governing the volume
 * @param [in]  index_page_map   the index page number map for the volume
 * @param [in]  name             the chunk name to search for
 * @param [out] record_page_ptr  the record page number of a match, else
 *                               NO_CHAPTER_INDEX_ENTRY if nothing matched
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
search_cached_chapter_index(struct cached_chapter_index *chapter,
			    const struct geometry *geometry,
			    const struct index_page_map *index_page_map,
			    const struct uds_chunk_name *name,
			    int *record_page_ptr)
{
	/*
	 * Find the index_page_number in the chapter that would have the chunk
	 * name.
	 */
	unsigned int physical_chapter =
		map_to_physical_chapter(geometry, chapter->virtual_chapter);
	unsigned int index_page_number =
		find_index_page_number(index_page_map, name, physical_chapter);

	return search_chapter_index_page(&chapter->index_pages[index_page_number],
				         geometry,
				         name,
				         record_page_ptr);
}

int search_sparse_cache(struct index_zone *zone,
			const struct uds_chunk_name *name,
			uint64_t *virtual_chapter_ptr,
			int *record_page_ptr)
{
	struct volume *volume = zone->index->volume;
	struct sparse_cache *cache = volume->sparse_cache;
	unsigned int zone_number = zone->id;
	/*
	 * If the caller did not specify a virtual chapter, search the entire
	 * cache.
	 */
	bool search_all = (*virtual_chapter_ptr == UINT64_MAX);

	/*
	 * Get the chapter search order for this zone thread, searching the
	 * chapters from most recently hit to least recently hit.
	 */
	struct search_list_iterator iterator =
		iterate_search_list(cache->search_lists[zone_number],
				    cache->chapters);
	while (has_next_chapter(&iterator)) {
		int result;
		struct cached_chapter_index *chapter =
			get_next_chapter(&iterator);

		/*
		 * Skip chapters no longer cached, or that have too many search
		 * misses.
		 */
		if (should_skip_chapter_index(zone, chapter,
					      *virtual_chapter_ptr)) {
			continue;
		}

		result = search_cached_chapter_index(chapter,
						     cache->geometry,
						     volume->index_page_map,
						     name,
						     record_page_ptr);
		if (result != UDS_SUCCESS) {
			return result;
		}

		/* Did we find an index entry for the name? */
		if (*record_page_ptr != NO_CHAPTER_INDEX_ENTRY) {
			if (zone_number == ZONE_ZERO) {
				score_search_hit(cache, chapter);
			}

			/* Move the chapter to the front of the search list. */
			rotate_search_list(iterator.list, iterator.next_entry);

			/*
			 * Return a matching entry as soon as it is found. It
			 * might be a false collision that has a true match in
			 * another chapter, but that's a very rare case and not
			 * worth the extra search cost or complexity.
			 */
			*virtual_chapter_ptr = chapter->virtual_chapter;
			return UDS_SUCCESS;
		}

		if (zone_number == ZONE_ZERO) {
			score_search_miss(cache, chapter);
		}

		if (!search_all) {
			/*
			 * We just searched the virtual chapter the caller
			 * specified and there was no match, so we're done.
			 */
			break;
		}
	}

	/* The name was not found in the cache. */
	*record_page_ptr = NO_CHAPTER_INDEX_ENTRY;
	return UDS_SUCCESS;
}
