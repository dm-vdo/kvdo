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
 * $Id: //eng/uds-releases/gloria/src/uds/chapterIndex.h#1 $
 */

#ifndef CHAPTER_INDEX_H
#define CHAPTER_INDEX_H 1

#include "deltaIndex.h"
#include "geometry.h"

enum {
  // The value returned as the record page number when an entry is not found
  // in the chapter index.
  NO_CHAPTER_INDEX_ENTRY  = -1
};

typedef struct openChapterIndex {
  const Geometry *geometry;
  DeltaIndex      deltaIndex;
  uint64_t        virtualChapterNumber;
} OpenChapterIndex;

typedef struct chapterIndexPage {
  DeltaIndex  deltaIndex;
  DeltaMemory deltaMemory;
} ChapterIndexPage;


/**
 * Make a new open chapter index.
 *
 * @param openChapterIndex  Location to hold new open chapter index pointer
 * @param geometry          The geometry
 *
 * @return error code or UDS_SUCCESS
 **/
int makeOpenChapterIndex(OpenChapterIndex **openChapterIndex,
                         const Geometry    *geometry)
  __attribute__((warn_unused_result));

/**
 * Terminate and clean up an open chapter index.
 *
 * @param openChapterIndex  The open chapter index to terminate
 **/
void freeOpenChapterIndex(OpenChapterIndex *openChapterIndex);

/**
 * Empty an open chapter index, and prepare it for writing a new virtual
 * chapter.
 *
 * @param openChapterIndex      The open chapter index to empty
 * @param virtualChapterNumber  The virtual chapter number
 **/
void emptyOpenChapterIndex(OpenChapterIndex *openChapterIndex,
                           uint64_t          virtualChapterNumber);

/**
 * Create a new record in an open chapter index, associating a chunk name with
 * the number of the record page containing the metadata for the chunk.
 *
 * @param openChapterIndex  The open chapter index
 * @param name              The chunk name
 * @param pageNumber        The number of the record page containing the name
 *
 * @return UDS_SUCCESS or an error code
 **/
int putOpenChapterIndexRecord(OpenChapterIndex   *openChapterIndex,
                              const UdsChunkName *name,
                              unsigned int        pageNumber)
  __attribute__((warn_unused_result));

/**
 * Pack a section of an open chapter index into a chapter index page.  A
 * range of delta lists (starting with a specified list index) is copied
 * from the open chapter index into a memory page.  The number of lists
 * copied onto the page is returned to the caller.
 *
 * @param openChapterIndex  The open chapter index
 * @param volumeNonce       The nonce value used to authenticate the volume
 * @param memory            The memory page to use
 * @param firstList         The first delta list number to be copied
 * @param lastPage          If true, this is the last page of the chapter
 *                          index and all the remaining lists must be packed
 *                          onto this page
 * @param numLists          The number of delta lists that were copied
 *
 * @return error code or UDS_SUCCESS.  On UDS_SUCCESS, the numLists
 *         argument contains the number of lists copied.
 **/
int packOpenChapterIndexPage(OpenChapterIndex *openChapterIndex,
                             uint64_t          volumeNonce,
                             byte             *memory,
                             unsigned int      firstList,
                             bool              lastPage,
                             unsigned int     *numLists)
  __attribute__((warn_unused_result));

/**
 * Get the number of records in an open chapter index.
 *
 * @param openChapterIndex  The open chapter index
 *
 * @return The number of records
 **/
int getOpenChapterIndexSize(OpenChapterIndex *openChapterIndex)
  __attribute__((warn_unused_result));

/**
 * Get the number of bytes allocated for the open chapter index.
 *
 * @param openChapterIndex  The open chapter index
 *
 * @return the number of bytes allocated
 **/
size_t getOpenChapterIndexMemoryAllocated(OpenChapterIndex *openChapterIndex);

/**
 * Make a new chapter index page, initializing it with the data from the
 * given buffer.
 *
 * @param chapterIndexPage  The new chapter index page
 * @param geometry          The geometry
 * @param indexPage         The memory page to use
 * @param volumeNonce       If non-zero, the volume nonce to verify
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeChapterIndexPage(ChapterIndexPage *chapterIndexPage,
                               const Geometry   *geometry,
                               byte             *indexPage,
                               uint64_t          volumeNonce)
  __attribute__((warn_unused_result));

/**
 * Validate a chapter index page.  This is called at rebuild time to ensure
 * that the volume file contains a coherent chapter index.
 *
 * @param chapterIndexPage  The chapter index page
 * @param geometry          The geometry of the volume
 *
 * @return The result code:
 *         UDS_SUCCESS for a good chapter index page
 *         UDS_CORRUPT_COMPONENT if the chapter index code detects invalid data
 *         UDS_CORRUPT_DATA if there is a problem in a delta list bit stream
 *         UDS_BAD_STATE if the code follows an invalid code path
 **/
int validateChapterIndexPage(const ChapterIndexPage *chapterIndexPage,
                             const Geometry         *geometry)
  __attribute__((warn_unused_result));

/**
 * Search a chapter index page for a chunk name, returning the record page
 * number that may contain the name.
 *
 * @param [in]  chapterIndexPage    The chapter index page
 * @param [in]  geometry            The geometry of the volume
 * @param [in]  name                The chunk name
 * @param [out] recordPagePtr       The record page number
 *                                  or NO_CHAPTER_INDEX_ENTRY if not found
 *
 * @return UDS_SUCCESS or an error code
 **/
int searchChapterIndexPage(ChapterIndexPage   *chapterIndexPage,
                           const Geometry     *geometry,
                           const UdsChunkName *name,
                           int                *recordPagePtr)
  __attribute__((warn_unused_result));

/**
 * Get the virtual chapter number from an immutable chapter index page.
 *
 * @param page  The chapter index page
 *
 * @return the virtual chapter number
 **/
uint64_t getChapterIndexVirtualChapterNumber(const ChapterIndexPage *page);

/**
 * Get the lowest numbered delta list for the given immutable chapter index
 * page.
 *
 * @param page  The chapter index page
 *
 * @return the number of the first delta list in the page
 **/
unsigned int getChapterIndexLowestListNumber(const ChapterIndexPage *page);

/**
 * Get the highest numbered delta list for the given immutable chapter index
 * page.
 *
 * @param page  The chapter index page
 *
 * @return the number of the last delta list in the page
 **/
unsigned int getChapterIndexHighestListNumber(const ChapterIndexPage *page);

#endif /* CHAPTER_INDEX_H */
