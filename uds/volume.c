/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/krusty/src/uds/volume.c#37 $
 */

#include "volume.h"

#include "cacheCounters.h"
#include "chapterIndex.h"
#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "recordPage.h"
#include "request.h"
#include "sparseCache.h"
#include "stringUtils.h"
#include "threads.h"

enum {
	MAX_BAD_CHAPTERS = 100,           // max number of contiguous bad
					  // chapters
	DEFAULT_VOLUME_READ_THREADS = 2,  // Default number of reader threads
	MAX_VOLUME_READ_THREADS = 16,     // Maximum number of reader threads
};

/**********************************************************************/
static unsigned int get_read_threads(const struct uds_parameters *user_params)
{
	unsigned int read_threads =
		(user_params == NULL ? DEFAULT_VOLUME_READ_THREADS :
				       user_params->read_threads);
	if (read_threads < 1) {
		read_threads = 1;
	}
	if (read_threads > MAX_VOLUME_READ_THREADS) {
		read_threads = MAX_VOLUME_READ_THREADS;
	}
	return read_threads;
}

/**********************************************************************/
static INLINE unsigned int map_to_page_number(struct geometry *geometry,
					      unsigned int physical_page)
{
	return ((physical_page - 1) % geometry->pages_per_chapter);
}

/**********************************************************************/
static INLINE unsigned int map_to_chapter_number(struct geometry *geometry,
						 unsigned int physical_page)
{
	return ((physical_page - 1) / geometry->pages_per_chapter);
}

/**********************************************************************/
static INLINE bool is_record_page(struct geometry *geometry,
				  unsigned int physical_page)
{
	return (((physical_page - 1) % geometry->pages_per_chapter) >=
		geometry->index_pages_per_chapter);
}

/**********************************************************************/
static INLINE unsigned int get_zone_number(Request *request)
{
	return (request == NULL) ? 0 : request->zone_number;
}

/**********************************************************************/
int map_to_physical_page(const struct geometry *geometry,
			 int chapter,
			 int page)
{
	// Page zero is the header page, so the first index page in the
	// first chapter is physical page one.
	return (1 + (geometry->pages_per_chapter * chapter) + page);
}

/**********************************************************************/
static void wait_for_read_queue_not_full(struct volume *volume,
					 Request *request)
{
	unsigned int zone_number = get_zone_number(request);
	invalidate_counter_t invalidate_counter =
		get_invalidate_counter(volume->page_cache, zone_number);

	if (search_pending(invalidate_counter)) {
		// Increment the invalidate counter to avoid deadlock where the
		// reader threads cannot make progress because they are waiting
		// on the counter and the index thread cannot because the read
		// queue is full.
		end_pending_search(volume->page_cache, zone_number);
	}

	while (read_queue_is_full(volume->page_cache)) {
		log_debug("Waiting until read queue not full");
		signal_cond(&volume->read_threads_cond);
		wait_cond(&volume->read_threads_read_done_cond,
			  &volume->read_threads_mutex);
	}

	if (search_pending(invalidate_counter)) {
		// Increment again so we get back to an odd value.
		begin_pending_search(volume->page_cache,
				     page_being_searched(invalidate_counter),
				     zone_number);
	}
}

/**********************************************************************/
int enqueue_page_read(struct volume *volume,
		      Request *request,
		      int physical_page)
{
	int result;
	// Don't allow new requests if we are shutting down, but make sure
	// to process any requests that are still in the pipeline.
	if ((volume->reader_state & READER_STATE_EXIT) != 0) {
		uds_log_info("failed to queue read while shutting down");
		return UDS_SHUTTINGDOWN;
	}

	// Mark the page as queued in the volume cache, for chapter
	// invalidation to be able to cancel a read. If we are unable to do
	// this because the queues are full, flush them first
	while ((result = enqueue_read(volume->page_cache,
				      request,
				      physical_page)) == UDS_SUCCESS) {
		log_debug("Read queues full, waiting for reads to finish");
		wait_for_read_queue_not_full(volume, request);
	}

	if (result == UDS_QUEUED) {
		/* signal a read thread */
		signal_cond(&volume->read_threads_cond);
	}

	return result;
}

/**********************************************************************/
static INLINE void
wait_to_reserve_read_queue_entry(struct volume *volume,
				 unsigned int *queue_pos,
				 Request **request_list,
				 unsigned int *physical_page,
				 bool *invalid)
{
	while (((volume->reader_state & READER_STATE_EXIT) == 0) &&
	       (((volume->reader_state & READER_STATE_STOP) != 0) ||
		!reserve_read_queue_entry(volume->page_cache,
					  queue_pos,
					  request_list,
					  physical_page,
					  invalid))) {
		wait_cond(&volume->read_threads_cond,
			  &volume->read_threads_mutex);
	}
}

/**********************************************************************/
static int init_chapter_index_page(const struct volume *volume,
				   byte *index_page,
				   unsigned int chapter,
				   unsigned int index_page_number,
				   struct delta_index_page *chapter_index_page)
{
	uint64_t ci_virtual;
	unsigned int ci_chapter;
	struct index_page_bounds bounds;
	struct geometry *geometry = volume->geometry;

	int result = initialize_chapter_index_page(chapter_index_page,
						   geometry, index_page,
						   volume->nonce);
	if (volume->lookup_mode == LOOKUP_FOR_REBUILD) {
		return result;
	}
	if (result != UDS_SUCCESS) {
		return log_error_strerror(result,
					  "Reading chapter index page for chapter %u page %u",
					  chapter, index_page_number);
	}

	result = get_list_number_bounds(volume->index_page_map, chapter,
					index_page_number, &bounds);
	if (result != UDS_SUCCESS) {
		return result;
	}

	ci_virtual = chapter_index_page->virtual_chapter_number;
	ci_chapter = map_to_physical_chapter(geometry, ci_virtual);
	if ((chapter == ci_chapter) &&
	    (bounds.lowest_list == chapter_index_page->lowest_list_number) &&
	    (bounds.highest_list == chapter_index_page->highest_list_number)) {
		return UDS_SUCCESS;
	}

	uds_log_warning("Index page map updated to %llu",
			get_last_update(volume->index_page_map));
	uds_log_warning("Page map expects that chapter %u page %u has range %u to %u, but chapter index page has chapter %llu with range %u to %u",
			chapter,
			index_page_number,
			bounds.lowest_list,
			bounds.highest_list,
			ci_virtual,
			chapter_index_page->lowest_list_number,
			chapter_index_page->highest_list_number);
	return ASSERT_WITH_ERROR_CODE(false, UDS_CORRUPT_DATA,
				      "index page map mismatch with chapter index");
}

/**********************************************************************/
static int initialize_index_page(const struct volume *volume,
				 unsigned int physical_page,
				 struct cached_page *page)
{
	unsigned int chapter =
		map_to_chapter_number(volume->geometry, physical_page);
	unsigned int index_page_number =
		map_to_page_number(volume->geometry, physical_page);
	int result =
		init_chapter_index_page(volume,
					get_page_data(&page->cp_page_data),
					chapter,
					index_page_number,
					&page->cp_index_page);
	return result;
}

/**********************************************************************/
static void read_thread_function(void *arg)
{
	struct volume *volume = arg;
	unsigned int queue_pos;
	Request *request_list;
	unsigned int physical_page;
	bool invalid = false;

	log_debug("reader starting");
	lock_mutex(&volume->read_threads_mutex);
	while (true) {
		bool record_page;
		struct cached_page *page = NULL;
		int result = UDS_SUCCESS;
		wait_to_reserve_read_queue_entry(volume,
						 &queue_pos,
						 &request_list,
						 &physical_page,
						 &invalid);
		if ((volume->reader_state & READER_STATE_EXIT) != 0) {
			break;
		}

		volume->busy_reader_threads++;

		record_page = is_record_page(volume->geometry, physical_page);

		if (!invalid) {
			// Find a place to put the read queue page we reserved
			// above.
			result = select_victim_in_cache(volume->page_cache,
							&page);
			if (result == UDS_SUCCESS) {
				unlock_mutex(&volume->read_threads_mutex);
				result =
					read_volume_page(&volume->volume_store,
							 physical_page,
							 &page->cp_page_data);
				if (result != UDS_SUCCESS) {
					uds_log_warning("Error reading page %u from volume",
							physical_page);
					cancel_page_in_cache(volume->page_cache,
							     physical_page,
							     page);
				}
				lock_mutex(&volume->read_threads_mutex);
			} else {
				uds_log_warning("Error selecting cache victim for page read");
			}

			if (result == UDS_SUCCESS) {
				if (!volume->page_cache->read_queue[queue_pos]
					     .invalid) {
					if (!record_page) {
						result = initialize_index_page(volume,
									       physical_page,
									       page);
						if (result != UDS_SUCCESS) {
							uds_log_warning("Error initializing chapter index page");
							cancel_page_in_cache(volume->page_cache,
									     physical_page,
									     page);
						}
					}

					if (result == UDS_SUCCESS) {
						result = put_page_in_cache(volume->page_cache,
									   physical_page,
									   page);
						if (result != UDS_SUCCESS) {
							uds_log_warning("Error putting page %u in cache",
								    physical_page);
							cancel_page_in_cache(volume->page_cache,
									     physical_page,
									     page);
						}
					}
				} else {
					uds_log_warning("Page %u invalidated after read",
						    physical_page);
					cancel_page_in_cache(volume->page_cache,
							     physical_page,
							     page);
					invalid = true;
				}
			}
		} else {
			log_debug("Requeuing requests for invalid page");
		}

		if (invalid) {
			result = UDS_SUCCESS;
			page = NULL;
		}

		while (request_list != NULL) {
			Request *request = request_list;
			request_list = request->next_request;

			/*
			 * If we've read in a record page, we're going to do an
			 * immediate search, in an attempt to speed up
			 * processing when we requeue the request, so that it
			 * doesn't have to go back into the
			 * get_record_from_zone code again. However, if we've
			 * just read in an index page, we don't want to search.
			 * We want the request to be processed again and
			 * get_record_from_zone to be run.  We have added new
			 * fields in request to allow the index code to know
			 * whether it can stop processing before
			 * get_record_from_zone is called again.
			 */
			if ((result == UDS_SUCCESS) && (page != NULL) &&
			    record_page) {
				if (search_record_page(get_page_data(&page->cp_page_data),
						       &request->chunk_name,
						       volume->geometry,
						       &request->old_metadata)) {
					request->sl_location = LOC_IN_DENSE;
				} else {
					request->sl_location = LOC_UNAVAILABLE;
				}
				request->sl_location_known = true;
			}

			// reflect any read failures in the request status
			request->status = result;
			restart_request(request);
		}

		release_read_queue_entry(volume->page_cache, queue_pos);

		volume->busy_reader_threads--;
		broadcast_cond(&volume->read_threads_read_done_cond);
	}
	unlock_mutex(&volume->read_threads_mutex);
	log_debug("reader done");
}

/**********************************************************************/
static int read_page_locked(struct volume *volume,
			    Request *request,
			    unsigned int physical_page,
			    bool sync_read,
			    struct cached_page **page_ptr)
{
	int result = UDS_SUCCESS;
	struct cached_page *page = NULL;

	sync_read |= ((volume->lookup_mode == LOOKUP_FOR_REBUILD) ||
		      (request == NULL) || (request->session == NULL));


	if (sync_read) {
		// Find a place to put the page.
		result = select_victim_in_cache(volume->page_cache, &page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error selecting cache victim for page read");
			return result;
		}
		result = read_volume_page(&volume->volume_store,
					  physical_page,
					  &page->cp_page_data);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error reading page %u from volume",
				    physical_page);
			cancel_page_in_cache(volume->page_cache, physical_page,
					     page);
			return result;
		}
		if (!is_record_page(volume->geometry, physical_page)) {
			result = initialize_index_page(volume, physical_page,
						       page);
			if (result != UDS_SUCCESS) {
				if (volume->lookup_mode !=
				    LOOKUP_FOR_REBUILD) {
					uds_log_warning("Corrupt index page %u",
						    physical_page);
				}
				cancel_page_in_cache(volume->page_cache,
						     physical_page,
						     page);
				return result;
			}
		}
		result = put_page_in_cache(volume->page_cache, physical_page,
					   page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error putting page %u in cache",
				    physical_page);
			cancel_page_in_cache(volume->page_cache, physical_page,
					     page);
			return result;
		}
	} else {
		result = enqueue_page_read(volume, request, physical_page);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	*page_ptr = page;

	return UDS_SUCCESS;
}

/**********************************************************************/
int get_volume_page_locked(struct volume *volume,
			   Request *request,
			   unsigned int physical_page,
			   enum cache_probe_type probe_type,
			   struct cached_page **page_ptr)
{
	struct cached_page *page = NULL;
	int result = get_page_from_cache(volume->page_cache, physical_page,
					 probe_type, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (page == NULL) {
		result = read_page_locked(volume, request, physical_page, true,
					  &page);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} else if (get_zone_number(request) == 0) {
		// Only 1 zone is responsible for updating LRU
		make_page_most_recent(volume->page_cache, page);
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_volume_page_protected(struct volume *volume,
			      Request *request,
			      unsigned int physical_page,
			      enum cache_probe_type probe_type,
			      struct cached_page **page_ptr)
{
	unsigned int zone_number;
	struct cached_page *page = NULL;
	int result =
		get_page_from_cache(volume->page_cache,
				    physical_page,
				    probe_type | CACHE_PROBE_IGNORE_FAILURE,
				    &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	zone_number = get_zone_number(request);
	// If we didn't find a page we need to enqueue a read for it, in which
	// case we need to grab the mutex.
	if (page == NULL) {
		end_pending_search(volume->page_cache, zone_number);
		lock_mutex(&volume->read_threads_mutex);

		/*
		 * Do the lookup again while holding the read mutex (no longer
		 * the fast case so this should be ok to repeat). We need to do
		 * this because an page may have been added to the page map by
		 * the reader thread between the time searched above and the
		 * time we went to actually try to enqueue it below. This could
		 * result in us enqueuing another read for an page which is
		 * already in the cache, which would mean we end up with two
		 * entries in the cache for the same page.
		 */
		result = get_page_from_cache(volume->page_cache, physical_page,
					     probe_type, &page);
		if (result != UDS_SUCCESS) {
			/*
			 * In non-success cases (anything not UDS_SUCCESS,
			 * meaning both UDS_QUEUED and "real" errors), the
			 * caller doesn't get a handle on a cache page, so it
			 * can't continue the search, and we don't need to
			 * prevent other threads from messing with the cache.
			 *
			 * However, we do need to set the "search pending" flag
			 * because the callers expect it to always be set on
			 * return, even if they can't actually do the search.
			 *
			 * Doing the calls in this order ought to be faster,
			 * since we let other threads have the reader thread
			 * mutex (which can require a syscall) ASAP, and set
			 * the "search pending" state that can block the reader
			 * thread as the last thing.
			 */
			unlock_mutex(&volume->read_threads_mutex);
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			return result;
		}

		// If we found the page now, we can release the mutex and
		// proceed as if this were the fast case.
		if (page != NULL) {
			/*
			 * If we found a page (*page_ptr != NULL and return
			 * UDS_SUCCESS), then we're telling the caller where to
			 * look for the cache page, and need to switch to
			 * "reader thread unlocked" and "search pending" state
			 * in careful order so no other thread can mess with
			 * the data before our caller gets to look at it.
			 */
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			unlock_mutex(&volume->read_threads_mutex);
		}
	}

	if (page == NULL) {
		result = read_page_locked(volume, request, physical_page,
					  false, &page);
		if (result != UDS_SUCCESS) {
			/*
			 * This code path is used frequently in the UDS_QUEUED
			 * case, so the performance gain from unlocking first,
			 * while "search pending" mode is off, turns out to be
			 * significant in some cases.
			 */
			unlock_mutex(&volume->read_threads_mutex);
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			return result;
		}

		// See above re: ordering requirement.
		begin_pending_search(volume->page_cache, physical_page,
				     zone_number);
		unlock_mutex(&volume->read_threads_mutex);
	} else {
		if (get_zone_number(request) == 0) {
			// Only 1 zone is responsible for updating LRU
			make_page_most_recent(volume->page_cache, page);
		}
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

/**********************************************************************/
int get_volume_page(struct volume *volume,
		    unsigned int chapter,
		    unsigned int page_number,
		    enum cache_probe_type probe_type,
		    byte **data_ptr,
		    struct delta_index_page **index_page_ptr)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int physical_page =
		map_to_physical_page(volume->geometry, chapter, page_number);

	lock_mutex(&volume->read_threads_mutex);
	result = get_volume_page_locked(volume, NULL, physical_page,
					probe_type, &page);
	unlock_mutex(&volume->read_threads_mutex);

	if (data_ptr != NULL) {
		*data_ptr = (page != NULL) ?
				    get_page_data(&page->cp_page_data) :
				    NULL;
	}
	if (index_page_ptr != NULL) {
		*index_page_ptr = (page != NULL) ? &page->cp_index_page : NULL;
	}
	return result;
}

/**
 * Search for a chunk name in a cached index page or chapter index, returning
 * the record page number from a chapter index match.
 *
 * @param volume              the volume containing the index page to search
 * @param request             the request originating the search (may be NULL
 *                            for a direct query from volume replay)
 * @param name                the name of the block or chunk
 * @param chapter             the chapter to search
 * @param index_page_number   the index page number of the page to search
 * @param record_page_number  pointer to return the chapter record page number
 *                            (value will be NO_CHAPTER_INDEX_ENTRY if the name
 *                            was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_cached_index_page(struct volume *volume,
				    Request *request,
				    const struct uds_chunk_name *name,
				    unsigned int chapter,
				    unsigned int index_page_number,
				    int *record_page_number)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int zone_number = get_zone_number(request);
	unsigned int physical_page = map_to_physical_page(volume->geometry,
							  chapter,
							  index_page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read
	 * from the page map.  This prevents this thread from reading a page in
	 * the page map which has already been marked for invalidation by the
	 * reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume,
					   request,
					   physical_page,
					   cache_probe_type(request, true),
					   &page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	result = ASSERT_LOG_ONLY(search_pending(get_invalidate_counter(volume->page_cache, zone_number)),
						"Search is pending for zone %u",
						zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = search_chapter_index_page(&page->cp_index_page,
					   volume->geometry,
					   name,
					   record_page_number);
	end_pending_search(volume->page_cache, zone_number);
	return result;
}

/**********************************************************************/
int search_cached_record_page(struct volume *volume,
			      Request *request,
			      const struct uds_chunk_name *name,
			      unsigned int chapter,
			      int record_page_number,
			      struct uds_chunk_data *duplicate,
			      bool *found)
{
	struct cached_page *record_page;
	struct geometry *geometry = volume->geometry;
	int physical_page, result;
	unsigned int page_number, zone_number;

	*found = false;

	if (record_page_number == NO_CHAPTER_INDEX_ENTRY) {
		// No record for that name can exist in the chapter.
		return UDS_SUCCESS;
	}

	result = ASSERT(((record_page_number >= 0) &&
			     ((unsigned int) record_page_number <
			      geometry->record_pages_per_chapter)),
			    "0 <= %d <= %u",
			    record_page_number,
			    geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	page_number = geometry->index_pages_per_chapter + record_page_number;

	zone_number = get_zone_number(request);
	physical_page =
		map_to_physical_page(volume->geometry, chapter, page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read
	 * from the page map. This prevents this thread from reading a page in
	 * the page map which has already been marked for invalidation by the
	 * reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume,
					   request,
					   physical_page,
					   cache_probe_type(request, false),
					   &record_page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	if (search_record_page(get_page_data(&record_page->cp_page_data),
			       name,
			       geometry,
			       duplicate)) {
		*found = true;
	}
	end_pending_search(volume->page_cache, zone_number);
	return UDS_SUCCESS;
}

/**********************************************************************/
int read_chapter_index_from_volume(const struct volume *volume,
				   uint64_t virtual_chapter,
				   struct volume_page volume_pages[],
				   struct delta_index_page index_pages[])
{
	int result;
	struct volume_page volume_page;
	unsigned int i;
	const struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter =
		map_to_physical_chapter(geometry, virtual_chapter);
	int physical_page =
		map_to_physical_page(geometry, physical_chapter, 0);
	prefetch_volume_pages(&volume->volume_store,
			      physical_page,
			      geometry->index_pages_per_chapter);

	result = initialize_volume_page(geometry, &volume_page);
	for (i = 0; i < geometry->index_pages_per_chapter; i++) {
		byte *index_page;
		int result = read_volume_page(&volume->volume_store,
					      physical_page + i,
					      &volume_pages[i]);
		if (result != UDS_SUCCESS) {
			break;
		}
		index_page = get_page_data(&volume_pages[i]);
		result = init_chapter_index_page(volume,
						 index_page,
						 physical_chapter,
						 i,
						 &index_pages[i]);
		if (result != UDS_SUCCESS) {
			break;
		}
	}
	destroy_volume_page(&volume_page);
	return result;
}

/**********************************************************************/
int search_volume_page_cache(struct volume *volume,
			     Request *request,
			     const struct uds_chunk_name *name,
			     uint64_t virtual_chapter,
			     struct uds_chunk_data *metadata,
			     bool *found)
{
	unsigned int physical_chapter =
		map_to_physical_chapter(volume->geometry, virtual_chapter);
	unsigned int index_page_number;
	int record_page_number;
	int result = find_index_page_number(volume->index_page_map,
					    name,
					    physical_chapter,
					    &index_page_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = search_cached_index_page(volume,
					  request,
					  name,
					  physical_chapter,
					  index_page_number,
					  &record_page_number);
	if (result == UDS_SUCCESS) {
		result = search_cached_record_page(volume,
						   request,
						   name,
						   physical_chapter,
						   record_page_number,
						   metadata,
						   found);
	}

	return result;
}

/**********************************************************************/
int forget_chapter(struct volume *volume,
		   uint64_t virtual_chapter,
		   enum invalidation_reason reason)
{
	int result;
	unsigned int physical_chapter =
		map_to_physical_chapter(volume->geometry, virtual_chapter);
	log_debug("forgetting chapter %llu", virtual_chapter);
	lock_mutex(&volume->read_threads_mutex);
	result = invalidate_page_cache_for_chapter(volume->page_cache,
						   physical_chapter,
						   volume->geometry->pages_per_chapter,
						   reason);
	unlock_mutex(&volume->read_threads_mutex);
	return result;
}

/**
 * Donate index page data to the page cache for an index page that was just
 * written to the volume.  The caller must already hold the reader thread
 * mutex.
 *
 * @param volume             the volume
 * @param physical_chapter   the physical chapter number of the index page
 * @param index_page_number  the chapter page number of the index page
 * @param scratch_page       the index page data
 **/
static int donate_index_page_locked(struct volume *volume,
				    unsigned int physical_chapter,
				    unsigned int index_page_number,
				    struct volume_page *scratch_page)
{
	unsigned int physical_page = map_to_physical_page(volume->geometry,
							  physical_chapter,
							  index_page_number);

	// Find a place to put the page.
	struct cached_page *page = NULL;
	int result = select_victim_in_cache(volume->page_cache, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Exchange the scratch page with the cache page
	swap_volume_pages(&page->cp_page_data, scratch_page);

	result = init_chapter_index_page(volume,
					 get_page_data(&page->cp_page_data),
					 physical_chapter,
					 index_page_number,
					 &page->cp_index_page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error initialize chapter index page");
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	result = put_page_in_cache(volume->page_cache, physical_page, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", physical_page);
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int write_index_pages(struct volume *volume,
		      int physical_page,
		      struct open_chapter_index *chapter_index,
		      byte **pages)
{
	struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry,
					chapter_index->virtual_chapter_number);
	unsigned int delta_list_number = 0;

	unsigned int index_page_number;
	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		unsigned int lists_packed;
		bool last_page;
		int result =
			prepare_to_write_volume_page(&volume->volume_store,
						     physical_page +
							index_page_number,
						     &volume->scratch_page);
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to prepare index page");
		}

		// Pack as many delta lists into the index page as will fit.
		last_page = ((index_page_number + 1) ==
			     geometry->index_pages_per_chapter);
		result = pack_open_chapter_index_page(chapter_index,
						      get_page_data(&volume->scratch_page),
						      delta_list_number,
						      last_page,
						      &lists_packed);
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to pack index page");
		}

		result = write_volume_page(&volume->volume_store,
					   physical_page + index_page_number,
					   &volume->scratch_page);
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to write chapter index page");
		}

		if (pages != NULL) {
			memcpy(pages[index_page_number],
			       get_page_data(&volume->scratch_page),
			       geometry->bytes_per_page);
		}

		// Tell the index page map the list number of the last delta
		// list that was packed into the index page.
		if (lists_packed == 0) {
			log_debug("no delta lists packed on chapter %u page %u",
				  physical_chapter_number,
				  index_page_number);
		} else {
			delta_list_number += lists_packed;
		}
		result = update_index_page_map(volume->index_page_map,
					       chapter_index->virtual_chapter_number,
					       physical_chapter_number,
					       index_page_number,
					       delta_list_number - 1);
		if (result != UDS_SUCCESS) {
			return log_error_strerror(result,
						  "failed to update index page map");
		}

		// Donate the page data for the index page to the page cache.
		lock_mutex(&volume->read_threads_mutex);
		result = donate_index_page_locked(volume,
						  physical_chapter_number,
						  index_page_number,
						  &volume->scratch_page);
		unlock_mutex(&volume->read_threads_mutex);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int write_record_pages(struct volume *volume,
		       int physical_page,
		       const struct uds_chunk_record records[],
		       byte **pages)
{
	unsigned int record_page_number;
	struct geometry *geometry = volume->geometry;
	// The record array from the open chapter is 1-based.
	const struct uds_chunk_record *next_record = &records[1];
	// Skip over the index pages, which come before the record pages
	physical_page += geometry->index_pages_per_chapter;

	for (record_page_number = 0;
	     record_page_number < geometry->record_pages_per_chapter;
	     record_page_number++) {
		int result =
			prepare_to_write_volume_page(&volume->volume_store,
						     physical_page +
							record_page_number,
						     &volume->scratch_page);
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to prepare record page");
		}

		// Sort the next page of records and copy them to the record
		// page as a binary tree stored in heap order.
		result = encode_record_page(volume, next_record,
					    get_page_data(&volume->scratch_page));
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to encode record page %u",
						    record_page_number);
		}
		next_record += geometry->records_per_page;

		result = write_volume_page(&volume->volume_store,
					   physical_page + record_page_number,
					   &volume->scratch_page);
		if (result != UDS_SUCCESS) {
			return log_warning_strerror(result,
						    "failed to write chapter record page");
		}

		if (pages != NULL) {
			memcpy(pages[record_page_number],
			       get_page_data(&volume->scratch_page),
			       geometry->bytes_per_page);
		}
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int write_chapter(struct volume *volume,
		  struct open_chapter_index *chapter_index,
		  const struct uds_chunk_record records[])
{
	// Determine the position of the virtual chapter in the volume file.
	struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry,
					chapter_index->virtual_chapter_number);
	int physical_page =
		map_to_physical_page(geometry, physical_chapter_number, 0);

	// Pack and write the delta chapter index pages to the volume.
	int result =
		write_index_pages(volume, physical_page, chapter_index, NULL);
	if (result != UDS_SUCCESS) {
		return result;
	}
	// Sort and write the record pages to the volume.
	result = write_record_pages(volume, physical_page, records, NULL);
	if (result != UDS_SUCCESS) {
		return result;
	}
	release_volume_page(&volume->scratch_page);
	// Flush the data to permanent storage.
	return sync_volume_store(&volume->volume_store);
}

/**********************************************************************/
size_t get_cache_size(struct volume *volume)
{
	size_t size = get_page_cache_size(volume->page_cache);
	if (is_sparse(volume->geometry)) {
		size += get_sparse_cache_memory_size(volume->sparse_cache);
	}
	return size;
}

/**********************************************************************/
static int probe_chapter(struct volume *volume,
			 unsigned int chapter_number,
			 uint64_t *virtual_chapter_number)
{
	const struct geometry *geometry = volume->geometry;
	unsigned int expected_list_number = 0;
	unsigned int i;
	uint64_t vcn, last_vcn = UINT64_MAX;

	prefetch_volume_pages(&volume->volume_store,
			      map_to_physical_page(geometry, chapter_number, 0),
			      geometry->index_pages_per_chapter);

	for (i = 0; i < geometry->index_pages_per_chapter; ++i) {
		struct delta_index_page *page;
		int result = get_volume_page(volume,
					     chapter_number,
					     i,
					     CACHE_PROBE_INDEX_FIRST,
					     NULL,
					     &page);
		if (result != UDS_SUCCESS) {
			return result;
		}

		vcn = page->virtual_chapter_number;
		if (last_vcn == UINT64_MAX) {
			last_vcn = vcn;
		} else if (vcn != last_vcn) {
			uds_log_error("inconsistent chapter %u index page %u: expected vcn %llu, got vcn %llu",
				      chapter_number, i, last_vcn, vcn);
			return UDS_CORRUPT_COMPONENT;
		}

		if (expected_list_number != page->lowest_list_number) {
			uds_log_error("inconsistent chapter %u index page %u: expected list number %u, got list number %u",
				      chapter_number, i, expected_list_number,
				      page->lowest_list_number);
			return UDS_CORRUPT_COMPONENT;
		}
		expected_list_number = page->highest_list_number + 1;

		result = validate_chapter_index_page(page, geometry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (last_vcn == UINT64_MAX) {
		uds_log_error("no chapter %u virtual chapter number determined",
			      chapter_number);
		return UDS_CORRUPT_COMPONENT;
	}
	if (chapter_number != map_to_physical_chapter(geometry, last_vcn)) {
		uds_log_error("chapter %u vcn %llu is out of phase (%u)",
			      chapter_number,
			      last_vcn,
			      geometry->chapters_per_volume);
		return UDS_CORRUPT_COMPONENT;
	}
	*virtual_chapter_number = last_vcn;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int probe_wrapper(void *aux,
			 unsigned int chapter_number,
			 uint64_t *virtual_chapter_number)
{
	struct volume *volume = aux;
	int result =
		probe_chapter(volume, chapter_number, virtual_chapter_number);
	if ((result == UDS_CORRUPT_COMPONENT) ||
	    (result == UDS_CORRUPT_DATA)) {
		*virtual_chapter_number = UINT64_MAX;
		return UDS_SUCCESS;
	}
	return result;
}

/**********************************************************************/
static int find_real_end_of_volume(struct volume *volume,
				   unsigned int limit,
				   unsigned int *limit_ptr)
{
	/*
	 * Start checking from the end of the volume. As long as we hit corrupt
	 * data, start skipping larger and larger amounts until we find real
	 * data. If we find real data, reduce the span and try again until we
	 * find the exact boundary.
	 */
	unsigned int span = 1;
	unsigned int tries = 0;
	while (limit > 0) {
		unsigned int chapter = (span > limit) ? 0 : limit - span;
		uint64_t vcn = 0;
		int result = probe_chapter(volume, chapter, &vcn);
		if (result == UDS_SUCCESS) {
			if (span == 1) {
				break;
			}
			span /= 2;
			tries = 0;
		} else if (result == UDS_CORRUPT_COMPONENT) {
			limit = chapter;
			if (++tries > 1) {
				span *= 2;
			}
		} else {
			return log_error_strerror(result,
						  "cannot determine end of volume");
		}
	}

	if (limit_ptr != NULL) {
		*limit_ptr = limit;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int find_volume_chapter_boundaries(struct volume *volume,
				   uint64_t *lowest_vcn,
				   uint64_t *highest_vcn,
				   bool *is_empty)
{
	unsigned int chapter_limit = volume->geometry->chapters_per_volume;

	int result =
		find_real_end_of_volume(volume, chapter_limit, &chapter_limit);
	if (result != UDS_SUCCESS) {
		return log_error_strerror(result,
					  "cannot find end of volume");
	}

	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		*is_empty = true;
		return UDS_SUCCESS;
	}

	*is_empty = false;
	return find_volume_chapter_boundaries_impl(chapter_limit,
						   MAX_BAD_CHAPTERS,
						   lowest_vcn,
						   highest_vcn,
						   probe_wrapper,
						   volume);
}

/**********************************************************************/
int find_volume_chapter_boundaries_impl(unsigned int chapter_limit,
					unsigned int max_bad_chapters,
					uint64_t *lowest_vcn,
					uint64_t *highest_vcn,
					int (*probe_func)(void *aux,
							  unsigned int chapter,
							  uint64_t *vcn),
					void *aux)
{
	uint64_t first_vcn = UINT64_MAX, lowest = UINT64_MAX;
	uint64_t highest = UINT64_MAX;

	unsigned int left_chapter, right_chapter, bad_chapters = 0;
	int result;

	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		return UDS_SUCCESS;
	}

	/*
	 * This method assumes there is at most one run of contiguous bad
	 * chapters caused by unflushed writes. Either the bad spot is at the
	 * beginning and end, or somewhere in the middle. Wherever it is, the
	 * highest and lowest VCNs are adjacent to it. Otherwise the volume is
	 * cleanly saved and somewhere in the middle of it the highest VCN
	 * immediately preceeds the lowest one.
	 */

	// doesn't matter if this results in a bad spot (UINT64_MAX)
	result = (*probe_func)(aux, 0, &first_vcn);
	if (result != UDS_SUCCESS) {
		return UDS_SUCCESS;
	}

	/*
	 * Binary search for end of the discontinuity in the monotonically
	 * increasing virtual chapter numbers; bad spots are treated as a span
	 * of UINT64_MAX values. In effect we're searching for the index of the
	 * smallest value less than first_vcn. In the case we go off the end it
	 * means that chapter 0 has the lowest vcn.
	 */

	left_chapter = 0;
	right_chapter = chapter_limit;

	while (left_chapter < right_chapter) {
		unsigned int chapter = (left_chapter + right_chapter) / 2;
		uint64_t probe_vcn;

		result = (*probe_func)(aux, chapter, &probe_vcn);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (first_vcn <= probe_vcn) {
			left_chapter = chapter + 1;
		} else {
			right_chapter = chapter;
		}
	}

	result = ASSERT(left_chapter == right_chapter,
			"left_chapter == right_chapter");
	if (result != UDS_SUCCESS) {
		return result;
	}

	left_chapter %= chapter_limit; // in case we're at the end

	// At this point, left_chapter is the chapter with the lowest virtual
	// chapter number.

	result = (*probe_func)(aux, left_chapter, &lowest);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((lowest != UINT64_MAX), "invalid lowest chapter");
	if (result != UDS_SUCCESS) {
		return result;
	}

	// We now circularly scan backwards, moving over any bad chapters until
	// we find the chapter with the highest vcn (the first good chapter we
	// encounter).

	for (;;) {
		right_chapter =
			(right_chapter + chapter_limit - 1) % chapter_limit;
		result = (*probe_func)(aux, right_chapter, &highest);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (highest != UINT64_MAX) {
			break;
		}
		if (++bad_chapters >= max_bad_chapters) {
			uds_log_error("too many bad chapters in volume: %u",
				      bad_chapters);
			return UDS_CORRUPT_COMPONENT;
		}
	}

	*lowest_vcn = lowest;
	*highest_vcn = highest;
	return UDS_SUCCESS;
}

/**
 * Allocate a volume.
 *
 * @param config               The configuration to use
 * @param layout               The index layout
 * @param read_queue_max_size  The maximum size of the read queue
 * @param zone_count           The number of zones to use
 * @param new_volume           A pointer to hold the new volume
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check allocate_volume(const struct configuration *config,
					struct index_layout *layout,
					unsigned int read_queue_max_size,
					unsigned int zone_count,
					struct volume **new_volume)
{
	struct volume *volume;
	unsigned int reserved_buffers;
	int result = ALLOCATE(1, struct volume, "volume", &volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	volume->nonce = get_volume_nonce(layout);
	// It is safe to call free_volume now to clean up and close the volume

	result = copy_geometry(config->geometry, &volume->geometry);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return log_warning_strerror(result,
					    "failed to allocate geometry: error");
	}

	// Need a buffer for each entry in the page cache
	reserved_buffers = config->cache_chapters *
		config->geometry->record_pages_per_chapter;
	// And a buffer for the chapter writer
	reserved_buffers += 1;
	// And a buffer for each entry in the sparse cache
	if (is_sparse(volume->geometry)) {
		reserved_buffers += config->cache_chapters *
				    config->geometry->index_pages_per_chapter;
	}
	result = open_volume_store(&volume->volume_store,
				   layout,
				   reserved_buffers,
				   config->geometry->bytes_per_page);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}
	result = initialize_volume_page(config->geometry,
					&volume->scratch_page);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = make_radix_sorter(config->geometry->records_per_page,
				   &volume->radix_sorter);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = ALLOCATE(config->geometry->records_per_page,
			  const struct uds_chunk_record *,
			  "record pointers",
			  &volume->record_pointers);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	if (is_sparse(volume->geometry)) {
		result = make_sparse_cache(volume->geometry,
					   config->cache_chapters,
					   zone_count,
					   &volume->sparse_cache);
		if (result != UDS_SUCCESS) {
			free_volume(volume);
			return result;
		}
	}
	result = make_page_cache(volume->geometry,
				 config->cache_chapters,
				 read_queue_max_size,
				 zone_count,
				 &volume->page_cache);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}
	result =
		make_index_page_map(volume->geometry, &volume->index_page_map);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	*new_volume = volume;
	return UDS_SUCCESS;
}

/**********************************************************************/
int make_volume(const struct configuration *config,
		struct index_layout *layout,
		const struct uds_parameters *user_params,
		unsigned int read_queue_max_size,
		unsigned int zone_count,
		struct volume **new_volume)
{
	unsigned int i;
	unsigned int volume_read_threads = get_read_threads(user_params);
	struct volume *volume = NULL;
	int result;

	if (read_queue_max_size <= volume_read_threads) {
		uds_log_error("Number of read threads must be smaller than read queue");
		return UDS_INVALID_ARGUMENT;
	}

	result = allocate_volume(config, layout, read_queue_max_size,
				     zone_count, &volume);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = init_mutex(&volume->read_threads_mutex);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}
	result = init_cond(&volume->read_threads_read_done_cond);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}
	result = init_cond(&volume->read_threads_cond);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	// Start the reader threads.  If this allocation succeeds, free_volume
	// knows that it needs to try and stop those threads.
	result = ALLOCATE(volume_read_threads,
			  struct thread *,
			  "reader threads",
			  &volume->reader_threads);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}
	for (i = 0; i < volume_read_threads; i++) {
		result = create_thread(read_thread_function,
				       (void *) volume,
				       "reader",
				       &volume->reader_threads[i]);
		if (result != UDS_SUCCESS) {
			free_volume(volume);
			return result;
		}
		// We only stop as many threads as actually got started.
		volume->num_read_threads = i + 1;
	}

	*new_volume = volume;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_volume(struct volume *volume)
{
	if (volume == NULL) {
		return;
	}

	// If reader_threads is NULL, then we haven't set up the reader
	// threads.
	if (volume->reader_threads != NULL) {
		unsigned int i;
		// Stop the reader threads.  It is ok if there aren't any of
		// them.
		lock_mutex(&volume->read_threads_mutex);
		volume->reader_state |= READER_STATE_EXIT;
		broadcast_cond(&volume->read_threads_cond);
		unlock_mutex(&volume->read_threads_mutex);
		for (i = 0; i < volume->num_read_threads; i++) {
			join_threads(volume->reader_threads[i]);
		}
		FREE(volume->reader_threads);
		volume->reader_threads = NULL;
	}

	// Must close the volume store AFTER freeing the scratch page and the
	// caches
	destroy_volume_page(&volume->scratch_page);
	free_page_cache(volume->page_cache);
	free_sparse_cache(volume->sparse_cache);
	close_volume_store(&volume->volume_store);

	destroy_cond(&volume->read_threads_cond);
	destroy_cond(&volume->read_threads_read_done_cond);
	destroy_mutex(&volume->read_threads_mutex);
	free_index_page_map(volume->index_page_map);
	free_radix_sorter(volume->radix_sorter);
	FREE(volume->geometry);
	FREE(volume->record_pointers);
	FREE(volume);
}
