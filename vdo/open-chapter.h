/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef OPENCHAPTER_H
#define OPENCHAPTER_H 1

#include "chapter-index.h"
#include "common.h"
#include "geometry.h"
#include "index.h"
#include "volume.h"

/**
 * open_chapter handles writing the open chapter records to the volume, and
 * also manages all the tools to generate and parse the open chapter file. The
 * open chapter file interleaves records from each open_chapter_zone structure.
 *
 * <p>Once each open chapter zone is filled, the records are interleaved to
 * preserve temporal locality, the index pages are generated through a
 * delta chapter index, and the record pages are derived by sorting each
 * page-sized batch of records by their names.
 *
 * <p>Upon index shutdown, the open chapter zone records are again
 * interleaved, and the records are stored as a single array. The hash
 * slots are not preserved, since the records may be reassigned to new
 * zones at load time.
 **/

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
 **/
void remove_from_open_chapter(struct open_chapter_zone *open_chapter,
			      const struct uds_chunk_name *name);

/**
 * Clean up an open chapter and its memory.
 *
 * @param open_chapter the chapter to destroy
 **/
void free_open_chapter(struct open_chapter_zone *open_chapter);

/**
 * Close the open chapter and write it to disk.
 *
 * @param chapter_zones          The zones of the chapter to close
 * @param zone_count             The number of zones
 * @param volume                 The volume to which to write the chapter
 * @param chapter_index          The open_chapter_index to use while writing
 * @param collated_records       Collated records array to use while writing
 * @param virtual_chapter_number The virtual chapter number of the open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check close_open_chapter(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct volume *volume,
				    struct open_chapter_index *chapter_index,
				    struct uds_chunk_record *collated_records,
				    uint64_t virtual_chapter_number);

/**
 * Write out a partially filled chapter to a file.
 *
 * @param index        the index to save the data from
 * @param writer       the writer to write out the chapters
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check save_open_chapters(struct uds_index *index,
				    struct buffered_writer *writer);

/**
 * Read a partially filled chapter from a file.
 *
 * @param index        the index to load the data into
 * @param reader       the buffered reader to read from
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check load_open_chapters(struct uds_index *index,
				    struct buffered_reader *reader);

/**
 * Compute the size of the maximum open chapter save image.
 *
 * @param geometry      the index geometry
 *
 * @return the number of bytes of the largest possible open chapter save
 *         image
 **/
uint64_t compute_saved_open_chapter_size(struct geometry *geometry);

#endif /* OPENCHAPTER_H */
