/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H

#include <linux/atomic.h>

#include "chapter-index.h"
#include "common.h"
#include "compiler.h"
#include "geometry.h"
#include "permassert.h"
#include "volume-store.h"

struct request_list {
	struct uds_request *first;
	struct uds_request *last;
};

struct cached_page {
	/* whether this page is currently being read asynchronously */
	bool cp_read_pending;
	/* if equal to num_cache_entries, the page is invalid */
	unsigned int cp_physical_page;
	/* the value of the volume clock when this page was last used */
	int64_t cp_last_used;
	/* the cache page data */
	struct volume_page cp_page_data;
	/* the chapter index page. This is here, even for record pages */
	struct delta_index_page cp_index_page;
};

enum {
	VOLUME_CACHE_MAX_ENTRIES = (UINT16_MAX >> 1),
	VOLUME_CACHE_QUEUED_FLAG = (1 << 15),
	VOLUME_CACHE_MAX_QUEUED_READS = 4096,
};

struct queued_read {
	/* whether this queue entry is invalid */
	bool invalid;
	/* whether this queue entry has a pending read on it */
	bool reserved;
	/* physical page to read */
	unsigned int physical_page;
	/* list of requests waiting on a queued read */
	struct request_list request_list;
};

/*
 * Value stored atomically in a search_pending_counter.  The low order
 * 32 bits is the physical page number of the cached page being read.
 * The high order 32 bits is a sequence number.
 *
 * An invalidate counter is only written by its zone thread by calling
 * the begin_pending_search or end_pending_search methods.
 *
 * Any other thread that is accessing an invalidate counter is reading
 * the value in the wait_for_pending_searches method.
 */
typedef int64_t invalidate_counter_t;
/*
 * Fields of invalidate_counter_t.
 * These must be 64 bit, so an enum cannot be not used.
 */
#define PAGE_FIELD ((long) UINT_MAX) /* The page number field */
#define COUNTER_LSB (PAGE_FIELD + 1L) /* The LSB of the counter field */

struct __attribute__((aligned(CACHE_LINE_BYTES))) search_pending_counter {
	atomic64_t atomic_value;
};

struct page_cache {
	/* Geometry governing the volume */
	const struct geometry *geometry;
	/* The number of zones */
	unsigned int zone_count;
	/* The number of index entries */
	unsigned int num_index_entries;
	/* The max number of cached entries */
	uint16_t num_cache_entries;
	/*
	 * The index used to quickly access page in cache - top bit is a
	 * 'queued' flag
	 */
	uint16_t *index;
	/* The cache */
	struct cached_page *cache;
	/*
	 * A counter for each zone to keep track of when a search is occurring
	 * within that zone.
	 */
	struct search_pending_counter *search_pending_counters;
	/* Queued reads, as a circular array, with first and last indexes */
	struct queued_read *read_queue;
	/*
	 * All entries above this point are constant once the structure has
	 * been initialized.
	 */

	/**
	 * Entries are enqueued at read_queue_last.
	 * To 'reserve' entries, we get the entry pointed to by
	 * read_queue_last_read and increment last read.  This is done
	 * with a lock so if another reader thread reserves a read, it
	 * will grab the next one.  After every read is completed, the
	 * reader thread calls release_read_queue_entry which
	 * increments read_queue_first until it is equal to
	 * read_queue_last_read, but only if the value pointed to by
	 * read_queue_first is no longer pending. This means that if n
	 * reads are outstanding, read_queue_first may not be
	 * incremented until the last of the reads finishes.
	 *
	 *  First                    Last
	 * ||    |    |    |    |    |    ||
	 *   LR   (1)   (2)
	 *
	 * Read thread 1 increments last read (1), then read thread 2
	 * increments it (2). When each read completes, it checks to
	 * see if it can increment first, when all concurrent reads
	 * have completed, read_queue_first should equal
	 * read_queue_last_read.
	 **/
	uint16_t read_queue_first;
	uint16_t read_queue_last_read;
	uint16_t read_queue_last;
	/* Page access counter */
	atomic64_t clock;
};

/**
 * Allocate a cache for a volume.
 *
 * @param geometry           The geometry governing the volume
 * @param chapters_in_cache  The size (in chapters) of the page cache
 * @param zone_count         The number of zones in the index
 * @param cache_ptr          A pointer to hold the new page cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_page_cache(const struct geometry *geometry,
				 unsigned int chapters_in_cache,
				 unsigned int zone_count,
				 struct page_cache **cache_ptr);

/**
 * Clean up a volume's cache
 *
 * @param cache the volumecache
 **/
void free_page_cache(struct page_cache *cache);

/**
 * Remove all entries and release all cache data from a page cache.
 *
 * @param cache  The page cache
 **/
void invalidate_page_cache(struct page_cache *cache);

/**
 * Invalidates a page cache for a particular chapter
 *
 * @param cache             the page cache
 * @param chapter           the chapter
 * @param pages_per_chapter the number of pages per chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
invalidate_page_cache_for_chapter(struct page_cache *cache,
				  unsigned int chapter,
				  unsigned int pages_per_chapter);

/**
 * Make the page the most recent in the cache
 *
 * @param cache    the page cache
 * @param page_ptr the page to make most recent
 **/
void make_page_most_recent(struct page_cache *cache,
			   struct cached_page *page_ptr);

/**
 * Verifies that a page is in the cache.  This method is only exposed for the
 * use of unit tests.
 *
 * @param cache the cache to verify
 * @param page  the page to find
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check assert_page_in_cache(struct page_cache *cache,
				      struct cached_page *page);

/**
 * Gets a page from the cache.
 *
 * @param [in] cache         the page cache
 * @param [in] physical_page the page number
 * @param [out] page         the found page
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_page_from_cache(struct page_cache *cache,
				     unsigned int physical_page,
				     struct cached_page **page);

/**
 * Enqueue a read request
 *
 * @param cache          the page cache
 * @param request        the request that depends on the read
 * @param physical_page  the physical page for the request
 *
 * @return UDS_QUEUED    if the page was queued
 *         UDS_SUCCESS   if the queue was full
 *         an error code if there was an error
 **/
int __must_check enqueue_read(struct page_cache *cache,
			      struct uds_request *request,
			      unsigned int physical_page);

/**
 * Reserves a queued read for future dequeuing, but does not remove it from
 * the queue. Must call release_read_queue_entry to complete the process
 *
 * @param cache           the page cache
 * @param queue_pos       the position in the read queue for this pending read
 * @param first_requests  list of requests for the pending read
 * @param physical_page   the physical page for the requests
 * @param invalid         whether or not this entry is invalid
 *
 * @return UDS_SUCCESS or an error code
 **/
bool reserve_read_queue_entry(struct page_cache *cache,
			      unsigned int *queue_pos,
			      struct uds_request **first_requests,
			      unsigned int *physical_page,
			      bool *invalid);

/**
 * Releases a read from the queue, allowing it to be reused by future
 * enqueues
 *
 * @param cache      the page cache
 * @param queue_pos  queue entry position
 **/
void release_read_queue_entry(struct page_cache *cache,
			      unsigned int queue_pos);

/**
 * Return the next read queue entry position after the given position.
 *
 * @param position  The read queue entry position to increment
 *
 * @return the position of the next read queue entry
 **/
static INLINE uint16_t next_read_queue_position(uint16_t position)
{
	return (position + 1) % VOLUME_CACHE_MAX_QUEUED_READS;
}

/**
 * Check for the page cache read queue being full.
 *
 * @param cache  the page cache for which to check the read queue.
 *
 * @return  true if the read queue for cache is full, false otherwise.
 **/
static INLINE bool read_queue_is_full(struct page_cache *cache)
{
	return (cache->read_queue_first ==
		next_read_queue_position(cache->read_queue_last));
}

/**
 * Selects a page in the cache to be used for a read.
 *
 * This will clear the pointer in the page map and
 * set read_pending to true on the cache page
 *
 * @param cache     the page cache
 * @param page_ptr  the page to add
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check select_victim_in_cache(struct page_cache *cache,
					struct cached_page **page_ptr);

/**
 * Completes an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and point
 * the page map for the new page to this entry
 *
 * @param cache          the page cache
 * @param physical_page  the page number
 * @param page           the page to complete processing on
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_page_in_cache(struct page_cache *cache,
				   unsigned int physical_page,
				   struct cached_page *page);

/**
 * Cancels an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and clear
 * the read queued flag on the page map entry, if it
 * was set.
 *
 * @param cache          the page cache
 * @param physical_page  the page number to clear the queued read flag on
 * @param page           the page to cancel processing on
 **/
void cancel_page_in_cache(struct page_cache *cache,
			  unsigned int physical_page,
			  struct cached_page *page);

/**
 * Get the page cache size
 *
 * @param cache the page cache
 *
 * @return the size of the page cache
 **/
size_t __must_check get_page_cache_size(struct page_cache *cache);

/**
 * Read the invalidate counter for the given zone.
 *
 * @param cache        the page cache
 * @param zone_number  the zone number
 *
 * @return the invalidate counter value
 **/
static INLINE invalidate_counter_t
get_invalidate_counter(struct page_cache *cache, unsigned int zone_number)
{
	return atomic64_read(&cache->search_pending_counters[zone_number].atomic_value);
}

/**
 * Write the invalidate counter for the given zone.
 *
 * @param cache               the page cache
 * @param zone_number         the zone number
 * @param invalidate_counter  the invalidate counter value to write
 **/
static INLINE void set_invalidate_counter(struct page_cache *cache,
					  unsigned int zone_number,
					  invalidate_counter_t invalidate_counter)
{
	atomic64_set(&cache->search_pending_counters[zone_number].atomic_value,
		     invalidate_counter);
}

/**
 * Return the physical page number of the page being searched.  The return
 * value is only valid if search_pending indicates that a search is in progress.
 *
 * @param counter  the invalidate counter value to check
 *
 * @return the page that the zone is searching
 **/
static INLINE unsigned int page_being_searched(invalidate_counter_t counter)
{
	return counter & PAGE_FIELD;
}

/**
 * Determines whether a given value indicates that a search is occuring.
 *
 * @param invalidate_counter  the invalidate counter value to check
 *
 * @return true if a search is pending, false otherwise
 **/
static INLINE bool search_pending(invalidate_counter_t invalidate_counter)
{
	return (invalidate_counter & COUNTER_LSB) != 0;
}

/**
 * Increment the counter for the specified zone to signal that a search has
 * begun.  Also set which page is being searched.  The search_pending_counters
 * are protecting read access to pages indexed by the cache.  This is the
 * "lock" action.
 *
 * @param cache          the page cache
 * @param physical_page  the page that the zone is searching
 * @param zone_number    the zone number
 **/
static INLINE void begin_pending_search(struct page_cache *cache,
					unsigned int physical_page,
					unsigned int zone_number)
{
	invalidate_counter_t invalidate_counter =
		get_invalidate_counter(cache, zone_number);
	invalidate_counter &= ~PAGE_FIELD;
	invalidate_counter |= physical_page;
	invalidate_counter += COUNTER_LSB;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
	ASSERT_LOG_ONLY(search_pending(invalidate_counter),
			"Search is pending for zone %u",
			zone_number);
	/*
	 * This memory barrier ensures that the write to the invalidate counter
	 * is seen by other threads before this thread accesses the cached
	 * page.  The corresponding read memory barrier is in
	 * wait_for_pending_searches.
	 */
	smp_mb();
}

/**
 * Increment the counter for the specified zone to signal that a search has
 * finished.  We do not need to reset the page since we only should ever look
 * at the page value if the counter indicates a search is ongoing.  The
 * search_pending_counters are protecting read access to pages indexed by the
 * cache.  This is the "unlock" action.
 *
 * @param cache        the page cache
 * @param zone_number  the zone number
 **/
static INLINE void end_pending_search(struct page_cache *cache,
				      unsigned int zone_number)
{
	invalidate_counter_t invalidate_counter;
	/*
	 * This memory barrier ensures that this thread completes reads of the
	 * cached page before other threads see the write to the invalidate
	 * counter.
	 */
	smp_mb();

	invalidate_counter = get_invalidate_counter(cache, zone_number);
	ASSERT_LOG_ONLY(search_pending(invalidate_counter),
			"Search is pending for zone %u",
			zone_number);
	invalidate_counter += COUNTER_LSB;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
}

#endif /* PAGE_CACHE_H */
