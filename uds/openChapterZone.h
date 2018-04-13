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
 * $Id: //eng/uds-releases/gloria/src/uds/openChapterZone.h#1 $
 */

#ifndef OPEN_CHAPTER_ZONE_H
#define OPEN_CHAPTER_ZONE_H 1

#include "common.h"
#include "geometry.h"
#include "typeDefs.h"

/**
 * OpenChapterZone is the mutable, in-memory representation of one zone's
 * section of an Albireo index chapter.
 *
 * <p>In addition to providing the same access to records as an on-disk
 * chapter, the open chapter zone must allow records to be added or
 * modified. It must provide a way to generate the on-disk representation
 * without excessive work. It does that by accumulating records in the order
 * they are added (maintaining temporal locality), and referencing them (as
 * record numbers) from hash slots selected from the name. If the metadata for
 * a name changes, the record field is just modified in place.
 *
 * <p>Storage for the records (names and metadata) is allocated when the zone
 * is created. It keeps no references to the data passed to it, and performs
 * no additional allocation when adding records. Opening a new chapter simply
 * marks it as being empty.
 *
 * <p>Records are stored in a flat array. To allow a value of zero in a
 * hash slot to indicate that the slot is empty, records are numbered starting
 * at one (1-based). Since C arrays are 0-based, the records array contains
 * enough space for N+1 records, and the record that starts at array index
 * zero is never used or referenced.
 *
 * <p>The array of hash slots is actually two arrays, superimposed: an
 * array of record numbers, indexed by hash value, and an array of deleted
 * flags, indexed by record number. This overlay is possible because the
 * number of hash slots always exceeds the number of records, and is done
 * simply to save on memory.
 **/

enum {
  OPEN_CHAPTER_RECORD_NUMBER_BITS = 23,
  OPEN_CHAPTER_MAX_RECORD_NUMBER = (1 << OPEN_CHAPTER_RECORD_NUMBER_BITS) - 1
};

typedef struct {
  /** If non-zero, the record number addressed by this hash slot */
  unsigned int recordNumber : OPEN_CHAPTER_RECORD_NUMBER_BITS;
  /** If true, the record at the index of this hash slot was deleted */
  bool         recordDeleted : 1;
} __attribute__((packed)) Slot;

typedef struct openChapterZone {
  /** Maximum number of records that can be stored */
  unsigned int    capacity;
  /** Number of records stored */
  unsigned int    size;
  /** Number of deleted records */
  unsigned int    deleted;
  /** Record data, stored as (name, metadata), 1-based */
  UdsChunkRecord *records;
  /** The number of slots in the chapter zone hash table. */
  unsigned int    slotCount;
  /** Hash table, referencing virtual record numbers */
  Slot            slots[];
} OpenChapterZone;

/**
 * Allocate an open chapter zone.
 *
 * @param geometry       the geometry of the volume
 * @param zoneCount      the total number of open chapter zones
 * @param openChapterPtr a pointer to hold the new open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeOpenChapter(const Geometry   *geometry,
                    unsigned int      zoneCount,
                    OpenChapterZone **openChapterPtr)
  __attribute__((warn_unused_result));

/**
 * Return the number of records in the open chapter zone that have not been
 * deleted.
 *
 * @return the number of non-deleted records
 **/
size_t openChapterSize(const OpenChapterZone *openChapter)
  __attribute__((warn_unused_result));

/**
 * Open a chapter by marking it empty.
 *
 * @param openChapter The chapter to open
 **/
void resetOpenChapter(OpenChapterZone *openChapter);

/**
 * Search the open chapter for a chunk name.
 *
 * @param openChapter The chapter to search
 * @param name        The name of the desired chunk
 * @param metadata    The holder for the metadata associated with the
 *                    chunk, if found (or NULL)
 * @param found       A pointer which will be set to true if the chunk
 *                    name was found
 **/
void searchOpenChapter(OpenChapterZone    *openChapter,
                       const UdsChunkName *name,
                       UdsChunkData       *metadata,
                       bool               *found);

/**
 * Put a record into the open chapter.
 *
 * @param openChapter The chapter into which to put the record
 * @param name        The name of the record
 * @param metadata    The record data
 * @param remaining   Pointer to an integer set to the number of additional
 *                    records that can be added to this chapter
 *
 * @return            UDS_SUCCESS or an error code
 **/
int putOpenChapter(OpenChapterZone    *openChapter,
                   const UdsChunkName *name,
                   const UdsChunkData *metadata,
                   unsigned int       *remaining)
  __attribute__((warn_unused_result));

/**
 * Remove a record from the open chapter.
 *
 * @param openChapter The chapter from which to remove the record
 * @param name        The name of the record
 * @param removed     Pointer to bool set to <code>true</code> if the
 *                    record was found
 **/
void removeFromOpenChapter(OpenChapterZone    *openChapter,
                           const UdsChunkName *name,
                           bool               *removed);

/**
 * Clean up an open chapter and its memory.
 *
 * @param openChapter the chapter to destroy
 **/
void freeOpenChapter(OpenChapterZone *openChapter);

#endif /* OPEN_CHAPTER_ZONE_H */
