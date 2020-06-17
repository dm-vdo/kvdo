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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndex005.c#24 $
 */
#include "masterIndex005.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "geometry.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "uds.h"
#include "zone.h"

/*
 * The master index is a kept as a delta index where the payload is a
 * chapter number.  The master index adds 2 basic functions to the delta
 * index:
 *
 *  (1) How to get the delta list number and address out of the chunk name.
 *
 *  (2) Dealing with chapter numbers, and especially the lazy flushing of
 *      chapters from the index.
 *
 * There are three ways of expressing chapter numbers: virtual, index, and
 * rolling.  The interface to the the master index uses virtual chapter
 * numbers, which are 64 bits long.  We do not store such large values in
 * memory, so we internally use a binary value using the minimal number of
 * bits.
 *
 * The delta index stores the index chapter number, which is the low-order
 * bits of the virtual chapter number.
 *
 * When we need to deal with ordering of index chapter numbers, we roll the
 * index chapter number around so that the smallest one we are using has
 * the representation 0.  See convert_index_to_virtual() or
 * flush_invalid_entries() for an example of this technique.
 */

struct master_index_zone {
	uint64_t virtual_chapter_low;   // The lowest virtual chapter indexed
	uint64_t virtual_chapter_high;  // The highest virtual chapter indexed
	long num_early_flushes;         // The number of early flushes
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct master_index5 {
	struct master_index common;     // Common master index methods
	struct delta_index delta_index; // The delta index
	uint64_t *flush_chapters;       // The first chapter to be flushed
	struct master_index_zone *master_zones; // The Zones
	uint64_t volume_nonce;          // The volume nonce
	uint64_t chapter_zone_bits;     // Expected size of a chapter
				        // (per zone)
	uint64_t max_zone_bits;         // Maximum size index (per zone)
	unsigned int address_bits;      // Number of bits in address mask
	unsigned int address_mask;      // Mask to get address within delta
				        // list
	unsigned int chapter_bits;      // Number of bits in chapter number
	unsigned int chapter_mask;      // Largest storable chapter number
	unsigned int num_chapters;      // Number of chapters used
	unsigned int num_delta_lists; // The number of delta lists
	unsigned int num_zones; // The number of zones
};

struct chapter_range {
	unsigned int chapter_start;  // The first chapter
	unsigned int chapter_count;  // The number of chapters
};

// Constants for the magic byte of a master_index_record
static const byte master_index_record_magic = 0xAA;
static const byte bad_magic = 0;

/*
 * In production, the default value for min_master_index_delta_lists will be
 * replaced by MAX_ZONES*MAX_ZONES.  Some unit tests will replace
 * min_master_index_delta_lists with the non-default value 1, because those
 * tests really want to run with a single delta list.
 */
unsigned int min_master_index_delta_lists;

/**
 * Maximum of two unsigned ints
 *
 * @param a  One unsigned int
 * @param b  Another unsigned int
 *
 * @return the bigger one
 **/
static INLINE unsigned int max_uint(unsigned int a, unsigned int b)
{
	return a > b ? a : b;
}

/**
 * Extract the address from a block name.
 *
 * @param mi5   The master index
 * @param name  The block name
 *
 * @return the address
 **/
static INLINE unsigned int extract_address(const struct master_index5 *mi5,
					   const struct uds_chunk_name *name)
{
	return extract_master_index_bytes(name) & mi5->address_mask;
}

/**
 * Extract the delta list number from a block name.
 *
 * @param mi5   The master index
 * @param name  The block name
 *
 * @return the delta list number
 **/
static INLINE unsigned int extract_dlist_num(const struct master_index5 *mi5,
					     const struct uds_chunk_name *name)
{
	uint64_t bits = extract_master_index_bytes(name);
	return (bits >> mi5->address_bits) % mi5->num_delta_lists;
}

/**
 * Get the master index zone containing a given master index record
 *
 * @param record  The master index record
 *
 * @return the master index zone
 **/
static INLINE const struct master_index_zone *
get_master_zone(const struct master_index_record *record)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	return &mi5->master_zones[record->zone_number];
}

/**
 * Convert an index chapter number to a virtual chapter number.
 *
 * @param record         The master index record
 * @param index_chapter  The index chapter number
 *
 * @return the virtual chapter number
 **/
static INLINE uint64_t
convert_index_to_virtual(const struct master_index_record *record,
			 unsigned int index_chapter)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	const struct master_index_zone *master_zone = get_master_zone(record);
	unsigned int rolling_chapter =
		((index_chapter - master_zone->virtual_chapter_low) &
		 mi5->chapter_mask);
	return master_zone->virtual_chapter_low + rolling_chapter;
}

/**
 * Convert a virtual chapter number to an index chapter number.
 *
 * @param mi5              The master index
 * @param virtual_chapter  The virtual chapter number
 *
 * @return the index chapter number
 **/
static INLINE unsigned int
convert_virtual_to_index(const struct master_index5 *mi5,
			 uint64_t virtual_chapter)
{
	return virtual_chapter & mi5->chapter_mask;
}

/**
 * Determine whether a virtual chapter number is in the range being indexed
 *
 * @param record           The master index record
 * @param virtual_chapter  The virtual chapter number
 *
 * @return true if the virtual chapter number is being indexed
 **/
static INLINE bool
is_virtual_chapter_indexed(const struct master_index_record *record,
			   uint64_t virtual_chapter)
{
	const struct master_index_zone *master_zone = get_master_zone(record);
	return ((virtual_chapter >= master_zone->virtual_chapter_low) &&
		(virtual_chapter <= master_zone->virtual_chapter_high));
}

/***********************************************************************/
/**
 * Flush an invalid entry from the master index, advancing to the next
 * valid entry.
 *
 * @param record                      Updated to describe the next valid record
 * @param flush_range                 Range of chapters to flush from the index
 * @param next_chapter_to_invalidate  Updated to record the next chapter that
 *                                    we will need to invalidate
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int
flush_invalid_entries(struct master_index_record *record,
		      struct chapter_range *flush_range,
		      unsigned int *next_chapter_to_invalidate)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	int result = next_delta_index_entry(&record->delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	while (!record->delta_entry.at_end) {
		unsigned int index_chapter =
			get_delta_entry_value(&record->delta_entry);
		unsigned int relative_chapter =
			((index_chapter - flush_range->chapter_start) &
			 mi5->chapter_mask);
		if (likely(relative_chapter >= flush_range->chapter_count)) {
			if (relative_chapter < *next_chapter_to_invalidate) {
				*next_chapter_to_invalidate = relative_chapter;
			}
			break;
		}
		result = remove_delta_index_entry(&record->delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**
 * Find the delta index entry, or the insertion point for a delta index
 * entry, while processing chapter LRU flushing.
 *
 * @param record        Updated to describe the entry being looked for
 * @param list_number   The delta list number
 * @param key           The address field being looked for
 * @param flush_range   The range of chapters to flush from the index
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_master_index_entry(struct master_index_record *record,
				  unsigned int list_number,
				  unsigned int key,
				  struct chapter_range *flush_range)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	unsigned int next_chapter_to_invalidate = mi5->chapter_mask;

	int result = start_delta_index_search(&mi5->delta_index, list_number,
					      0, false, &record->delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	do {
		result = flush_invalid_entries(record, flush_range,
					       &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} while (!record->delta_entry.at_end &&
			(key > record->delta_entry.key));

	result = remember_delta_index_offset(&record->delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// We probably found the record we want, but we need to keep going
	struct master_index_record other_record = *record;
	if (!other_record.delta_entry.at_end &&
	    (key == other_record.delta_entry.key)) {
		for (;;) {
			result = flush_invalid_entries(&other_record,
						       flush_range,
						       &next_chapter_to_invalidate);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (other_record.delta_entry.at_end ||
			    !other_record.delta_entry.is_collision) {
				break;
			}
			byte collision_name[UDS_CHUNK_NAME_SIZE];
			result = get_delta_entry_collision(&other_record.delta_entry,
							   collision_name);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (memcmp(collision_name,
				   record->name,
				   UDS_CHUNK_NAME_SIZE) == 0) {
				// This collision record is the one we are
				// looking for
				*record = other_record;
				break;
			}
		}
	}
	while (!other_record.delta_entry.at_end) {
		result = flush_invalid_entries(&other_record,
					       flush_range,
					       &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	next_chapter_to_invalidate += flush_range->chapter_start;
	next_chapter_to_invalidate &= mi5->chapter_mask;
	flush_range->chapter_start = next_chapter_to_invalidate;
	flush_range->chapter_count = 0;
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Terminate and clean up the master index
 *
 * @param master_index The master index to terminate
 **/
static void free_master_index_005(struct master_index *master_index)
{
	if (master_index != NULL) {
		struct master_index5 *mi5 = container_of(master_index,
							 struct master_index5,
							 common);
		FREE(mi5->flush_chapters);
		mi5->flush_chapters = NULL;
		FREE(mi5->master_zones);
		mi5->master_zones = NULL;
		uninitialize_delta_index(&mi5->delta_index);
		FREE(master_index);
	}
}

/**
 * Constants and structures for the saved master index file.  "MI5" is for
 * master index 005, and "-XXXX" is a number to increment when the format of
 * the data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_MI_START[] = "MI5-0005";

struct mi005_data {
	char magic[MAGIC_SIZE]; // MAGIC_MI_START
	uint64_t volume_nonce;
	uint64_t virtual_chapter_low;
	uint64_t virtual_chapter_high;
	unsigned int first_list;
	unsigned int num_lists;
};

/***********************************************************************/
/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param master_index  The master index
 * @param tag           The tag value
 **/
static void set_master_index_tag_005(struct master_index *master_index,
				     byte tag)
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	set_delta_index_tag(&mi5->delta_index, tag);
}

/***********************************************************************/
static int __must_check encode_master_index_header(struct buffer *buffer,
						   struct mi005_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_MI_START);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, header->volume_nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint64_le_into_buffer(buffer, header->virtual_chapter_low);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer,
					   header->virtual_chapter_high);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) ==
					sizeof(struct mi005_data),
				 "%zu bytes of config written, of %zu expected",
				 content_length(buffer),
				 sizeof(struct mi005_data));
	return result;
}

/**
 * Start saving a master index to a buffered output stream.
 *
 * @param master_index     The master index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_saving_master_index_005(const struct master_index *master_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	struct master_index_zone *master_zone =
		&mi5->master_zones[zone_number];
	unsigned int first_list =
		get_delta_index_zone_first_list(&mi5->delta_index,
						zone_number);
	unsigned int num_lists =
		get_delta_index_zone_num_lists(&mi5->delta_index, zone_number);

	struct mi005_data header;
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_MI_START, MAGIC_SIZE);
	header.volume_nonce = mi5->volume_nonce;
	header.virtual_chapter_low = master_zone->virtual_chapter_low;
	header.virtual_chapter_high = master_zone->virtual_chapter_high;
	header.first_list = first_list;
	header.num_lists = num_lists;

	struct buffer *buffer;
	int result = make_buffer(sizeof(struct mi005_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = encode_master_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(&buffer);
		return result;
	}
	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(&buffer);
	if (result != UDS_SUCCESS) {
		return logWarningWithStringError(result,
						 "failed to write master index header");
	}
	result = make_buffer(num_lists * sizeof(uint64_t), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	uint64_t *first_flush_chapter = &mi5->flush_chapters[first_list];
	result = put_uint64_les_into_buffer(buffer, num_lists,
					    first_flush_chapter);
	if (result != UDS_SUCCESS) {
		free_buffer(&buffer);
		return result;
	}
	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(&buffer);
	if (result != UDS_SUCCESS) {
		return logWarningWithStringError(result,
						 "failed to write master index flush ranges");
	}

	return start_saving_delta_index(&mi5->delta_index, zone_number,
					buffered_writer);
}

/***********************************************************************/
/**
 * Have all the data been written while saving a master index to an output
 * stream?  If the answer is yes, it is still necessary to call
 * finish_saving_master_index(), which will return quickly.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static bool
is_saving_master_index_done_005(const struct master_index *master_index,
				unsigned int zone_number)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	return is_saving_delta_index_done(&mi5->delta_index, zone_number);
}

/***********************************************************************/
/**
 * Finish saving a master index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
finish_saving_master_index_005(const struct master_index *master_index,
			       unsigned int zone_number)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	return finish_saving_delta_index(&mi5->delta_index, zone_number);
}

/***********************************************************************/
/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param master_index  The master index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
abort_saving_master_index_005(const struct master_index *master_index,
			      unsigned int zone_number)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	return abort_saving_delta_index(&mi5->delta_index, zone_number);
}

/***********************************************************************/
static int __must_check decode_master_index_header(struct buffer *buffer,
						   struct mi005_data *header)
{
	int result = get_bytes_from_buffer(buffer, sizeof(header->magic),
					   &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &header->volume_nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer,
					   &header->virtual_chapter_low);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer,
					   &header->virtual_chapter_high);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		ASSERT_LOG_ONLY(content_length(buffer) == 0,
				"%zu bytes decoded of %zu expected",
				buffer_length(buffer) - content_length(buffer),
				buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		result = UDS_CORRUPT_COMPONENT;
	}
	return result;
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param master_index      The master index to restore into
 * @param buffered_readers  The buffered readers to read the master index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_restoring_master_index_005(struct master_index *master_index,
				 struct buffered_reader **buffered_readers,
				 int num_readers)
{
	if (master_index == NULL) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "cannot restore to null master index");
	}
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	empty_delta_index(&mi5->delta_index);

	uint64_t virtual_chapter_low = 0;
	uint64_t virtual_chapter_high = 0;
	int i;
	for (i = 0; i < num_readers; i++) {
		struct buffer *buffer;
		int result = make_buffer(sizeof(struct mi005_data), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return logWarningWithStringError(result,
							 "failed to read master index header");
		}
		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return result;
		}
		struct mi005_data header;
		result = decode_master_index_header(buffer, &header);
		free_buffer(&buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (memcmp(header.magic, MAGIC_MI_START, MAGIC_SIZE) != 0) {
			return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
							 "master index file had bad magic number");
		}
		if (mi5->volume_nonce == 0) {
			mi5->volume_nonce = header.volume_nonce;
		} else if (header.volume_nonce != mi5->volume_nonce) {
			return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
							 "master index volume nonce incorrect");
		}
		if (i == 0) {
			virtual_chapter_low = header.virtual_chapter_low;
			virtual_chapter_high = header.virtual_chapter_high;
		} else if (virtual_chapter_high !=
			   header.virtual_chapter_high) {
			return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
							 "Inconsistent master index zone files: Chapter range is [%llu,%llu], chapter range %d is [%llu,%llu]",
							 virtual_chapter_low,
							 virtual_chapter_high,
							 i,
							 header.virtual_chapter_low,
							 header.virtual_chapter_high);
		} else if (virtual_chapter_low < header.virtual_chapter_low) {
			virtual_chapter_low = header.virtual_chapter_low;
		}
		uint64_t *first_flush_chapter =
			&mi5->flush_chapters[header.first_list];
		result = make_buffer(header.num_lists * sizeof(uint64_t),
				     &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return logWarningWithStringError(result,
							 "failed to read master index flush ranges");
		}
		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(&buffer);
			return result;
		}
		result = get_uint64_les_from_buffer(buffer, header.num_lists,
						    first_flush_chapter);
		free_buffer(&buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	unsigned int z;
	for (z = 0; z < mi5->num_zones; z++) {
		memset(&mi5->master_zones[z],
		       0,
		       sizeof(struct master_index_zone));
		mi5->master_zones[z].virtual_chapter_low = virtual_chapter_low;
		mi5->master_zones[z].virtual_chapter_high =
			virtual_chapter_high;
	}

	int result = start_restoring_delta_index(&mi5->delta_index,
						 buffered_readers,
						 num_readers);
	if (result != UDS_SUCCESS) {
		return logWarningWithStringError(result,
						 "restoring delta index failed");
	}
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Have all the data been read while restoring a master index from an
 * input stream?
 *
 * @param master_index  The master index to restore into
 *
 * @return true if all the data are read
 **/
static bool
is_restoring_master_index_done_005(const struct master_index *master_index)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	return is_restoring_delta_index_done(&mi5->delta_index);
}

/***********************************************************************/
/**
 * Restore a saved delta list
 *
 * @param master_index  The master index to restore into
 * @param dlsi          The delta_list_save_info describing the delta list
 * @param data          The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static int
restore_delta_list_to_master_index_005(struct master_index *master_index,
				       const struct delta_list_save_info *dlsi,
				       const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	return restore_delta_list_to_delta_index(&mi5->delta_index, dlsi, data);
}

/***********************************************************************/
/**
 * Abort restoring a master index from an input stream.
 *
 * @param master_index  The master index
 **/
static void abort_restoring_master_index_005(struct master_index *master_index)
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	abort_restoring_delta_index(&mi5->delta_index);
}

/***********************************************************************/
static void remove_newest_chapters(struct master_index5 *mi5,
				   unsigned int zone_number,
				   uint64_t virtual_chapter)
{
	// Get the range of delta lists belonging to this zone
	unsigned int first_list =
		get_delta_index_zone_first_list(&mi5->delta_index, zone_number);
	unsigned int num_lists =
		get_delta_index_zone_num_lists(&mi5->delta_index, zone_number);
	unsigned int last_list = first_list + num_lists - 1;

	if (virtual_chapter > mi5->chapter_mask) {
		// The virtual chapter number is large enough so that we can
		// use the normal LRU mechanism without an unsigned underflow.
		virtual_chapter -= mi5->chapter_mask + 1;
		// Eliminate the newest chapters by renumbering them to become
		// the oldest chapters
		unsigned int i;
		for (i = first_list; i <= last_list; i++) {
			if (virtual_chapter < mi5->flush_chapters[i]) {
				mi5->flush_chapters[i] = virtual_chapter;
			}
		}
	} else {
		// Underflow will prevent the fast path.  Do it the slow and
		// painful way.
		struct master_index_zone *master_zone =
			&mi5->master_zones[zone_number];
		struct chapter_range range;
		range.chapter_start =
			convert_virtual_to_index(mi5, virtual_chapter);
		range.chapter_count =
			(mi5->chapter_mask + 1 -
			 (virtual_chapter - master_zone->virtual_chapter_low));
		struct uds_chunk_name name;
		memset(&name, 0, sizeof(struct uds_chunk_name));
		struct master_index_record record =
			(struct master_index_record){
				.magic = master_index_record_magic,
				.master_index = &mi5->common,
				.name = &name,
				.zone_number = zone_number,
			};
		unsigned int i;
		for (i = first_list; i <= last_list; i++) {
			struct chapter_range temp_range = range;
			get_master_index_entry(&record, i, 0, &temp_range);
		}
	}
}

/***********************************************************************/
/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param master_index     The master index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_master_index_zone_open_chapter_005(struct master_index *master_index,
				       unsigned int zone_number,
				       uint64_t virtual_chapter)
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	struct master_index_zone *master_zone =
		&mi5->master_zones[zone_number];
	// Take care here to avoid underflow of an unsigned value.  Note that
	// this is the smallest valid virtual low.  We may or may not actually
	// use this value.
	uint64_t new_virtual_low =
		(virtual_chapter >= mi5->num_chapters ?
			 virtual_chapter - mi5->num_chapters + 1 :
			 0);

	if (virtual_chapter <= master_zone->virtual_chapter_low) {
		/*
		 * Moving backwards and the new range is totally before the old
		 * range. Note that moving to the lowest virtual chapter counts
		 * as totally before the old range, as we need to remove the
		 * entries in the open chapter.
		 */
		empty_delta_index_zone(&mi5->delta_index, zone_number);
		master_zone->virtual_chapter_low = virtual_chapter;
		master_zone->virtual_chapter_high = virtual_chapter;
	} else if (virtual_chapter <= master_zone->virtual_chapter_high) {
		// Moving backwards and the new range overlaps the old range.
		// Note that moving to the same open chapter counts as
		// backwards, as we need to remove the entries in the open
		// chapter.
		remove_newest_chapters(mi5, zone_number, virtual_chapter);
		master_zone->virtual_chapter_high = virtual_chapter;
	} else if (new_virtual_low < master_zone->virtual_chapter_low) {
		// Moving forwards and we can keep all the old chapters
		master_zone->virtual_chapter_high = virtual_chapter;
	} else if (new_virtual_low <= master_zone->virtual_chapter_high) {
		// Moving forwards and we can keep some old chapters
		master_zone->virtual_chapter_low = new_virtual_low;
		master_zone->virtual_chapter_high = virtual_chapter;
	} else {
		// Moving forwards and the new range is totally after the old
		// range
		master_zone->virtual_chapter_low = virtual_chapter;
		master_zone->virtual_chapter_high = virtual_chapter;
	}
	// Check to see if the zone data has grown to be too large
	if (master_zone->virtual_chapter_low <
	    master_zone->virtual_chapter_high) {
		uint64_t used_bits =
			get_delta_index_zone_dlist_bits_used(&mi5->delta_index,
							     zone_number);
		if (used_bits > mi5->max_zone_bits) {
			// Expire enough chapters to free the desired space
			uint64_t expire_count =
				1 + (used_bits - mi5->max_zone_bits) /
					    mi5->chapter_zone_bits;
			if (expire_count == 1) {
				logRatelimit(logInfo,
					     "masterZone %u:  At chapter %llu, expiring chapter %llu early",
					     zone_number,
					     virtual_chapter,
					     master_zone->virtual_chapter_low);
				master_zone->num_early_flushes++;
				master_zone->virtual_chapter_low++;
			} else {
				uint64_t first_expired =
					master_zone->virtual_chapter_low;
				if (first_expired + expire_count <
				    master_zone->virtual_chapter_high) {
					master_zone->num_early_flushes +=
						expire_count;
					master_zone->virtual_chapter_low +=
						expire_count;
				} else {
					master_zone->num_early_flushes +=
						master_zone
							->virtual_chapter_high -
						master_zone
							->virtual_chapter_low;
					master_zone->virtual_chapter_low =
						master_zone
							->virtual_chapter_high;
				}
				logRatelimit(logInfo,
					     "masterZone %u:  At chapter %llu, expiring chapters %llu to %llu early",
					     zone_number,
					     virtual_chapter,
					     first_expired,
					     master_zone->virtual_chapter_low - 1);
			}
		}
	}
}

/***********************************************************************/
/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param master_index     The master index
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_master_index_open_chapter_005(struct master_index *master_index,
				  uint64_t virtual_chapter)
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	unsigned int z;
	for (z = 0; z < mi5->num_zones; z++) {
		// In normal operation, we advance forward one chapter at a
		// time. Log all abnormal changes.
		struct master_index_zone *master_zone = &mi5->master_zones[z];
		bool log_move = virtual_chapter !=
				master_zone->virtual_chapter_high + 1;
		if (log_move) {
			logDebug("masterZone %u: The range of indexed chapters is moving from [%llu, %llu] ...",
				z,
				master_zone->virtual_chapter_low,
				master_zone->virtual_chapter_high);
		}

		set_master_index_zone_open_chapter_005(master_index, z,
						       virtual_chapter);

		if (log_move) {
			logDebug("masterZone %u: ... and moving to [%" PRIu64
				 ", %llu]",
				 z,
				 master_zone->virtual_chapter_low,
				 master_zone->virtual_chapter_high);
		}
	}
}

/***********************************************************************/
/**
 * Find the master index zone associated with a chunk name
 *
 * @param master_index The master index
 * @param name         The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int
get_master_index_zone_005(const struct master_index *master_index,
			  const struct uds_chunk_name *name)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	unsigned int delta_list_number = extract_dlist_num(mi5, name);
	return get_delta_index_zone(&mi5->delta_index, delta_list_number);
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param master_index The master index
 * @param name         The chunk name
 * @param triage       Information about the chunk name
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
lookup_master_index_name_005(const struct master_index *master_index,
			     const struct uds_chunk_name *name,
			     struct master_index_triage *triage)
{
	triage->is_sample = false;
	triage->in_sampled_chapter = false;
	triage->zone = get_master_index_zone_005(master_index, name);
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param master_index The master index
 * @param name         The chunk name
 * @param triage       Information about the chunk name.  The zone and
 *                     is_sample fields are already filled in.  Set
 *                     in_sampled_chapter and virtual_chapter if the chunk
 *                     name is found in the index.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
lookup_master_index_sampled_name_005(const struct master_index *master_index,
				     const struct uds_chunk_name *name,
				     struct master_index_triage *triage)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	unsigned int address = extract_address(mi5, name);
	unsigned int delta_list_number = extract_dlist_num(mi5, name);
	struct delta_index_entry delta_entry;
	int result = get_delta_index_entry(&mi5->delta_index,
					   delta_list_number,
					   address,
					   name->name,
					   true,
					   &delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	triage->in_sampled_chapter =
		!delta_entry.at_end && (delta_entry.key == address);
	if (triage->in_sampled_chapter) {
		const struct master_index_zone *master_zone =
			&mi5->master_zones[triage->zone];
		unsigned int index_chapter =
			get_delta_entry_value(&delta_entry);
		unsigned int rolling_chapter =
			((index_chapter - master_zone->virtual_chapter_low) &
			 mi5->chapter_mask);
		triage->virtual_chapter =
			master_zone->virtual_chapter_low + rolling_chapter;
		if (triage->virtual_chapter >
		    master_zone->virtual_chapter_high) {
			triage->in_sampled_chapter = false;
		}
	}
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Find the master index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * master index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the master_index_record so that
 * put_master_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the master_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_master_index_record() will remove the entry, calls to
 * set_master_index_record_chapter() can modify the entry, and calls to
 * put_master_index_record() can insert a collision record with this
 * entry.
 *
 * @param master_index  The master index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_master_index_record_005(struct master_index *master_index,
				       const struct uds_chunk_name *name,
				       struct master_index_record *record)
{
	struct master_index5 *mi5 =
		container_of(master_index, struct master_index5, common);
	unsigned int address = extract_address(mi5, name);
	unsigned int delta_list_number = extract_dlist_num(mi5, name);
	uint64_t flush_chapter = mi5->flush_chapters[delta_list_number];
	record->magic = master_index_record_magic;
	record->master_index = master_index;
	record->mutex = NULL;
	record->name = name;
	record->zone_number =
		get_delta_index_zone(&mi5->delta_index, delta_list_number);
	const struct master_index_zone *master_zone = get_master_zone(record);

	int result;
	if (flush_chapter < master_zone->virtual_chapter_low) {
		struct chapter_range range;
		uint64_t flush_count =
			master_zone->virtual_chapter_low - flush_chapter;
		range.chapter_start =
			convert_virtual_to_index(mi5, flush_chapter);
		range.chapter_count = (flush_count > mi5->chapter_mask ?
					       mi5->chapter_mask + 1 :
					       flush_count);
		result = get_master_index_entry(record, delta_list_number,
						address, &range);
		flush_chapter =
			convert_index_to_virtual(record, range.chapter_start);
		if (flush_chapter > master_zone->virtual_chapter_high) {
			flush_chapter = master_zone->virtual_chapter_high;
		}
		mi5->flush_chapters[delta_list_number] = flush_chapter;
	} else {
		result = get_delta_index_entry(&mi5->delta_index,
					       delta_list_number,
					       address,
					       name->name,
					       false,
					       &record->delta_entry);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	record->is_found = (!record->delta_entry.at_end &&
			   (record->delta_entry.key == address));
	if (record->is_found) {
		unsigned int index_chapter =
			get_delta_entry_value(&record->delta_entry);
		record->virtual_chapter =
			convert_index_to_virtual(record, index_chapter);
	}
	record->is_collision = record->delta_entry.is_collision;
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Create a new record associated with a block name.
 *
 * @param record           The master index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int put_master_index_record(struct master_index_record *record,
			    uint64_t virtual_chapter)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	if (record->magic != master_index_record_magic) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "bad magic number in master index record");
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct master_index_zone *master_zone =
			get_master_zone(record);
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot put record into chapter number %llu that is out of the valid range %llu to %llu",
						 virtual_chapter,
						 master_zone->virtual_chapter_low,
						 master_zone->virtual_chapter_high);
	}
	unsigned int address = extract_address(mi5, record->name);
	if (unlikely(record->mutex != NULL)) {
		lockMutex(record->mutex);
	}
	int result = put_delta_index_entry(&record->delta_entry,
					   address,
					   convert_virtual_to_index(mi5, virtual_chapter),
					   record->is_found ? record->name->name : NULL);
	if (unlikely(record->mutex != NULL)) {
		unlockMutex(record->mutex);
	}
	switch (result) {
	case UDS_SUCCESS:
		record->virtual_chapter = virtual_chapter;
		record->is_collision = record->delta_entry.is_collision;
		record->is_found = true;
		break;
	case UDS_OVERFLOW:
		logRatelimit(logWarningWithStringError,
			     UDS_OVERFLOW,
			     "Master index entry dropped due to overflow condition");
		log_delta_index_entry(&record->delta_entry);
		break;
	default:
		break;
	}
	return result;
}

/**********************************************************************/
static INLINE int validate_record(struct master_index_record *record)
{
	if (record->magic != master_index_record_magic) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "bad magic number in master index record");
	}
	if (!record->is_found) {
		return logWarningWithStringError(UDS_BAD_STATE,
						 "illegal operation on new record");
	}
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Remove an existing record.
 *
 * @param record      The master index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int remove_master_index_record(struct master_index_record *record)
{
	int result = validate_record(record);
	if (result != UDS_SUCCESS) {
		return result;
	}
	// Mark the record so that it cannot be used again
	record->magic = bad_magic;
	if (unlikely(record->mutex != NULL)) {
		lockMutex(record->mutex);
	}
	result = remove_delta_index_entry(&record->delta_entry);
	if (unlikely(record->mutex != NULL)) {
		unlockMutex(record->mutex);
	}
	return result;
}

/***********************************************************************/
int set_master_index_record_chapter(struct master_index_record *record,
				    uint64_t virtual_chapter)
{
	const struct master_index5 *mi5 = container_of(record->master_index,
						       struct master_index5,
						       common);
	int result = validate_record(record);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct master_index_zone *master_zone =
			get_master_zone(record);
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot set chapter number %llu that is out of the valid range %llu to %llu",
						 virtual_chapter,
						 master_zone->virtual_chapter_low,
						 master_zone->virtual_chapter_high);
	}
	if (unlikely(record->mutex != NULL)) {
		lockMutex(record->mutex);
	}
	result = set_delta_entry_value(&record->delta_entry,
				       convert_virtual_to_index(mi5,
				       				virtual_chapter));
	if (unlikely(record->mutex != NULL)) {
		unlockMutex(record->mutex);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	record->virtual_chapter = virtual_chapter;
	return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Get the number of bytes used for master index entries.
 *
 * @param master_index The master index
 *
 * @return The number of bytes in use
 **/
static size_t
get_master_index_memory_used_005(const struct master_index *master_index)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	uint64_t bits = get_delta_index_dlist_bits_used(&mi5->delta_index);
	return (bits + CHAR_BIT - 1) / CHAR_BIT;
}

/***********************************************************************/
/**
 * Return the master index stats.  There is only one portion of the master
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param master_index  The master index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static void get_master_index_stats_005(const struct master_index *master_index,
				       struct master_index_stats *dense,
				       struct master_index_stats *sparse)
{
	const struct master_index5 *mi5 =
		const_container_of(master_index, struct master_index5, common);
	struct delta_index_stats dis;
	get_delta_index_stats(&mi5->delta_index, &dis);
	dense->memory_allocated =
		(dis.memory_allocated + sizeof(struct master_index5) +
		 mi5->num_delta_lists * sizeof(uint64_t) +
		 mi5->num_zones * sizeof(struct master_index_zone));
	dense->rebalance_time = dis.rebalance_time;
	dense->rebalance_count = dis.rebalance_count;
	dense->record_count = dis.record_count;
	dense->collision_count = dis.collision_count;
	dense->discard_count = dis.discard_count;
	dense->overflow_count = dis.overflow_count;
	dense->num_lists = dis.num_lists;
	dense->early_flushes = 0;
	unsigned int z;
	for (z = 0; z < mi5->num_zones; z++) {
		dense->early_flushes += mi5->master_zones[z].num_early_flushes;
	}
	memset(sparse, 0, sizeof(struct master_index_stats));
}

/***********************************************************************/
/**
 * Determine whether a given chunk name is a hook.
 *
 * @param master_index   The master index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static bool is_master_index_sample_005(const struct master_index *master_index
				       __attribute__((unused)),
				       const struct uds_chunk_name *name
				       __attribute__((unused)))
{
	return false;
}

/***********************************************************************/
struct parameters005 {
	unsigned int address_bits;     // Number of bits in address mask
	unsigned int chapter_bits;     // Number of bits in chapter number
	unsigned int mean_delta;       // The mean delta
	unsigned long num_delta_lists; // The number of delta lists
	unsigned long num_chapters;    // Number of chapters used
	size_t num_bits_per_chapter;   // Number of bits per chapter
	size_t memory_size;            // Number of bytes of delta list memory
	size_t target_free_size;       // Number of free bytes we desire
};

/***********************************************************************/
static int
compute_master_index_parameters005(const struct configuration *config,
				   struct parameters005 *params)
{
	enum { DELTA_LIST_SIZE = 256 };
	/*
	 * For a given zone count, setting the the minimum number of delta
	 * lists to the square of the number of zones ensures that the
	 * distribution of delta lists over zones doesn't underflow, leaving
	 * the last zone with an invalid number of delta lists. See the
	 * explanation in initialize_delta_index(). Because we can restart with
	 * a different number of zones but the number of delta lists is
	 * invariant across restart, we must use the largest number of zones to
	 * compute this minimum.
	 */
	unsigned long min_delta_lists =
		(min_master_index_delta_lists ? min_master_index_delta_lists :
						MAX_ZONES * MAX_ZONES);

	struct geometry *geometry = config->geometry;
	unsigned long records_per_chapter = geometry->records_per_chapter;
	params->num_chapters = geometry->chapters_per_volume;
	unsigned long records_per_volume =
		records_per_chapter * params->num_chapters;
	unsigned int num_addresses =
		config->master_index_mean_delta * DELTA_LIST_SIZE;
	params->num_delta_lists =
		max_uint(records_per_volume / DELTA_LIST_SIZE, min_delta_lists);
	params->address_bits = compute_bits(num_addresses - 1);
	params->chapter_bits = compute_bits(params->num_chapters - 1);

	if ((unsigned int) params->num_delta_lists !=
	    params->num_delta_lists) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize master index with %lu delta lists",
						 params->num_delta_lists);
	}
	if (params->address_bits > 31) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize master index with %u address bits",
						 params->address_bits);
	}
	if (is_sparse(geometry)) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize dense master index with %u sparse chapters",
						 geometry->sparse_chapters_per_volume);
	}
	if (records_per_chapter == 0) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize master index with %lu records per chapter",
						 records_per_chapter);
	}
	if (params->num_chapters == 0) {
		return logWarningWithStringError(UDS_INVALID_ARGUMENT,
						 "cannot initialize master index with %lu chapters per volume",
						 params->num_chapters);
	}

	/*
	 * We can now compute the probability that a delta list is not touched
	 * during the writing of an entire chapter.  The computation is:
	 *
	 * double p_not_touched = pow((double) (params->num_delta_lists - 1)
	 *                          / params->num_delta_lists,
	 *                          records_per_chapter);
	 *
	 * For the standard index sizes, about 78% of the delta lists are not
	 * touched, and therefore contain dead index entries that have not been
	 * eliminated by the lazy LRU processing.  We can then compute how many
	 * dead index entries accumulate over time.  The computation is:
	 *
	 * double invalid_chapters = p_not_touched / (1.0 - p_not_touched);
	 *
	 * For the standard index sizes, we will need about 3.5 chapters of
	 * space for the dead index entries in a 1K chapter index.  Since we do
	 * not want to do that floating point computation, we use 4 chapters
	 * per 1K of chapters.
	 */
	unsigned long invalid_chapters =
		max_uint(params->num_chapters / 256, 2);
	unsigned long chapters_in_master_index =
		params->num_chapters + invalid_chapters;
	unsigned long entries_in_master_index =
		records_per_chapter * chapters_in_master_index;
	// Compute the mean delta
	unsigned long address_span = params->num_delta_lists
				     << params->address_bits;
	params->mean_delta = address_span / entries_in_master_index;
	// Project how large we expect a chapter to be
	params->num_bits_per_chapter =
		get_delta_memory_size(records_per_chapter, params->mean_delta,
				      params->chapter_bits);
	// Project how large we expect the index to be
	size_t num_bits_per_index =
		params->num_bits_per_chapter * chapters_in_master_index;
	size_t expected_index_size = num_bits_per_index / CHAR_BIT;
	/*
	 * Set the total memory to be 6% larger than the expected index size.
	 * We want this number to be large enough that the we do not do a great
	 * many rebalances as the list when the list is full.  We use
	 * MasterIndex_p1 to tune this setting.
	 */
	params->memory_size = expected_index_size * 106 / 100;
	// Set the target free size to 5% of the expected index size
	params->target_free_size = expected_index_size / 20;
	return UDS_SUCCESS;
}

/***********************************************************************/
int compute_master_index_save_bytes005(const struct configuration *config,
				       size_t *num_bytes)
{
	struct parameters005 params = { .address_bits = 0 };
	int result = compute_master_index_parameters005(config, &params);
	if (result != UDS_SUCCESS) {
		return result;
	}
	// Saving a master index 005 needs a header plus one uint64_t per delta
	// list plus the delta index.
	*num_bytes = (sizeof(struct mi005_data) +
		      params.num_delta_lists * sizeof(uint64_t) +
		      compute_delta_index_save_bytes(params.num_delta_lists,
						     params.memory_size));
	return UDS_SUCCESS;
}

/***********************************************************************/
int make_master_index005(const struct configuration *config,
			 unsigned int num_zones,
			 uint64_t volume_nonce,
			 struct master_index **master_index)
{
	struct parameters005 params = { .address_bits = 0 };
	int result = compute_master_index_parameters005(config, &params);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct master_index5 *mi5;
	result = ALLOCATE(1, struct master_index5, "master index", &mi5);
	if (result != UDS_SUCCESS) {
		*master_index = NULL;
		return result;
	}

	mi5->common.abort_restoring_master_index =
		abort_restoring_master_index_005;
	mi5->common.abort_saving_master_index = abort_saving_master_index_005;
	mi5->common.finish_saving_master_index =
		finish_saving_master_index_005;
	mi5->common.free_master_index = free_master_index_005;
	mi5->common.get_master_index_memory_used =
		get_master_index_memory_used_005;
	mi5->common.get_master_index_record = get_master_index_record_005;
	mi5->common.get_master_index_stats = get_master_index_stats_005;
	mi5->common.get_master_index_zone = get_master_index_zone_005;
	mi5->common.is_master_index_sample = is_master_index_sample_005;
	mi5->common.is_restoring_master_index_done =
		is_restoring_master_index_done_005;
	mi5->common.is_saving_master_index_done =
		is_saving_master_index_done_005;
	mi5->common.lookup_master_index_name = lookup_master_index_name_005;
	mi5->common.lookup_master_index_sampled_name =
		lookup_master_index_sampled_name_005;
	mi5->common.restore_delta_list_to_master_index =
		restore_delta_list_to_master_index_005;
	mi5->common.set_master_index_open_chapter =
		set_master_index_open_chapter_005;
	mi5->common.set_master_index_tag = set_master_index_tag_005;
	mi5->common.set_master_index_zone_open_chapter =
		set_master_index_zone_open_chapter_005;
	mi5->common.start_restoring_master_index =
		start_restoring_master_index_005;
	mi5->common.start_saving_master_index = start_saving_master_index_005;

	mi5->address_bits = params.address_bits;
	mi5->address_mask = (1u << params.address_bits) - 1;
	mi5->chapter_bits = params.chapter_bits;
	mi5->chapter_mask = (1u << params.chapter_bits) - 1;
	mi5->num_chapters = params.num_chapters;
	mi5->num_delta_lists = params.num_delta_lists;
	mi5->num_zones = num_zones;
	mi5->chapter_zone_bits = params.num_bits_per_chapter / num_zones;
	mi5->volume_nonce = volume_nonce;

	result = initialize_delta_index(&mi5->delta_index,
					num_zones,
					params.num_delta_lists,
					params.mean_delta,
					params.chapter_bits,
					params.memory_size);
	if (result == UDS_SUCCESS) {
		mi5->max_zone_bits =
			((get_delta_index_dlist_bits_allocated(&mi5->delta_index) -
				params.target_free_size * CHAR_BIT) / num_zones);
	}

	// Initialize the chapter flush ranges to be empty.  This depends upon
	// allocate returning zeroed memory.
	if (result == UDS_SUCCESS) {
		result = ALLOCATE(params.num_delta_lists,
				  uint64_t,
				  "first chapter to flush",
				  &mi5->flush_chapters);
	}

	// Initialize the virtual chapter ranges to start at zero.  This
	// depends upon allocate returning zeroed memory.
	if (result == UDS_SUCCESS) {
		result = ALLOCATE(num_zones,
				  struct master_index_zone,
				  "master index zones",
				  &mi5->master_zones);
	}

	if (result == UDS_SUCCESS) {
		*master_index = &mi5->common;
	} else {
		free_master_index_005(&mi5->common);
		*master_index = NULL;
	}
	return result;
}
