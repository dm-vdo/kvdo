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
 * $Id: //eng/uds-releases/homer/src/uds/volumeInternals.c#2 $
 */

#include "volumeInternals.h"

#include "bufferedReader.h"
#include "errors.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "recordPage.h"
#include "stringUtils.h"
#include "volume.h"

/* Magic number and versioning */
const byte VOLUME_MAGIC_NUMBER[]       = "ALBV";
const byte VOLUME_VERSION[]            = "04.20";
const unsigned int VOLUME_MAGIC_LENGTH = sizeof(VOLUME_MAGIC_NUMBER) - 1;

const bool READ_ONLY_VOLUME = true;

/**********************************************************************/
size_t encodeVolumeFormat(byte *volumeFormat, const Geometry *geometry)
{
  int size = 0;
  memcpy(volumeFormat, VOLUME_MAGIC_NUMBER, VOLUME_MAGIC_LENGTH);
  size += VOLUME_MAGIC_LENGTH;

  STATIC_ASSERT(VOLUME_VERSION_LENGTH == (sizeof(VOLUME_VERSION) - 1));
  memcpy(volumeFormat + size, VOLUME_VERSION, VOLUME_VERSION_LENGTH);
  size += VOLUME_VERSION_LENGTH;

  if (geometry) {
    memcpy(volumeFormat + size, geometry, sizeof(Geometry));
    size += sizeof(Geometry);
  }

  return size;
}

/**********************************************************************/
int allocateVolume(const Configuration  *config,
                   IndexLayout          *layout,
                   unsigned int          readQueueMaxSize,
                   unsigned int          zoneCount,
                   bool                  readOnly,
                   Volume              **newVolume)
{
  IOAccessMode access = readOnly ? IO_READ : IO_READ_WRITE;
  IORegion *region;
  int result = openVolumeRegion(layout, access, &region);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Volume *volume;
  result = ALLOCATE(1, Volume, "volume", &volume);
  if (result != UDS_SUCCESS) {
    closeIORegion(&region);
    return result;
  }
  // Fill these fields in now so that freeVolume will close the volume region
  volume->region = region;
  volume->readOnly = readOnly;
  volume->nonce = getVolumeNonce(layout);

  result = copyGeometry(config->geometry, &volume->geometry);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return logWarningWithStringError(result,
                                     "failed to allocate geometry: error");
  }

  result = ALLOCATE_IO_ALIGNED(config->geometry->bytesPerPage, byte,
                               "scratch page", &volume->scratchPage);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = makeRadixSorter(config->geometry->recordsPerPage,
                           &volume->radixSorter);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }
  result = ALLOCATE(config->geometry->recordsPerPage, const UdsChunkRecord *,
                    "record pointers", &volume->recordPointers);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  if (!readOnly) {
    if (isSparse(volume->geometry)) {
      result = makeSparseCache(volume->geometry, config->cacheChapters,
                               zoneCount, &volume->sparseCache);
      if (result != UDS_SUCCESS) {
        freeVolume(volume);
        return result;
      }
    }
    result = makePageCache(volume->geometry, config->cacheChapters,
                           readQueueMaxSize, zoneCount, &volume->pageCache);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return result;
    }
    result = makeIndexPageMap(volume->geometry, &volume->indexPageMap);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return result;
    }
  }

  *newVolume = volume;
  return UDS_SUCCESS;
}

/**********************************************************************/
int mapToPhysicalPage(Geometry *geometry, int chapter, int page)
{
  // Page zero is the header page, so the first index page in the
  // first chapter is physical page one.
  return (1 + (geometry->pagesPerChapter * chapter) + page);
}

/**********************************************************************/
int readPageToBuffer(const Volume *volume,
                     unsigned int  physicalPage,
                     byte         *buffer)
{
  off_t pageOffset
    = ((off_t) physicalPage) * ((off_t) volume->geometry->bytesPerPage);
  int result = readFromRegion(volume->region, pageOffset, buffer,
                              volume->geometry->bytesPerPage, NULL);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "error reading physical page %u",
                                     physicalPage);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int readChapterIndexToBuffer(const Volume *volume,
                             unsigned int  chapterNumber,
                             byte         *buffer)
{
  Geometry *geometry = volume->geometry;
  off_t chapterIndexOffset = offsetForChapter(geometry, chapterNumber);
  int result = readFromRegion(volume->region, chapterIndexOffset, buffer,
                              geometry->bytesPerPage *
                                geometry->indexPagesPerChapter, NULL);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "error reading physical chapter index %u",
                                     chapterNumber);
  }
  return UDS_SUCCESS;
}
