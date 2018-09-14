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
 * $Id: //eng/uds-releases/gloria/src/uds/volumeInternals.c#4 $
 */

#include "volumeInternals.h"

#include "bufferedReader.h"
#include "errors.h"
#include "hashUtils.h"
#include "indexConfig.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "recordPage.h"
#include "stringUtils.h"
#include "volume.h"

/* Magic number and versioning */
const byte VOLUME_MAGIC_NUMBER[]         = "ALBV";
const byte VOLUME_VERSION[]              = "04.20";
const byte VOLUME_VERSION_V4_10[]        = "04.10";
static const byte VOLUME_VERSION_V4[]    = "04.00";
static const byte VOLUME_VERSION_V3[]    = "00227";
const unsigned int VOLUME_MAGIC_LENGTH   = sizeof(VOLUME_MAGIC_NUMBER) - 1;
const unsigned int VOLUME_VERSION_LENGTH = sizeof(VOLUME_VERSION) - 1;

const bool READ_ONLY_VOLUME = true;

static int readGeometryV4_10(BufferedReader *reader, Volume *volume);
static int readGeometryV4(BufferedReader *reader, Volume *volume);

/**********************************************************************/
size_t encodeVolumeFormat(byte *volumeFormat, const Geometry *geometry)
{
  int size = 0;
  memcpy(volumeFormat, VOLUME_MAGIC_NUMBER, VOLUME_MAGIC_LENGTH);
  size += VOLUME_MAGIC_LENGTH;

  memcpy(volumeFormat + size, VOLUME_VERSION, VOLUME_VERSION_LENGTH);
  size += VOLUME_VERSION_LENGTH;

  if (geometry) {
    memcpy(volumeFormat + size, geometry, sizeof(Geometry));
    size += sizeof(Geometry);
  }

  return size;
}

/**
 * Read the geometry from a file at the indicated position.
 *
 * @param reader   The buffered reader to read from
 * @param volume   The volume whose geometry is to be read
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readGeometry(BufferedReader *reader, Volume *volume)
{
  int result = ALLOCATE(1, Geometry, "geometry", &volume->geometry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  memset(volume->geometry, 0, sizeof(Geometry));
  return readFromBufferedReader(reader, volume->geometry, sizeof(Geometry));
}

/**
 * Read the version from a volume file.
 *
 * @param reader        A buffered reader
 * @param versionNumber A buffer to hold the version
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readVersionNumber(BufferedReader *reader, byte *versionNumber)
{
  int result =
    verifyBufferedData(reader, VOLUME_MAGIC_NUMBER, VOLUME_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = readFromBufferedReader(reader, versionNumber, VOLUME_VERSION_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
static bool isVersion(byte *versionNumber, const byte *expectedVersionNumber)
{
  return
    memcmp(versionNumber, expectedVersionNumber, VOLUME_VERSION_LENGTH) == 0;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int openVolume(Volume *volume)
{
  BufferedReader *reader = NULL;
  int result = makeBufferedReader(volume->region, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }

  byte versionNumber[VOLUME_VERSION_LENGTH];
  result = readVersionNumber(reader, versionNumber);
  if (result != UDS_SUCCESS) {
    freeBufferedReader(reader);
    return result;
  }

  result = UDS_UNSUPPORTED_VERSION;
  if (isVersion(versionNumber, VOLUME_VERSION)) {
    result = readGeometry(reader, volume);
  } else if (isVersion(versionNumber, VOLUME_VERSION_V4_10)) {
    if (volume->readOnly) {
      logWarning("opening obsolete volume version v4.10 for reading");
      result = readGeometryV4_10(reader, volume);
    }
  } else if (isVersion(versionNumber, VOLUME_VERSION_V4)) {
    if (volume->readOnly) {
      logWarning("opening obsolete volume version v4.00 for reading");
      result = readGeometryV4(reader, volume);
    }
  } else if (isVersion(versionNumber, VOLUME_VERSION_V3)) {
    if (volume->readOnly) {
      logWarning("opening obsolete volume version v3 for reading");
      result = readGeometryV4(reader, volume);
    }
  }
  freeBufferedReader(reader);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result, "failed to open volume");
  }
  return UDS_SUCCESS;
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

  result = openVolume(volume);
  if (result != UDS_SUCCESS) {
    freeVolume(volume);
    return result;
  }

  if (volume->geometry == NULL) {
    result = copyGeometry(config->geometry, &volume->geometry);
    if (result != UDS_SUCCESS) {
      freeVolume(volume);
      return logWarningWithStringError(result,
                                       "failed to allocate geometry: error");
    }
  } else {
    if (!readOnly && !verifyGeometry(config->geometry, volume->geometry)) {
      freeVolume(volume);
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                              "config and volume geometries are inconsistent");
    }
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

/*
 * Verion 4.10 geometry stuff.
 */
typedef struct geometry_V4_10 {
  /** Length of a page in a chapter, in bytes */
  size_t bytesPerPage;
  /** Number of record pages in a chapter */
  unsigned int recordPagesPerChapter;
  /** Number of (total) chapters in a volume */
  unsigned int chaptersPerVolume;
  /** Number of sparsely-indexed chapters in a volume */
  unsigned int sparseChaptersPerVolume;
  /** Number of bits used to determine delta list numbers */
  unsigned int chapterDeltaListBits;
  /** Total number of pages in a volume, excluding header */
  unsigned int pagesPerVolume;
  /** Total number of bytes in a volume, including header */
  size_t bytesPerVolume;
  /** Total number of bytes in a chapter */
  size_t bytesPerChapter;
  /** Number of pages in a chapter */
  unsigned int pagesPerChapter;
  /** Number of index pages in a chapter index */
  unsigned int indexPagesPerChapter;
  /** The minimum ratio of hash slots to records in an open chapter */
  unsigned int openChapterLoadRatio;
  /** Number of records that fit on a page */
  unsigned int recordsPerPage;
  /** Number of records that fit in a chapter */
  unsigned int recordsPerChapter;
  /** Number of records that fit in a volume */
  uint64_t recordsPerVolume;
  /** Offset of the first record page in a chapter */
  unsigned int recordPageOffset;
  /** Number of deltaLists per chapter index */
  unsigned int deltaListsPerChapter;
  /** Mean delta in chapter indexes */
  unsigned int chapterMeanDelta;
  /** Number of bits needed for record page numbers */
  unsigned int chapterPayloadBits;
  /** Number of bits used to compute addresses for chapter delta lists */
  unsigned int chapterAddressBits;
  /** Number of densely-indexed chapters in a volume */
  unsigned int denseChaptersPerVolume;
} GeometryV4_10;

static void convertGeometryV4_10(Geometry *dst, const GeometryV4_10 *src)
{
  dst->bytesPerPage = src->bytesPerPage;
  dst->recordPagesPerChapter = src->recordPagesPerChapter;
  dst->chaptersPerVolume = src->chaptersPerVolume;
  dst->sparseChaptersPerVolume = src->sparseChaptersPerVolume;
  dst->chapterDeltaListBits = src->chapterDeltaListBits;
  dst->pagesPerVolume = src->pagesPerVolume;
  dst->headerPagesPerVolume = 1;
  dst->bytesPerVolume = src->bytesPerVolume + src->bytesPerPage;
  dst->bytesPerChapter = src->bytesPerChapter;
  dst->pagesPerChapter = src->pagesPerChapter;
  dst->indexPagesPerChapter = src->indexPagesPerChapter;
  dst->openChapterLoadRatio = src->openChapterLoadRatio;
  dst->recordsPerPage = src->recordsPerPage;
  dst->recordsPerChapter = src->recordsPerChapter;
  dst->recordsPerVolume = src->recordsPerVolume;
  dst->recordPageOffset = src->recordPageOffset;
  dst->deltaListsPerChapter = src->deltaListsPerChapter;
  dst->chapterMeanDelta = src->chapterMeanDelta;
  dst->chapterPayloadBits = src->chapterPayloadBits;
  dst->chapterAddressBits = src->chapterAddressBits;
  dst->denseChaptersPerVolume = src->denseChaptersPerVolume;
}

/*
 * Version 4.00 geometry stuff.
 */
typedef struct geometryV4 {
  /** Length of a page in a chapter, in bytes */
  unsigned int bytesPerPage;
  /** Number of record pages in a chapter */
  unsigned int recordPagesPerChapter;
  /** Number of (total) chapters in a volume */
  unsigned int chaptersPerVolume;
  /** Number of sparsely-indexed chapters in a volume */
  unsigned int sparseChaptersPerVolume;
  /** Number of bits used to determine mean delta in chapter indexes */
  unsigned int chapterMeanDeltaBits;
  /** Number of bits used to determine delta list numbers */
  unsigned int chapterDeltaListBits;

  // These are derived properties, expressed as fields for convenience.
  /** Total number of pages in a volume */
  unsigned int pagesPerVolume;
  /** Total number of bytes in a volume */
  uint64_t bytesPerVolume;
  /** Total number of bytes in a chapter */
  uint64_t bytesPerChapter;
  /** Number of pages in a chapter */
  unsigned int pagesPerChapter;
  /** Number of index pages in a chapter index */
  unsigned int indexPagesPerChapter;
  /** Total number of hash slots in a chapter index */
  unsigned int indexSlotsPerChapter;
  /** Number of hash slots in an index hash page */
  unsigned int slotsPerIndexPage;
  /** Number of records that fit on a page */
  unsigned int recordsPerPage;
  /** Number of records that fit in a chapter */
  unsigned int recordsPerChapter;
  /** Number of records that fit in a volume */
  uint64_t recordsPerVolume;
  /** Offset of the first record page in a chapter */
  unsigned int recordPageOffset;
  /** Number of deltaLists per chapter index */
  unsigned int deltaListsPerChapter;
  /** Mean delta in chapter indexes */
  unsigned int chapterMeanDelta;
  /** Number of bits needed for record page numbers */
  unsigned int chapterPayloadBits;
  /** Number of bits used to compute addresses for chapter delta lists */
  unsigned int chapterAddressBits;
  /** Number of densely-indexed chapters in a volume */
  unsigned int denseChaptersPerVolume;
} GeometryV4;

static void convertGeometryV4(Geometry *dst, const GeometryV4 *src)
{
  dst->bytesPerPage = src->bytesPerPage;
  dst->recordPagesPerChapter = src->recordPagesPerChapter;
  dst->chaptersPerVolume = src->chaptersPerVolume;
  dst->sparseChaptersPerVolume = src->sparseChaptersPerVolume;
  dst->chapterDeltaListBits = src->chapterDeltaListBits;
  dst->pagesPerVolume = src->pagesPerVolume;
  dst->headerPagesPerVolume = 1;
  dst->bytesPerVolume = src->bytesPerVolume + src->bytesPerPage;
  dst->bytesPerChapter = src->bytesPerChapter;
  dst->pagesPerChapter = src->pagesPerChapter;
  dst->indexPagesPerChapter = src->indexPagesPerChapter;
  dst->openChapterLoadRatio = DEFAULT_OPEN_CHAPTER_LOAD_RATIO;
  dst->recordsPerPage = src->recordsPerPage;
  dst->recordsPerChapter = src->recordsPerChapter;
  dst->recordsPerVolume = src->recordsPerVolume;
  dst->recordPageOffset = src->recordPageOffset;
  dst->deltaListsPerChapter = src->deltaListsPerChapter;
  dst->chapterMeanDelta = src->chapterMeanDelta;
  dst->chapterPayloadBits = src->chapterPayloadBits;
  dst->chapterAddressBits = src->chapterAddressBits;
  dst->denseChaptersPerVolume = src->denseChaptersPerVolume;
}

/**
 * Read a V4.10 geometry from a file at the current position.
 *
 * @param reader   A buffered reader.
 * @param volume   The volume whose geometry is to be read
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readGeometryV4_10(BufferedReader *reader, Volume *volume)
{
  int result = ALLOCATE(1, Geometry, "geometry", &volume->geometry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  GeometryV4_10 oldGeometry;
  memset(&oldGeometry, 0, sizeof(oldGeometry));
  result = readFromBufferedReader(reader, &oldGeometry, sizeof(oldGeometry));
  if (result != UDS_SUCCESS) {
    return result;
  }

  memset(volume->geometry, 0, sizeof(Geometry));
  convertGeometryV4_10(volume->geometry, &oldGeometry);
  return UDS_SUCCESS;
}

/**
 * Read a V4.00 geometry from a file at the current position.
 *
 * @param reader   A buffered reader.
 * @param volume   The volume whose geometry is to be read
 *
 * @return UDS_SUCCESS or an error code
 **/
static int readGeometryV4(BufferedReader *reader, Volume *volume)
{
  int result = ALLOCATE(1, Geometry, "geometry", &volume->geometry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  GeometryV4 geometryV4;
  memset(&geometryV4, 0, sizeof(GeometryV4));
  result = readFromBufferedReader(reader, &geometryV4, sizeof(GeometryV4));
  if (result != UDS_SUCCESS) {
    return result;
  }

  memset(volume->geometry, 0, sizeof(Geometry));
  convertGeometryV4(volume->geometry, &geometryV4);
  return UDS_SUCCESS;
}
