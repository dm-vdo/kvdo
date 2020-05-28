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
 * $Id: //eng/uds-releases/krusty/src/uds/geometry.h#3 $
 */

#ifndef GEOMETRY_H
#define GEOMETRY_H 1

#include "compiler.h"
#include "typeDefs.h"
#include "uds.h"
#include "uds-block.h"

/**
 * geometry defines constants and a record that parameterize the layout of an
 * Albireo index volume.
 *
 * <p>An index volume is divided into a fixed number of fixed-size
 * chapters, each consisting of a fixed number of fixed-size
 * pages. The volume layout is defined by two assumptions and four
 * parameters. The assumptions (constants) are that index records are
 * 64 bytes (32-byte block name plus 32-byte metadata) and that open
 * chapter index hash slots are one byte long. The four parameters are
 * the number of bytes in a page, the number of chapters in a volume,
 * the number of record pages in a chapter, and the number of chapters
 * that are sparse. From these parameters, we derive the rest of the
 * layout and derived properties, ranging from the number of pages in
 * a chapter to the number of records in the volume.
 *
 * <p>The default geometry is 64 KByte pages, 1024 chapters, 256
 * record pages in a chapter, and zero sparse chapters. This will
 * allow us to store 2^28 entries (indexing 1TB of 4K blocks) in an
 * approximately 16.5 MByte volume using fourteen index pages in each
 * chapter.
 **/
struct geometry {
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
};

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
 * @param geometryPtr             A pointer to hold the new geometry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check makeGeometry(size_t bytesPerPage,
			      unsigned int recordPagesPerChapter,
			      unsigned int chaptersPerVolume,
			      unsigned int sparseChaptersPerVolume,
			      struct geometry **geometryPtr);

/**
 * Allocate a new geometry and initialize it with the same parameters as an
 * existing geometry.
 *
 * @param source      The geometry record to copy
 * @param geometryPtr A pointer to hold the new geometry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check copyGeometry(struct geometry  *source,
                              struct geometry **geometryPtr);

/**
 * Clean up a geometry and its memory.
 *
 * @param geometry The geometry record to free
 **/
void freeGeometry(struct geometry *geometry);

/**
 * Map a virtual chapter number to a physical chapter number
 *
 * @param geometry        The geometry
 * @param virtualChapter  The virtual chapter number
 *
 * @return the corresponding physical chapter number
 **/
static INLINE unsigned int __must_check
mapToPhysicalChapter(const struct geometry *geometry, uint64_t virtualChapter)
{
  return (virtualChapter % geometry->chaptersPerVolume);
}

/**
 * Convert a physical chapter number to its current virtual chapter number.
 *
 * @param geometry             The geometry
 * @param newestVirtualChapter The number of the newest virtual chapter
 * @param physicalChapter      The physical chapter number to convert
 *
 * @return The current virtual chapter number of the physical chapter
 *         in question
 **/
uint64_t mapToVirtualChapterNumber(struct geometry *geometry,
                                   uint64_t         newestVirtualChapter,
                                   unsigned int     physicalChapter);

/**
 * Check whether this geometry is for a sparse index.
 *
 * @param geometry   The geometry to check
 *
 * @return true if this geometry has sparse chapters
 **/
static INLINE bool __must_check isSparse(const struct geometry *geometry)
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
bool __must_check hasSparseChapters(const struct geometry *geometry,
				    uint64_t oldestVirtualChapter,
				    uint64_t newestVirtualChapter);

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
bool __must_check isChapterSparse(const struct geometry *geometry,
				  uint64_t oldestVirtualChapter,
				  uint64_t newestVirtualChapter,
				  uint64_t virtualChapterNumber);

/**
 * Check whether two virtual chapter numbers correspond to the same
 * physical chapter.
 *
 * @param geometry The geometry of the index
 * @param chapter1 The first chapter to compare
 * @param chapter2 The second chapter to compare
 *
 * @return <code>true</code> if both chapters correspond to the same
 *         physical chapter
 **/
bool __must_check
areSamePhysicalChapter(const struct geometry *geometry,
		       uint64_t chapter1,
		       uint64_t chapter2);

#endif /* GEOMETRY_H */
