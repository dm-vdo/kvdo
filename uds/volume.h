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
 * $Id: //eng/uds-releases/krusty/src/uds/volume.h#9 $
 */

#ifndef VOLUME_H
#define VOLUME_H

#include "cacheCounters.h"
#include "common.h"
#include "chapterIndex.h"
#include "indexConfig.h"
#include "indexLayout.h"
#include "indexPageMap.h"
#include "pageCache.h"
#include "request.h"
#include "sparseCache.h"
#include "uds.h"
#include "util/radixSort.h"
#include "volumeStore.h"

typedef enum {
  READER_STATE_RUN   = 1,
  READER_STATE_EXIT  = 2,
  READER_STATE_STOP  = 4
} ReaderState;

typedef enum indexLookupMode {
  /* Always do lookups in all chapters normally.  */
  LOOKUP_NORMAL,
  /*
   * Don't do lookups in closed chapters; assume records not in the
   * open chapter are always new.  You don't want this normally; it's
   * for programs like albfill.  (Even then, with multiple runs using
   * the same tag, we may actually duplicate older records, but if
   * it's in a separate chapter it won't really matter.)
   */
  LOOKUP_CURRENT_CHAPTER_ONLY,
  /*
   * Only do a subset of lookups needed when rebuilding an index.
   * This cannot be set externally.
   */
  LOOKUP_FOR_REBUILD
} IndexLookupMode;

typedef struct volume {
  /* The layout of the volume */
  Geometry                       *geometry;
  /* The configuration of the volume */
  struct configuration           *config;
  /* The access to the volume's backing store */
  struct volume_store             volumeStore;
  /* A single page used for writing to the volume */
  struct volume_page              scratchPage;
  /* The nonce used to save the volume */
  uint64_t                        nonce;
  /* A single page's records, for sorting */
  const struct uds_chunk_record **recordPointers;
  /* For sorting record pages */
  struct radix_sorter            *radixSorter;
  /* The sparse chapter index cache */
  SparseCache                    *sparseCache;
  /* The page cache */
  PageCache                      *pageCache;
  /* The index page map maps delta list numbers to index page numbers */
  IndexPageMap                   *indexPageMap;
  /* Mutex to sync between read threads and index thread */
  Mutex                           readThreadsMutex;
  /* Condvar to indicate when read threads should start working */
  CondVar                         readThreadsCond;
  /* Condvar to indicate when a read thread has finished a read */
  CondVar                         readThreadsReadDoneCond;
  /* Threads to read data from disk */
  Thread                         *readerThreads;
  /* Number of threads busy with reads */
  unsigned int                    busyReaderThreads;
  /* The state of the reader threads */
  ReaderState                     readerState;
  /* The lookup mode for the index */
  IndexLookupMode                 lookupMode;
  /* Number of read threads to use (run-time parameter) */
  unsigned int                    numReadThreads;
} Volume;

/**
 * Create a volume.
 *
 * @param config            The configuration to use.
 * @param layout            The index layout
 * @param userParams        The index session parameters.  If NULL, the default
 *                          session parameters will be used.
 * @param readQueueMaxSize  The maximum size of the read queue.
 * @param zoneCount         The number of zones to use.
 * @param newVolume         A pointer to hold a pointer to the new volume.
 *
 * @return          UDS_SUCCESS or an error code
 **/
int __must_check makeVolume(const struct configuration *config,
			    struct index_layout *layout,
			    const struct uds_parameters *userParams,
			    unsigned int readQueueMaxSize,
			    unsigned int zoneCount,
			    Volume **newVolume);

/**
 * Clean up a volume and its memory.
 *
 * @param volume  The volume to destroy.
 **/
void freeVolume(Volume *volume);

/**
 * Enqueue a page read.
 *
 * @param volume       the volume
 * @param request      the request to waiting on the read
 * @param physicalPage the page number to read
 *
 * @return UDS_QUEUED if successful, or an error code
 **/
int __must_check
enqueuePageRead(Volume *volume, Request *request, int physicalPage);

/**
 * Find the lowest and highest contiguous chapters and determine their
 * virtual chapter numbers.
 *
 * @param [in]  volume      The volume to probe.
 * @param [out] lowestVCN   Pointer for lowest virtual chapter number.
 * @param [out] highestVCN  Pointer for highest virtual chapter number.
 * @param [out] isEmpty     Pointer to a bool indicating whether or not the
 *                          volume is empty.
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
 *              pcn == vcn % chaptersPerVolume.
 **/
int __must_check
findVolumeChapterBoundaries(Volume *volume,
			    uint64_t *lowestVCN,
			    uint64_t *highestVCN,
			    bool *isEmpty);

/**
 * Find any matching metadata for the given name within a given physical
 * chapter.
 *
 * @param volume          The volume.
 * @param request         The request originating the search.
 * @param name            The block name of interest.
 * @param virtualChapter  The number of the chapter to search.
 * @param metadata        The old metadata for the name.
 * @param found           A pointer which will be set to
 *                        <code>true</code> if a match was found.
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check searchVolumePageCache(Volume *volume,
				       Request *request,
				       const struct uds_chunk_name *name,
				       uint64_t virtualChapter,
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
 * @param volume           the volume containing the record page to search
 * @param request          the request originating the search (may be NULL for
 *                         a direct query from volume replay)
 * @param name             the name of the block or chunk
 * @param chapter          the chapter to search
 * @param recordPageNumber the record page number of the page to search
 * @param duplicate        an array in which to place the metadata of the
 *                         duplicate, if one was found
 * @param found            a (bool *) which will be set to true if the chunk
 *                         was found
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int __must_check searchCachedRecordPage(Volume *volume,
					Request *request,
					const struct uds_chunk_name *name,
					unsigned int chapter,
					int recordPageNumber,
					struct uds_chunk_data *duplicate,
					bool *found);

/**
 * Forget the contents of a chapter. Invalidates any cached state for the
 * specified chapter.
 *
 * @param volume   the volume containing the chapter
 * @param chapter  the virtual chapter number
 * @param reason   the reason for invalidation
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
forgetChapter(Volume *volume, uint64_t chapter, InvalidationReason reason);

/**
 * Write a chapter's worth of index pages to a volume
 *
 * @param volume        the volume containing the chapter
 * @param physicalPage  the page number in the volume for the chapter
 * @param chapterIndex  the populated delta chapter index
 * @param pages         pointer to array of page pointers. Used only in testing
 *                      to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check writeIndexPages(Volume *volume,
				 int physicalPage,
				 struct open_chapter_index *chapterIndex,
				 byte **pages);

/**
 * Write a chapter's worth of record pages to a volume
 *
 * @param volume        the volume containing the chapter
 * @param physicalPage  the page number in the volume for the chapter
 * @param records       a 1-based array of chunk records in the chapter
 * @param pages         pointer to array of page pointers. Used only in testing
 *                      to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check writeRecordPages(Volume *volume,
				  int physicalPage,
				  const struct uds_chunk_record records[],
				  byte **pages);

/**
 * Write the index and records from the most recently filled chapter to the
 * volume.
 *
 * @param volume                the volume containing the chapter
 * @param chapterIndex          the populated delta chapter index
 * @param records               a 1-based array of chunk records in the chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check writeChapter(Volume *volume,
			      struct open_chapter_index *chapterIndex,
			      const struct uds_chunk_record records[]);

/**
 * Read all the index pages for a chapter from the volume and initialize an
 * array of ChapterIndexPages to represent them.
 *
 * @param [in]  volume          the volume containing the chapter
 * @param [in]  virtualChapter  the virtual chapter number of the index to read
 * @param [out] volumePages     an array to receive the raw index page data
 * @param [out] indexPages      an array of ChapterIndexPages to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
readChapterIndexFromVolume(const Volume *volume,
			   uint64_t virtualChapter,
			   struct volume_page volumePages[],
			   struct delta_index_page indexPages[]);

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
 * @param volume       The volume containing the page
 * @param request      The request originating the search
 * @param physicalPage The physical page number
 * @param probeType    The type of cache access being done
 * @param entryPtr     A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check getPageLocked(Volume *volume,
			       Request *request,
			       unsigned int physicalPage,
			       CacheProbeType probeType,
			       CachedPage **entryPtr);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, the read request is enqueued for later processing
 * by another thread. When that thread finally reads the page into the cache,
 * a callback function is called to inform the caller the read is complete.
 *
 * The caller of this function should not be holding the volume read lock.
 * Instead, the caller must call beingPendingSearch() for the given zone
 * the request is being processed in. That state will be maintained or
 * restored when the call returns, at which point the caller should call
 * endPendingSearch().
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function is only exposed for the use of unit tests.
 *
 * @param volume       The volume containing the page
 * @param request      The request originating the search
 * @param physicalPage The physical page number
 * @param probeType    The type of cache access being done
 * @param entryPtr     A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check getPageProtected(Volume *volume,
				  Request *request,
				  unsigned int physicalPage,
				  CacheProbeType probeType,
				  CachedPage **entryPtr);

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
 * @param volume        The volume containing the page
 * @param chapter       The number of the chapter containing the page
 * @param pageNumber    The number of the page
 * @param probeType     The type of cache access being done
 * @param dataPtr       Pointer to hold the retrieved page, NULL if not wanted
 * @param indexPagePtr  Pointer to hold the retrieved chapter index page, or
 *                      NULL if not wanted
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check getPage(Volume *volume,
			 unsigned int chapter,
			 unsigned int pageNumber,
			 CacheProbeType probeType,
			 byte **dataPtr,
			 struct delta_index_page **indexPagePtr);

/**********************************************************************/
size_t __must_check getCacheSize(Volume *volume);

/**********************************************************************/
int __must_check
findVolumeChapterBoundariesImpl(unsigned int  chapterLimit,
                                unsigned int  maxBadChapters,
                                uint64_t     *lowestVCN,
                                uint64_t     *highestVCN,
                                int (*probeFunc)(void         *aux,
                                                 unsigned int  chapter,
                                                 uint64_t     *vcn),
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
int __must_check
mapToPhysicalPage(const Geometry *geometry, int chapter, int page);

#endif /* VOLUME_H */
