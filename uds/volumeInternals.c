/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/volumeInternals.c#6 $
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

/**********************************************************************/
int allocateVolume(const Configuration  *config,
                   IndexLayout          *layout,
                   unsigned int          readQueueMaxSize,
                   unsigned int          zoneCount,
                   Volume              **newVolume)
{
  Volume *volume;
  int result = ALLOCATE(1, Volume, "volume", &volume);
  if (result != UDS_SUCCESS) {
    return result;
  }
  volume->nonce = getVolumeNonce(layout);
  // It is safe to call freeVolume now to clean up and close the volume

  result = copyGeometry(config->geometry, &volume->geometry);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return logWarningWithStringError(result,
                                     "failed to allocate geometry: error");
  }

  result = openVolumeBufio(layout, config->geometry->bytesPerPage, 1,
                           &volume->bufioClient);
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
                     byte         *data)
{
  struct dm_buffer *buffer = NULL;
  byte *cacheData = dm_bufio_read(volume->bufioClient, physicalPage, &buffer);
  if (IS_ERR(cacheData)) {
    return logWarningWithStringError(-PTR_ERR(cacheData),
                                     "error reading physical page %u",
                                     physicalPage);
  }
  memcpy(data, cacheData, volume->geometry->bytesPerPage);
  dm_bufio_release(buffer);
  return UDS_SUCCESS;
}

/**********************************************************************/
int readChapterIndexToBuffer(const Volume *volume,
                             unsigned int  chapterNumber,
                             byte         *buffer)
{
  Geometry *geometry = volume->geometry;
  int result = UDS_SUCCESS;
  int physicalPage = mapToPhysicalPage(geometry, chapterNumber, 0);
  dm_bufio_prefetch(volume->bufioClient, physicalPage,
                    geometry->indexPagesPerChapter);
  unsigned int i;
  for (i = 0; i < geometry->indexPagesPerChapter; i++) {
    result = readPageToBuffer(volume, physicalPage + i, buffer);
    buffer += geometry->bytesPerPage;
    if (result != UDS_SUCCESS) {
      break;
    }
  }
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "error reading physical chapter index %u",
                                     chapterNumber);
  }
  return UDS_SUCCESS;
}
