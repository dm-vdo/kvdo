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
 * $Id: //eng/uds-releases/gloria/src/uds/geometry.c#1 $
 */

#include "geometry.h"

#include "deltaIndex.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/**********************************************************************/
static int initializeGeometry(Geometry    *geometry,
                              size_t       bytesPerPage,
                              unsigned int recordPagesPerChapter,
                              unsigned int chaptersPerVolume,
                              unsigned int sparseChaptersPerVolume)
{
  int result = ASSERT_WITH_ERROR_CODE(bytesPerPage >= BYTES_PER_RECORD,
                                      UDS_BAD_STATE,
                                      "page is smaller than a record: %zu",
                                      bytesPerPage);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT_WITH_ERROR_CODE(chaptersPerVolume > sparseChaptersPerVolume,
                                  UDS_INVALID_ARGUMENT,
                                  "sparse chapters per volume (%u) must be less"
                                  " than chapters per volume (%u)",
                                  sparseChaptersPerVolume,
                                  chaptersPerVolume);
  if (result != UDS_SUCCESS) {
    return result;
  }

  geometry->bytesPerPage            = bytesPerPage;
  geometry->recordPagesPerChapter   = recordPagesPerChapter;
  geometry->chaptersPerVolume       = chaptersPerVolume;
  geometry->sparseChaptersPerVolume = sparseChaptersPerVolume;
  geometry->denseChaptersPerVolume  =
    chaptersPerVolume - sparseChaptersPerVolume;

  // Calculate the number of records in a page, chapter, and volume.
  geometry->recordsPerPage = bytesPerPage / BYTES_PER_RECORD;
  geometry->recordsPerChapter
    = geometry->recordsPerPage * recordPagesPerChapter;
  geometry->recordsPerVolume
    = (unsigned long) geometry->recordsPerChapter * chaptersPerVolume;
  geometry->openChapterLoadRatio = DEFAULT_OPEN_CHAPTER_LOAD_RATIO;

  // Initialize values for delta chapter indexes.
  geometry->chapterMeanDelta     = 1 << DEFAULT_CHAPTER_MEAN_DELTA_BITS;
  geometry->chapterPayloadBits   = computeBits(recordPagesPerChapter - 1);
  // We want 1 delta list for every 64 records in the chapter.  The "| 077"
  // ensures that the chapterDeltaListBits computation does not underflow.
  geometry->chapterDeltaListBits
    = computeBits((geometry->recordsPerChapter - 1) | 077) - 6;
  geometry->deltaListsPerChapter = 1 << geometry->chapterDeltaListBits;
  // We need enough address bits to achieve the desired mean delta.
  geometry->chapterAddressBits
    = (DEFAULT_CHAPTER_MEAN_DELTA_BITS - geometry->chapterDeltaListBits
       + computeBits(geometry->recordsPerChapter - 1));
  // Let the delta index code determine how many pages are needed for the index
  geometry->indexPagesPerChapter
    = getDeltaIndexPageCount(geometry->recordsPerChapter,
                             geometry->deltaListsPerChapter,
                             geometry->chapterMeanDelta,
                             geometry->chapterPayloadBits,
                             bytesPerPage);

  // Now that we have the size of a chapter index, we can calculate the
  // space used by chapters and volumes.
  geometry->pagesPerChapter
    = geometry->indexPagesPerChapter + recordPagesPerChapter;
  geometry->pagesPerVolume  = geometry->pagesPerChapter * chaptersPerVolume;
  geometry->headerPagesPerVolume = 1;
  geometry->bytesPerVolume = bytesPerPage *
    (geometry->pagesPerVolume + geometry->headerPagesPerVolume);
  geometry->bytesPerChapter = bytesPerPage * geometry->pagesPerChapter;
  geometry->recordPageOffset = geometry->indexPagesPerChapter * bytesPerPage;

  return UDS_SUCCESS;
}

/**********************************************************************/
int makeGeometry(size_t       bytesPerPage,
                 unsigned int recordPagesPerChapter,
                 unsigned int chaptersPerVolume,
                 unsigned int sparseChaptersPerVolume,
                 Geometry   **geometryPtr)
{
  Geometry *geometry;
  int result = ALLOCATE(1, Geometry, "geometry", &geometry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initializeGeometry(geometry, bytesPerPage, recordPagesPerChapter,
                              chaptersPerVolume, sparseChaptersPerVolume);
  if (result != UDS_SUCCESS) {
    freeGeometry(geometry);
    return result;
  }

  *geometryPtr = geometry;
  return UDS_SUCCESS;
}

/**********************************************************************/
int copyGeometry(Geometry *source, Geometry **geometryPtr)
{
  return makeGeometry(source->bytesPerPage,
                      source->recordPagesPerChapter,
                      source->chaptersPerVolume,
                      source->sparseChaptersPerVolume,
                      geometryPtr);
}

/**********************************************************************/
bool verifyGeometry(const Geometry *expected, const Geometry *actual)
{
  return (expected->bytesPerPage == actual->bytesPerPage) &&
    (expected->recordPagesPerChapter == actual->recordPagesPerChapter) &&
    (expected->chaptersPerVolume == actual->chaptersPerVolume) &&
    (expected->sparseChaptersPerVolume == actual->sparseChaptersPerVolume) &&
    (expected->denseChaptersPerVolume == actual->denseChaptersPerVolume) &&
    (expected->denseChaptersPerVolume ==
     (expected->chaptersPerVolume - expected->sparseChaptersPerVolume)) &&
    (expected->chapterDeltaListBits == actual->chapterDeltaListBits) &&
    (expected->pagesPerVolume == actual->pagesPerVolume) &&
    (expected->bytesPerVolume == actual->bytesPerVolume) &&
    (expected->bytesPerChapter == actual->bytesPerChapter) &&
    (expected->pagesPerChapter == actual->pagesPerChapter) &&
    (expected->indexPagesPerChapter == actual->indexPagesPerChapter) &&
    (expected->openChapterLoadRatio == actual->openChapterLoadRatio) &&
    (expected->recordsPerPage == actual->recordsPerPage) &&
    (expected->recordsPerChapter == actual->recordsPerChapter) &&
    (expected->recordsPerVolume == actual->recordsPerVolume) &&
    (expected->recordPageOffset == actual->recordPageOffset) &&
    (expected->deltaListsPerChapter == actual->deltaListsPerChapter) &&
    (expected->chapterMeanDelta == actual->chapterMeanDelta) &&
    (expected->chapterPayloadBits == actual->chapterPayloadBits) &&
    (expected->chapterAddressBits == actual->chapterAddressBits);
}

/**********************************************************************/
void freeGeometry(Geometry *geometry)
{
  FREE(geometry);
}

/**********************************************************************/
uint64_t mapToVirtualChapterNumber(Geometry     *geometry,
                                   uint64_t      newestVirtualChapter,
                                   unsigned int  physicalChapter)
{
  unsigned int newestPhysicalChapter
    = mapToPhysicalChapter(geometry, newestVirtualChapter);
  uint64_t virtualChapter
    = newestVirtualChapter - newestPhysicalChapter + physicalChapter;
  if (physicalChapter > newestPhysicalChapter) {
    virtualChapter -= geometry->chaptersPerVolume;
  }
  return virtualChapter;
}

/**********************************************************************/
bool hasSparseChapters(const Geometry *geometry,
                       uint64_t        oldestVirtualChapter,
                       uint64_t        newestVirtualChapter)
{
  return (isSparse(geometry)
          && ((newestVirtualChapter - oldestVirtualChapter + 1)
              > geometry->denseChaptersPerVolume));
}

/**********************************************************************/
bool isChapterSparse(const Geometry *geometry,
                     uint64_t        oldestVirtualChapter,
                     uint64_t        newestVirtualChapter,
                     uint64_t        virtualChapterNumber)
{
  return (hasSparseChapters(geometry, oldestVirtualChapter,
                            newestVirtualChapter)
          && ((virtualChapterNumber + geometry->denseChaptersPerVolume)
              <= newestVirtualChapter));
}

/**********************************************************************/
bool areSamePhysicalChapter(const Geometry *geometry,
                            uint64_t        chapter1,
                            uint64_t        chapter2)
{
  return ((chapter1 % geometry->chaptersPerVolume)
          == (chapter2 % geometry->chaptersPerVolume));
}
