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
 * $Id: //eng/uds-releases/jasper/src/uds/geometry.c#8 $
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
static int initializeGeometry(Geometry     *geometry,
                              size_t        bytesPerPage,
                              unsigned int  recordPagesPerChapter,
                              unsigned int  chaptersPerVolume,
                              unsigned int  sparseChaptersPerVolume,
                              uint64_t      remappedVirtual,
                              uint64_t      remappedPhysical)
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
  geometry->remappedVirtual         = remappedVirtual;
  geometry->remappedPhysical        = remappedPhysical;

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

  return UDS_SUCCESS;
}

/**********************************************************************/
int makeGeometry(size_t         bytesPerPage,
                 unsigned int   recordPagesPerChapter,
                 unsigned int   chaptersPerVolume,
                 unsigned int   sparseChaptersPerVolume,
                 uint64_t       remappedVirtual,
                 uint64_t       remappedPhysical,
                 Geometry     **geometryPtr)
{
  Geometry *geometry;
  int result = ALLOCATE(1, Geometry, "geometry", &geometry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = initializeGeometry(geometry,
                              bytesPerPage,
                              recordPagesPerChapter,
                              chaptersPerVolume,
                              sparseChaptersPerVolume,
                              remappedVirtual,
                              remappedPhysical);
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
                      source->remappedVirtual,
                      source->remappedPhysical,
                      geometryPtr);
}

/**********************************************************************/
void freeGeometry(Geometry *geometry)
{
  FREE(geometry);
}

/**********************************************************************/
__attribute__((warn_unused_result))
unsigned int mapToPhysicalChapter(const Geometry *geometry,
                                  uint64_t        virtualChapter)
{
  uint64_t delta;
  if (!isReducedGeometry(geometry)) {
    return (virtualChapter % geometry->chaptersPerVolume);
  }

  if (likely(virtualChapter > geometry->remappedVirtual)) {
    delta = virtualChapter - geometry->remappedVirtual;
    if (likely(delta > geometry->remappedPhysical)) {
      return (delta % geometry->chaptersPerVolume);
    } else {
      return (delta - 1);
    }
  }

  if (virtualChapter == geometry->remappedVirtual) {
    return geometry->remappedPhysical;
  }

  delta = geometry->remappedVirtual - virtualChapter;
  if (delta < geometry->chaptersPerVolume) {
    return (geometry->chaptersPerVolume - delta);
  }

  // This chapter is so old the answer doesn't matter.
  return 0;
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
  return (mapToPhysicalChapter(geometry, chapter1)
          == mapToPhysicalChapter(geometry, chapter2));
}
