/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUME_H
#define VOLUME_H

#include "common.h"
#include "config.h"
#include "chapter-index.h"
#include "index-layout.h"
#include "index-page-map.h"
#include "page-cache.h"
#include "radix-sort.h"
#include "request.h"
#include "sparse-cache.h"
#include "uds.h"
#include "volume-store.h"

enum reader_state {
	READER_STATE_RUN = 1,
	READER_STATE_EXIT = 2,
	READER_STATE_STOP = 4
};

enum index_lookup_mode {
	/* Always do lookups in all chapters normally.  */
	LOOKUP_NORMAL,
	/* Only do a subset of lookups needed when rebuilding an index. */
	LOOKUP_FOR_REBUILD,
};

struct volume {
	/* The layout of the volume */
	struct geometry *geometry;
	/* The access to the volume's backing store */
	struct volume_store volume_store;
	/* A single page used for writing to the volume */
	struct volume_page scratch_page;
	/* The nonce used to save the volume */
	uint64_t nonce;
	/* A single page's records, for sorting */
	const struct uds_chunk_record **record_pointers;
	/* For sorting record pages */
	struct radix_sorter *radix_sorter;
	/* The sparse chapter index cache */
	struct sparse_cache *sparse_cache;
	/* The page cache */
	struct page_cache *page_cache;
	/* The index page map maps delta list numbers to index page numbers */
	struct index_page_map *index_page_map;
	/* mutex to sync between read threads and index thread */
	struct mutex read_threads_mutex;
	/* cond_var to indicate when read threads should start working */
	struct cond_var read_threads_cond;
	/* cond_var to indicate when a read thread has finished a read */
	struct cond_var read_threads_read_done_cond;
	/* Threads to read data from disk */
	struct thread **reader_threads;
	/* Number of threads busy with reads */
	unsigned int busy_reader_threads;
	/* The state of the reader threads */
	enum reader_state reader_state;
	/* The lookup mode for the index */
	enum index_lookup_mode lookup_mode;
	/* Number of read threads to use (run-time parameter) */
	unsigned int num_read_threads;
	/* Number of reserved buffers for the volume store */
	unsigned int reserved_buffers;
};

/**
 * Create a volume.
 *
 * @param config      The configuration to use.
 * @param layout      The index layout
 * @param new_volume  A pointer to hold a pointer to the new volume.
 *
 * @return          UDS_SUCCESS or an error code
 **/
int __must_check make_volume(const struct configuration *config,
			     struct index_layout *layout,
			     struct volume **new_volume);

/**
 * Clean up a volume and its memory.
 *
 * @param volume  The volume to destroy.
 **/
void free_volume(struct volume *volume);

/**
 * Replace the backing storage for a volume.
 *
 * @param volume  The volume to reconfigure
 * @param layout  The index layout
 * @param path    The path to the new backing store
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check replace_volume_storage(struct volume *volume,
					struct index_layout *layout,
					const char *path);

/**
 * Enqueue a page read.
 *
 * @param volume         the volume
 * @param request        the request to waiting on the read
 * @param physical_page  the page number to read
 *
 * @return UDS_QUEUED if successful, or an error code
 **/
int __must_check enqueue_page_read(struct volume *volume,
				   struct uds_request *request,
				   int physical_page);

/**
 * Find the lowest and highest contiguous chapters and determine their
 * virtual chapter numbers.
 *
 * @param [in]  volume       The volume to probe.
 * @param [out] lowest_vcn   Pointer for lowest virtual chapter number.
 * @param [out] highest_vcn  Pointer for highest virtual chapter number.
 * @param [out] is_empty     Pointer to a bool indicating whether or not the
 *                           volume is empty.
 *
 * @return              UDS_SUCCESS, or an error code.
 *
 * @note        This routine does something similar to a binary search to find
 *              the location in the volume file where the discontinuity of
 *              chapter numbers occurs.  In a good save, the discontinuity is
 *              a sharp cliff, but if write failures occured during saving
 *              there may be one or more chapters which are partially written.
 *
 * @note        This method takes advantage of the fact that the physical
 *              chapter number in which the index pages are found should have
 *              headers which state that the virtual chapter number are all
 *              identical and maintain the invariant that
 *              pcn == vcn % chapters_per_volume.
 **/
int __must_check find_volume_chapter_boundaries(struct volume *volume,
						uint64_t *lowest_vcn,
						uint64_t *highest_vcn,
						bool *is_empty);

/**
 * Find any matching metadata for the given name within a given physical
 * chapter.
 *
 * @param volume           The volume.
 * @param request          The request originating the search.
 * @param name             The block name of interest.
 * @param virtual_chapter  The number of the chapter to search.
 * @param metadata         The old metadata for the name.
 * @param found            A pointer which will be set to
 *                         <code>true</code> if a match was found.
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check search_volume_page_cache(struct volume *volume,
					  struct uds_request *request,
					  const struct uds_chunk_name *name,
					  uint64_t virtual_chapter,
					  struct uds_chunk_data *metadata,
					  bool *found);

/**
 * Fetch a record page from the cache or read it from the volume and search it
 * for a chunk name.
 *
 * If a match is found, optionally returns the metadata from the stored
 * record. If the requested record page is not cached, the page fetch may be
 * asynchronously completed on the slow lane, in which case UDS_QUEUED will be
 * returned and the request will be requeued for continued processing after
 * the page is read and added to the cache.
 *
 * @param volume              the volume containing the record page to search
 * @param request             the request originating the search (may be NULL
 *                            for a direct query from volume replay)
 * @param name                the name of the block or chunk
 * @param chapter             the chapter to search
 * @param record_page_number  the record page number of the page to search
 * @param duplicate           an array in which to place the metadata of the
 *                            duplicate, if one was found
 * @param found               a (bool *) which will be set to true if the chunk
 *                            was found
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int __must_check search_cached_record_page(struct volume *volume,
					   struct uds_request *request,
					   const struct uds_chunk_name *name,
					   unsigned int chapter,
					   int record_page_number,
					   struct uds_chunk_data *duplicate,
					   bool *found);

/**
 * Forget the contents of a chapter. Invalidates any cached state for the
 * specified chapter.
 *
 * @param volume   the volume containing the chapter
 * @param chapter  the virtual chapter number
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check forget_chapter(struct volume *volume, uint64_t chapter);

/**
 * Write a chapter's worth of index pages to a volume
 *
 * @param volume         the volume containing the chapter
 * @param physical_page  the page number in the volume for the chapter
 * @param chapter_index  the populated delta chapter index
 * @param pages          pointer to array of page pointers. Used only in
 *                       testing to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_index_pages(struct volume *volume,
				   int physical_page,
				   struct open_chapter_index *chapter_index,
				   byte **pages);

/**
 * Write a chapter's worth of record pages to a volume
 *
 * @param volume         the volume containing the chapter
 * @param physical_page  the page number in the volume for the chapter
 * @param records        a 1-based array of chunk records in the chapter
 * @param pages          pointer to array of page pointers. Used only in
 *                       testing to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_record_pages(struct volume *volume,
				    int physical_page,
				    const struct uds_chunk_record records[],
				    byte **pages);

/**
 * Write the index and records from the most recently filled chapter to the
 * volume.
 *
 * @param volume                the volume containing the chapter
 * @param chapter_index         the populated delta chapter index
 * @param records               a 1-based array of chunk records in the chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_chapter(struct volume *volume,
			       struct open_chapter_index *chapter_index,
			       const struct uds_chunk_record records[]);

/**
 * Read all the index pages for a chapter from the volume and initialize an
 * array of ChapterIndexPages to represent them.
 *
 * @param [in]  volume           the volume containing the chapter
 * @param [in]  virtual_chapter  the virtual chapter number of the index to
 *                               read
 * @param [out] volume_pages     an array to receive the raw index page data
 * @param [out] index_pages      an array of ChapterIndexPages to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
read_chapter_index_from_volume(const struct volume *volume,
			       uint64_t virtual_chapter,
			       struct volume_page volume_pages[],
			       struct delta_index_page index_pages[]);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, this is done immediately in the same thread and the
 * page is then returned.
 *
 * The caller of this function must be holding the volume read mutex before
 * calling this function.
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function is only exposed for the use of unit tests.
 *
 * @param volume         The volume containing the page
 * @param physical_page  The physical page number
 * @param entry_ptr      A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_page_locked(struct volume *volume,
					unsigned int physical_page,
					struct cached_page **entry_ptr);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, the read request is enqueued for later processing
 * by another thread. When that thread finally reads the page into the cache,
 * a callback function is called to inform the caller the read is complete.
 *
 * The caller of this function should not be holding the volume read lock.
 * Instead, the caller must call begin_pending_search() for the given zone
 * the request is being processed in. That state will be maintained or
 * restored when the call returns, at which point the caller should call
 * end_pending_search().
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function is only exposed for the use of unit tests.
 *
 * @param volume         The volume containing the page
 * @param request        The request originating the search
 * @param physical_page  The physical page number
 * @param entry_ptr      A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_page_protected(struct volume *volume,
					   struct uds_request *request,
					   unsigned int physical_page,
					   struct cached_page **entry_ptr);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, this is done immediately in the same thread and the
 * page is then returned.
 *
 * The caller of this function must not be holding the volume read lock before
 * calling this function. This method will grab that lock and release it
 * when it returns.
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function should only be called by areas of the code that do not use
 * multi-threading to access the volume. These include rebuild, volume
 * explorer, and certain unit tests.
 *
 * @param volume          The volume containing the page
 * @param chapter         The number of the chapter containing the page
 * @param page_number     The number of the page
 * @param data_ptr        Pointer to hold the retrieved page, NULL if not
 *                        wanted
 * @param index_page_ptr  Pointer to hold the retrieved chapter index page, or
 *                        NULL if not wanted
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_page(struct volume *volume,
				 unsigned int chapter,
				 unsigned int page_number,
				 byte **data_ptr,
				 struct delta_index_page **index_page_ptr);

size_t __must_check get_cache_size(struct volume *volume);

int __must_check
find_volume_chapter_boundaries_impl(unsigned int chapter_limit,
				    unsigned int max_bad_chapters,
				    uint64_t *lowest_vcn,
				    uint64_t *highest_vcn,
				    int (*probe_func)(void *aux,
						      unsigned int chapter,
						      uint64_t *vcn),
				    struct geometry *geometry,
				    void *aux);

/**
 * Map a chapter number and page number to a phsical volume page number.
 *
 * @param geometry the layout of the volume
 * @param chapter  the chapter number of the desired page
 * @param page     the chapter page number of the desired page
 *
 * @return the physical page number
 **/
int __must_check map_to_physical_page(const struct geometry *geometry,
				      int chapter,
				      int page);

#endif /* VOLUME_H */
