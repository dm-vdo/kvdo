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
 * $Id: //eng/uds-releases/krusty/src/uds/openChapterZone.h#12 $
 */

#ifndef OPEN_CHAPTER_ZONE_H
#define OPEN_CHAPTER_ZONE_H 1

#include "common.h"
#include "geometry.h"
#include "typeDefs.h"

/**
 * open_chapter_zone is the mutable, in-memory representation of one zone's
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
	OPEN_CHAPTER_MAX_RECORD_NUMBER =
	       (1 << OPEN_CHAPTER_RECORD_NUMBER_BITS) - 1
};

struct open_chapter_zone_slot {
	/** If non-zero, the record number addressed by this hash slot */
	unsigned int record_number : OPEN_CHAPTER_RECORD_NUMBER_BITS;
	/** If true, the record at the index of this hash slot was deleted */
	bool record_deleted : 1;
} __packed;

struct open_chapter_zone {
	/** Maximum number of records that can be stored */
	unsigned int capacity;
	/** Number of records stored */
	unsigned int size;
	/** Number of deleted records */
	unsigned int deleted;
	/** Record data, stored as (name, metadata), 1-based */
	struct uds_chunk_record *records;
	/** The number of slots in the chapter zone hash table. */
	unsigned int slot_count;
	/** Hash table, referencing virtual record numbers */
	struct open_chapter_zone_slot slots[];
};

/**
 * Allocate an open chapter zone.
 *
 * @param geometry          the geometry of the volume
 * @param zone_count        the total number of open chapter zones
 * @param open_chapter_ptr  a pointer to hold the new open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
make_open_chapter(const struct geometry *geometry,
		  unsigned int zone_count,
		  struct open_chapter_zone **open_chapter_ptr);

/**
 * Return the number of records in the open chapter zone that have not been
 * deleted.
 *
 * @return the number of non-deleted records
 **/
size_t __must_check
open_chapter_size(const struct open_chapter_zone *open_chapter);

/**
 * Open a chapter by marking it empty.
 *
 * @param open_chapter The chapter to open
 **/
void reset_open_chapter(struct open_chapter_zone *open_chapter);

/**
 * Search the open chapter for a chunk name.
 *
 * @param open_chapter  The chapter to search
 * @param name          The name of the desired chunk
 * @param metadata      The holder for the metadata associated with the
 *                      chunk, if found (or NULL)
 * @param found         A pointer which will be set to true if the chunk
 *                      name was found
 **/
void search_open_chapter(struct open_chapter_zone *open_chapter,
			 const struct uds_chunk_name *name,
			 struct uds_chunk_data *metadata,
			 bool *found);

/**
 * Put a record into the open chapter.
 *
 * @param open_chapter  The chapter into which to put the record
 * @param name          The name of the record
 * @param metadata      The record data
 * @param remaining     Pointer to an integer set to the number of additional
 *                      records that can be added to this chapter
 *
 * @return            UDS_SUCCESS or an error code
 **/
int __must_check put_open_chapter(struct open_chapter_zone *open_chapter,
				  const struct uds_chunk_name *name,
				  const struct uds_chunk_data *metadata,
				  unsigned int *remaining);

/**
 * Remove a record from the open chapter.
 *
 * @param open_chapter  The chapter from which to remove the record
 * @param name          The name of the record
 * @param removed       Pointer to bool set to <code>true</code> if the
 *                      record was found
 **/
void remove_from_open_chapter(struct open_chapter_zone *open_chapter,
			      const struct uds_chunk_name *name,
			      bool *removed);

/**
 * Clean up an open chapter and its memory.
 *
 * @param open_chapter the chapter to destroy
 **/
void free_open_chapter(struct open_chapter_zone *open_chapter);

#endif /* OPEN_CHAPTER_ZONE_H */
