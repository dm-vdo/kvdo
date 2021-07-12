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
 * $Id: //eng/uds-releases/jasper/src/uds/geometry.h#9 $
 */

#ifndef GEOMETRY_H
#define GEOMETRY_H 1

#include "compiler.h"
#include "typeDefs.h"
#include "uds.h"
#include "uds-block.h"

/**
 * geometry defines constants and a record that parameterize the
 * layout of a UDS index volume.
 *
 * <p>An index volume is divided into a fixed number of fixed-size
 * chapters, each consisting of a fixed number of fixed-size
 * pages. The volume layout is defined by two assumptions and four
 * parameters. The assumptions (constants) are that index records are
 * 32 bytes (16-byte block name plus 16-byte metadata) and that open
 * chapter index hash slots are one byte long. The four parameters are
 * the number of bytes in a page, the number of chapters in a volume,
 * the number of record pages in a chapter, and the number of chapters
 * that are sparse. From these parameters, we derive the rest of the
 * layout and derived properties, ranging from the number of pages in
 * a chapter to the number of records in the volume.
 *
 * <p>The index volume is sized by its memory footprint. For a dense
 * index, the persistent storage is about 10 times the size of the
 * memory footprint. For a sparse index, the persistent storage is
 * about 100 times the size of the memory footprint.
 *
 * <p>For a small index with a memory footprint less than 1GB, there
 * are three possible memory configurations: 0.25GB, 0.5GB and
 * 0.75GB. The default geometry for each is 1024 index records per 32
 * KB page, 1024 chapters per volume, and either 64, 128, or 192
 * record pages per chapter and 6, 13, or 20 index pages per chapter
 * depending on the memory configuration. For a 0.25 GB index as
 * commonly used with small VDO volumes, this yields a deduplication
 * window of 256 GB using about 2.5 GB for the persistent storage and
 * 256 MB of RAM.
 *
 * <p>For a large index, that is one with a memory footprint that is a
 * multiple of one GB, the geometry is 1024 index records per 32 KB
 * page, 256 record pages per chapter, 26 index pages per chapter, and
 * 1024 chapters for every GB of memory footprint. For a one GB
 * volume, this yields a deduplication window of 1 TB using about 9GB
 * of persistent storage and 1 GB of RAM.
 *
 * <p>For all sizes, the default is zero sparse chapters. A sparse
 * volume has about 10 times the deduplication window using 10 times
 * as much persistent storage as the equivalent non-sparse volume with
 * the same memory footprint.
 *
 * <p>If the number of chapters per volume has been reduced by one by
 * eliminating physical chapter 0, the virtual chapter that formerly
 * mapped to physical chapter 0 may be remapped to another physical
 * chapter. This remapping is expressed by storing which virtual
 * chapter was remapped, and which physical chapter it was moved to.
 **/
typedef struct geometry {
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
  /** Virtual chapter remapped from physical chapter 0 */
  uint64_t remappedVirtual;
  /** New physical chapter which remapped chapter was moved to */
  uint64_t remappedPhysical;
  // These are derived properties, expressed as fields for convenience.
  /** Total number of pages in a volume, excluding header */
  unsigned int pagesPerVolume;
  /** Total number of header pages per volume */
  unsigned int headerPagesPerVolume;
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
} Geometry;

enum {
  /* The number of bytes in a record (name + metadata) */
  BYTES_PER_RECORD = (UDS_CHUNK_NAME_SIZE + UDS_MAX_BLOCK_DATA_SIZE),

  /* The default length of a page in a chapter, in bytes */
  DEFAULT_BYTES_PER_PAGE = 1024 * BYTES_PER_RECORD,

  /* The default maximum number of records per page */
  DEFAULT_RECORDS_PER_PAGE = DEFAULT_BYTES_PER_PAGE / BYTES_PER_RECORD,

  /** The default number of record pages in a chapter */
  DEFAULT_RECORD_PAGES_PER_CHAPTER = 256,

  /** The default number of record pages in a chapter for a small index */
  SMALL_RECORD_PAGES_PER_CHAPTER = 64,

  /** The default number of chapters in a volume */
  DEFAULT_CHAPTERS_PER_VOLUME = 1024,

  /** The default number of sparsely-indexed chapters in a volume */
  DEFAULT_SPARSE_CHAPTERS_PER_VOLUME = 0,

  /** The log2 of the default mean delta */
  DEFAULT_CHAPTER_MEAN_DELTA_BITS = 16,

  /** The log2 of the number of delta lists in a large chapter */
  DEFAULT_CHAPTER_DELTA_LIST_BITS = 12,

  /** The log2 of the number of delta lists in a small chapter */
  SMALL_CHAPTER_DELTA_LIST_BITS = 10,

  /** The default min ratio of slots to records in an open chapter */
  DEFAULT_OPEN_CHAPTER_LOAD_RATIO = 2,

  /** Checkpoint every n chapters written.  Default is to not checkpoint */
  DEFAULT_CHECKPOINT_FREQUENCY = 0
};

/**
 * Allocate and initialize all fields of a volume geometry using the
 * specified layout parameters.
 *
 * @param bytesPerPage            The length of a page in a chapter, in bytes
 * @param recordPagesPerChapter   The number of pages in a chapter
 * @param chaptersPerVolume       The number of chapters in a volume
 * @param sparseChaptersPerVolume The number of sparse chapters in a volume
 * @param remappedVirtual         The remapped virtual chapter
 * @param remappedPhysical        The physical chapter remapped to
 * @param geometryPtr             A pointer to hold the new geometry
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeGeometry(size_t         bytesPerPage,
                 unsigned int   recordPagesPerChapter,
                 unsigned int   chaptersPerVolume,
                 unsigned int   sparseChaptersPerVolume,
                 uint64_t       remappedVirtual,
                 uint64_t       remappedPhysical,
                 Geometry     **geometryPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate a new geometry and initialize it with the same parameters as an
 * existing geometry.
 *
 * @param source      The geometry record to copy
 * @param geometryPtr A pointer to hold the new geometry
 *
 * @return UDS_SUCCESS or an error code
 **/
int copyGeometry(Geometry  *source,
                 Geometry **geometryPtr)
  __attribute__((warn_unused_result));

/**
 * Clean up a geometry and its memory.
 *
 * @param geometry The geometry record to free
 **/
void freeGeometry(Geometry *geometry);

/**
 * Map a virtual chapter number to a physical chapter number
 *
 * @param geometry        The geometry
 * @param virtualChapter  The virtual chapter number
 *
 * @return the corresponding physical chapter number
 **/
__attribute__((warn_unused_result))
unsigned int mapToPhysicalChapter(const Geometry *geometry,
                                  uint64_t        virtualChapter);

/**
 * Check whether this geometry is reduced by a chapter
 *
 * @param geometry  The geometry to check
 *
 * @return true if this geometry is reduced by a chapter
 **/
static INLINE bool 
isReducedGeometry(const Geometry *geometry)
{
  return !!(geometry->chaptersPerVolume & 1);
}

/**
 * Check whether this geometry is for a sparse index.
 *
 * @param geometry   The geometry to check
 *
 * @return true if this geometry has sparse chapters
 **/
__attribute__((warn_unused_result))
static INLINE bool isSparse(const Geometry *geometry)
{
  return (geometry->sparseChaptersPerVolume > 0);
}

/**
 * Check whether any sparse chapters have been filled.
 *
 * @param geometry             The geometry of the index
 * @param oldestVirtualChapter The number of the oldest chapter in the
 *                             index
 * @param newestVirtualChapter The number of the newest chapter in the
 *                             index
 *
 * @return true if the index has filled at least one sparse chapter
 **/
bool hasSparseChapters(const Geometry *geometry,
                       uint64_t        oldestVirtualChapter,
                       uint64_t        newestVirtualChapter)
  __attribute__((warn_unused_result));

/**
 * Check whether a chapter is sparse or dense.
 *
 * @param geometry             The geometry of the index containing the chapter
 * @param oldestVirtualChapter The number of the oldest chapter in the index
 * @param newestVirtualChapter The number of the newest chapter in the index
 * @param virtualChapterNumber The number of the chapter to check
 *
 * @return true if the chapter is sparse
 **/
bool isChapterSparse(const Geometry *geometry,
                     uint64_t        oldestVirtualChapter,
                     uint64_t        newestVirtualChapter,
                     uint64_t        virtualChapterNumber)
  __attribute__((warn_unused_result));

/**
 * Calculate how many chapters to expire after opening the newest chapter.
 *
 * @param geometry       The geometry of the index
 * @param newestChapter  The newest virtual chapter number
 *
 * @return The number of oldest chapters to expire
 **/
unsigned int chaptersToExpire(const Geometry *geometry, uint64_t newestChapter)
  __attribute__((warn_unused_result));

#endif /* GEOMETRY_H */
