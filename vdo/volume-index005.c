// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "volume-index005.h"

#include "buffer.h"
#include "compiler.h"
#include "config.h"
#include "errors.h"
#include "geometry.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "uds.h"

/*
 * The volume index is a kept as a delta index where the payload is a
 * chapter number.  The volume index adds 2 basic functions to the delta
 * index:
 *
 *  (1) How to get the delta list number and address out of the chunk name.
 *
 *  (2) Dealing with chapter numbers, and especially the lazy flushing of
 *      chapters from the index.
 *
 * There are three ways of expressing chapter numbers: virtual, index, and
 * rolling.  The interface to the the volume index uses virtual chapter
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

struct volume_index_zone {
	uint64_t virtual_chapter_low;   /* The lowest virtual chapter indexed */
	uint64_t virtual_chapter_high;  /* The highest virtual chapter indexed */
	long num_early_flushes;         /* The number of early flushes */
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct volume_index5 {
	struct volume_index common;     /* Common volume index methods */
	struct delta_index delta_index; /* The delta index */
	uint64_t *flush_chapters;       /* The first chapter to be flushed */
	struct volume_index_zone *zones; /* The Zones */
	uint64_t volume_nonce;          /* The volume nonce */
	uint64_t chapter_zone_bits;     /* Expected size of a chapter */
				        /* (per zone) */
	uint64_t max_zone_bits;         /* Maximum size index (per zone) */
	unsigned int address_bits;      /* Number of bits in address mask */
	unsigned int address_mask;      /* Mask to get address within delta */
				        /* list */
	unsigned int chapter_bits;      /* Number of bits in chapter number */
	unsigned int chapter_mask;      /* Largest storable chapter number */
	unsigned int num_chapters;      /* Number of chapters used */
	unsigned int num_delta_lists; /* The number of delta lists */
	unsigned int num_zones; /* The number of zones */
};

struct chapter_range {
	unsigned int chapter_start;  /* The first chapter */
	unsigned int chapter_count;  /* The number of chapters */
};

/* Constants for the magic byte of a volume_index_record */
static const byte volume_index_record_magic = 0xAA;
static const byte bad_magic = 0;

/*
 * In production, the default value for min_volume_index_delta_lists will be
 * replaced by MAX_ZONES*MAX_ZONES.  Some unit tests will replace
 * min_volume_index_delta_lists with the non-default value 1, because those
 * tests really want to run with a single delta list.
 */
unsigned int min_volume_index_delta_lists;

/**
 * Extract the address from a block name.
 *
 * @param vi5   The volume index
 * @param name  The block name
 *
 * @return the address
 **/
static INLINE unsigned int extract_address(const struct volume_index5 *vi5,
					   const struct uds_chunk_name *name)
{
	return extract_volume_index_bytes(name) & vi5->address_mask;
}

/**
 * Extract the delta list number from a block name.
 *
 * @param vi5   The volume index
 * @param name  The block name
 *
 * @return the delta list number
 **/
static INLINE unsigned int extract_dlist_num(const struct volume_index5 *vi5,
					     const struct uds_chunk_name *name)
{
	uint64_t bits = extract_volume_index_bytes(name);

	return (bits >> vi5->address_bits) % vi5->num_delta_lists;
}

/**
 * Get the volume index zone containing a given volume index record
 *
 * @param record  The volume index record
 *
 * @return the volume index zone
 **/
static INLINE const struct volume_index_zone *
get_zone_for_record(const struct volume_index_record *record)
{
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
						       common);
	return &vi5->zones[record->zone_number];
}

/**
 * Convert an index chapter number to a virtual chapter number.
 *
 * @param record         The volume index record
 * @param index_chapter  The index chapter number
 *
 * @return the virtual chapter number
 **/
static INLINE uint64_t
convert_index_to_virtual(const struct volume_index_record *record,
			 unsigned int index_chapter)
{
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
						       common);
	const struct volume_index_zone *volume_index_zone =
		get_zone_for_record(record);
	unsigned int rolling_chapter =
		((index_chapter - volume_index_zone->virtual_chapter_low) &
		 vi5->chapter_mask);
	return volume_index_zone->virtual_chapter_low + rolling_chapter;
}

/**
 * Convert a virtual chapter number to an index chapter number.
 *
 * @param vi5              The volume index
 * @param virtual_chapter  The virtual chapter number
 *
 * @return the index chapter number
 **/
static INLINE unsigned int
convert_virtual_to_index(const struct volume_index5 *vi5,
			 uint64_t virtual_chapter)
{
	return virtual_chapter & vi5->chapter_mask;
}

/**
 * Determine whether a virtual chapter number is in the range being indexed
 *
 * @param record           The volume index record
 * @param virtual_chapter  The virtual chapter number
 *
 * @return true if the virtual chapter number is being indexed
 **/
static INLINE bool
is_virtual_chapter_indexed(const struct volume_index_record *record,
			   uint64_t virtual_chapter)
{
	const struct volume_index_zone *volume_index_zone =
		get_zone_for_record(record);
	return ((virtual_chapter >= volume_index_zone->virtual_chapter_low) &&
		(virtual_chapter <= volume_index_zone->virtual_chapter_high));
}

/**
 * Flush an invalid entry from the volume index, advancing to the next
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
flush_invalid_entries(struct volume_index_record *record,
		      struct chapter_range *flush_range,
		      unsigned int *next_chapter_to_invalidate)
{
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
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
			 vi5->chapter_mask);
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
static int get_volume_index_entry(struct volume_index_record *record,
				  unsigned int list_number,
				  unsigned int key,
				  struct chapter_range *flush_range)
{
	struct volume_index_record other_record;
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
						       common);
	unsigned int next_chapter_to_invalidate = vi5->chapter_mask;

	int result = start_delta_index_search(&vi5->delta_index, list_number,
					      0, &record->delta_entry);
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

	/* We probably found the record we want, but we need to keep going */
	other_record = *record;
	if (!other_record.delta_entry.at_end &&
	    (key == other_record.delta_entry.key)) {
		for (;;) {
			byte collision_name[UDS_CHUNK_NAME_SIZE];

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
			result = get_delta_entry_collision(&other_record.delta_entry,
							   collision_name);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (memcmp(collision_name,
				   record->name,
				   UDS_CHUNK_NAME_SIZE) == 0) {
				/*
				 * This collision record is the one we are
				 * looking for
				 */
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
	next_chapter_to_invalidate &= vi5->chapter_mask;
	flush_range->chapter_start = next_chapter_to_invalidate;
	flush_range->chapter_count = 0;
	return UDS_SUCCESS;
}

/**
 * Terminate and clean up the volume index
 *
 * @param volume_index The volume index to terminate
 **/
static void free_volume_index_005(struct volume_index *volume_index)
{
	if (volume_index != NULL) {
		struct volume_index5 *vi5 = container_of(volume_index,
							 struct volume_index5,
							 common);
		UDS_FREE(vi5->flush_chapters);
		vi5->flush_chapters = NULL;
		UDS_FREE(vi5->zones);
		vi5->zones = NULL;
		uninitialize_delta_index(&vi5->delta_index);
		UDS_FREE(volume_index);
	}
}

/**
 * Constants and structures for the saved volume index region. "MI5"
 * indicates volume index 005, and "-XXXX" is a number to increment
 * when the format of the data changes. The abbreviation MI5 is
 * derived from the name of a previous data structure that represented
 * the volume index region.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_START[] = "MI5-0005";

struct vi005_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START */
	uint64_t volume_nonce;
	uint64_t virtual_chapter_low;
	uint64_t virtual_chapter_high;
	unsigned int first_list;
	unsigned int num_lists;
};

/**
 * Set the tag value used when saving and/or restoring a volume index.
 *
 * @param volume_index  The volume index
 * @param tag           The tag value
 **/
static void set_volume_index_tag_005(struct volume_index *volume_index,
				     byte tag)
{
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	set_delta_index_tag(&vi5->delta_index, tag);
}

static int __must_check encode_volume_index_header(struct buffer *buffer,
						   struct vi005_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START);

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
					sizeof(struct vi005_data),
				 "%zu bytes of config written, of %zu expected",
				 content_length(buffer),
				 sizeof(struct vi005_data));
	return result;
}

/**
 * Start saving a volume index to a buffered output stream.
 *
 * @param volume_index     The volume index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_saving_volume_index_005(const struct volume_index *volume_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	int result;
	const struct volume_index5 *vi5 =
		const_container_of(volume_index, struct volume_index5, common);
	struct volume_index_zone *volume_index_zone =
		&vi5->zones[zone_number];
	unsigned int first_list =
		get_delta_zone_first_list(&vi5->delta_index, zone_number);
	unsigned int num_lists =
		get_delta_zone_list_count(&vi5->delta_index, zone_number);

	struct vi005_data header;
	uint64_t *first_flush_chapter;
	struct buffer *buffer;

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_START, MAGIC_SIZE);
	header.volume_nonce = vi5->volume_nonce;
	header.virtual_chapter_low = volume_index_zone->virtual_chapter_low;
	header.virtual_chapter_high = volume_index_zone->virtual_chapter_high;
	header.first_list = first_list;
	header.num_lists = num_lists;

	result = make_buffer(sizeof(struct vi005_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_volume_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to write volume index header");
	}

	result = make_buffer(num_lists * sizeof(uint64_t), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	first_flush_chapter = &vi5->flush_chapters[first_list];
	result = put_uint64_les_into_buffer(buffer, num_lists,
					    first_flush_chapter);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to write volume index flush ranges");
	}

	return start_saving_delta_index(&vi5->delta_index, zone_number,
					buffered_writer);
}

/**
 * Finish saving a volume index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param volume_index  The volume index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
finish_saving_volume_index_005(const struct volume_index *volume_index,
			       unsigned int zone_number)
{
	const struct volume_index5 *vi5 =
		const_container_of(volume_index, struct volume_index5, common);
	return finish_saving_delta_index(&vi5->delta_index, zone_number);
}

static int __must_check decode_volume_index_header(struct buffer *buffer,
						   struct vi005_data *header)
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
		result = UDS_CORRUPT_DATA;
	}
	return result;
}

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_restoring_volume_index_005(struct volume_index *volume_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int num_readers)
{
	unsigned int z;
	int result;
	struct volume_index5 *vi5;
	uint64_t *first_flush_chapter;
	uint64_t virtual_chapter_low = 0, virtual_chapter_high = 0;
	unsigned int i;

	if (volume_index == NULL) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cannot restore to null volume index");
	}
	vi5 = container_of(volume_index, struct volume_index5, common);
	empty_delta_index(&vi5->delta_index);

	for (i = 0; i < num_readers; i++) {
		struct buffer *buffer;
		struct vi005_data header;
		int result = make_buffer(sizeof(struct vi005_data), &buffer);

		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index header");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = decode_volume_index_header(buffer, &header);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (memcmp(header.magic, MAGIC_START, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");
		}

		if (vi5->volume_nonce == 0) {
			vi5->volume_nonce = header.volume_nonce;
		} else if (header.volume_nonce != vi5->volume_nonce) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index volume nonce incorrect");
		}

		if (i == 0) {
			virtual_chapter_low = header.virtual_chapter_low;
			virtual_chapter_high = header.virtual_chapter_high;
		} else if (virtual_chapter_high !=
			   header.virtual_chapter_high) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"Inconsistent volume index zone files: Chapter range is [%llu,%llu], chapter range %d is [%llu,%llu]",
							(unsigned long long) virtual_chapter_low,
							(unsigned long long) virtual_chapter_high,
							i,
							(unsigned long long) header.virtual_chapter_low,
							(unsigned long long) header.virtual_chapter_high);
		} else if (virtual_chapter_low < header.virtual_chapter_low) {
			virtual_chapter_low = header.virtual_chapter_low;
		}

		first_flush_chapter = &vi5->flush_chapters[header.first_list];
		result = make_buffer(header.num_lists * sizeof(uint64_t),
				     &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index flush ranges");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = get_uint64_les_from_buffer(buffer, header.num_lists,
						    first_flush_chapter);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	for (z = 0; z < vi5->num_zones; z++) {
		memset(&vi5->zones[z],
		       0,
		       sizeof(struct volume_index_zone));
		vi5->zones[z].virtual_chapter_low = virtual_chapter_low;
		vi5->zones[z].virtual_chapter_high = virtual_chapter_high;
	}

	result = start_restoring_delta_index(&vi5->delta_index,
					     buffered_readers,
					     num_readers);
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"restoring delta index failed");
	}
	return UDS_SUCCESS;
}

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
static void abort_restoring_volume_index_005(struct volume_index *volume_index)
{
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	abort_restoring_delta_index(&vi5->delta_index);
}

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
static int
finish_restoring_volume_index_005(struct volume_index *volume_index,
				  struct buffered_reader **buffered_readers,
				  unsigned int num_readers)
{
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	return finish_restoring_delta_index(&vi5->delta_index,
					    buffered_readers,
					    num_readers);
}

static void remove_newest_chapters(struct volume_index5 *vi5,
				   unsigned int zone_number,
				   uint64_t virtual_chapter)
{
	/* Get the range of delta lists belonging to this zone */
	unsigned int first_list =
		get_delta_zone_first_list(&vi5->delta_index, zone_number);
	unsigned int num_lists =
		get_delta_zone_list_count(&vi5->delta_index, zone_number);
	unsigned int last_list = first_list + num_lists - 1;

	if (virtual_chapter > vi5->chapter_mask) {
		unsigned int i;
		/*
		 * The virtual chapter number is large enough so that we can
		 * use the normal LRU mechanism without an unsigned underflow.
		 */
		virtual_chapter -= vi5->chapter_mask + 1;
		/*
		 * Eliminate the newest chapters by renumbering them to become
		 * the oldest chapters
		 */
		for (i = first_list; i <= last_list; i++) {
			if (virtual_chapter < vi5->flush_chapters[i]) {
				vi5->flush_chapters[i] = virtual_chapter;
			}
		}
	} else {
		/*
		 * Underflow will prevent the fast path.  Do it the slow and
		 * painful way.
		 */
		struct volume_index_zone *volume_index_zone =
			&vi5->zones[zone_number];
		unsigned int i;
		struct uds_chunk_name name;
		struct volume_index_record record =
			(struct volume_index_record){
				.magic = volume_index_record_magic,
				.volume_index = &vi5->common,
				.name = &name,
				.zone_number = zone_number,
			};
		struct chapter_range range;

		range.chapter_start =
			convert_virtual_to_index(vi5, virtual_chapter);
		range.chapter_count =
			(vi5->chapter_mask + 1 -
			 (virtual_chapter - volume_index_zone->virtual_chapter_low));
		memset(&name, 0, sizeof(name));
		for (i = first_list; i <= last_list; i++) {
			struct chapter_range temp_range = range;

			get_volume_index_entry(&record, i, 0, &temp_range);
		}
	}
}

/**
 * Set the open chapter number on a zone.  The volume index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param volume_index     The volume index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_volume_index_zone_open_chapter_005(struct volume_index *volume_index,
				       unsigned int zone_number,
				       uint64_t virtual_chapter)
{
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	struct volume_index_zone *volume_index_zone =
		&vi5->zones[zone_number];
	/*
	 * Take care here to avoid underflow of an unsigned value.  Note that
	 * this is the smallest valid virtual low.  We may or may not actually
	 * use this value.
	 */
	uint64_t new_virtual_low =
		(virtual_chapter >= vi5->num_chapters ?
			 virtual_chapter - vi5->num_chapters + 1 :
			 0);

	if (virtual_chapter <= volume_index_zone->virtual_chapter_low) {
		/*
		 * Moving backwards and the new range is totally before the old
		 * range. Note that moving to the lowest virtual chapter counts
		 * as totally before the old range, as we need to remove the
		 * entries in the open chapter.
		 */
		empty_delta_zone(&vi5->delta_index, zone_number);
		volume_index_zone->virtual_chapter_low = virtual_chapter;
		volume_index_zone->virtual_chapter_high = virtual_chapter;
	} else if (virtual_chapter <= volume_index_zone->virtual_chapter_high) {
		/*
		 * Moving backwards and the new range overlaps the old range.
		 * Note that moving to the same open chapter counts as
		 * backwards, as we need to remove the entries in the open
		 * chapter.
		 */
		remove_newest_chapters(vi5, zone_number, virtual_chapter);
		volume_index_zone->virtual_chapter_high = virtual_chapter;
	} else if (new_virtual_low < volume_index_zone->virtual_chapter_low) {
		/* Moving forwards and we can keep all the old chapters */
		volume_index_zone->virtual_chapter_high = virtual_chapter;
	} else if (new_virtual_low <= volume_index_zone->virtual_chapter_high) {
		/* Moving forwards and we can keep some old chapters */
		volume_index_zone->virtual_chapter_low = new_virtual_low;
		volume_index_zone->virtual_chapter_high = virtual_chapter;
	} else {
		/*
		 * Moving forwards and the new range is totally after the old
		 * range
		 */
		volume_index_zone->virtual_chapter_low = virtual_chapter;
		volume_index_zone->virtual_chapter_high = virtual_chapter;
	}
	/* Check to see if the zone data has grown to be too large */
	if (volume_index_zone->virtual_chapter_low <
	    volume_index_zone->virtual_chapter_high) {
		uint64_t used_bits =
			get_delta_zone_bits_used(&vi5->delta_index,
						 zone_number);
		if (used_bits > vi5->max_zone_bits) {
			/* Expire enough chapters to free the desired space */
			uint64_t expire_count =
				1 + (used_bits - vi5->max_zone_bits) /
					    vi5->chapter_zone_bits;
			if (expire_count == 1) {
				uds_log_ratelimit(uds_log_info,
						  "zone %u:  At chapter %llu, expiring chapter %llu early",
						  zone_number,
						  (unsigned long long) virtual_chapter,
						  (unsigned long long) volume_index_zone->virtual_chapter_low);
				volume_index_zone->num_early_flushes++;
				volume_index_zone->virtual_chapter_low++;
			} else {
				uint64_t first_expired =
					volume_index_zone->virtual_chapter_low;
				if (first_expired + expire_count <
				    volume_index_zone->virtual_chapter_high) {
					volume_index_zone->num_early_flushes +=
						expire_count;
					volume_index_zone->virtual_chapter_low +=
						expire_count;
				} else {
					volume_index_zone->num_early_flushes +=
						volume_index_zone
							->virtual_chapter_high -
						volume_index_zone
							->virtual_chapter_low;
					volume_index_zone->virtual_chapter_low =
						volume_index_zone
							->virtual_chapter_high;
				}
				uds_log_ratelimit(uds_log_info,
						  "zone %u:  At chapter %llu, expiring chapters %llu to %llu early",
						  zone_number,
						  (unsigned long long) virtual_chapter,
						  (unsigned long long) first_expired,
						  (unsigned long long) volume_index_zone->virtual_chapter_low - 1);
			}
		}
	}
}

/**
 * Set the open chapter number.  The volume index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param volume_index     The volume index
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_volume_index_open_chapter_005(struct volume_index *volume_index,
				  uint64_t virtual_chapter)
{
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	unsigned int z;

	for (z = 0; z < vi5->num_zones; z++) {
		/*
		 * In normal operation, we advance forward one chapter at a
		 * time. Log all abnormal changes.
		 */
		struct volume_index_zone *volume_index_zone = &vi5->zones[z];
		bool log_move = virtual_chapter !=
				volume_index_zone->virtual_chapter_high + 1;
		if (log_move) {
			uds_log_debug("zone %u: The range of indexed chapters is moving from [%llu, %llu] ...",
				      z,
				      (unsigned long long) volume_index_zone->virtual_chapter_low,
				      (unsigned long long) volume_index_zone->virtual_chapter_high);
		}

		set_volume_index_zone_open_chapter_005(volume_index, z,
						       virtual_chapter);

		if (log_move) {
			uds_log_debug("zone %u: ... and moving to [%llu, %llu]",
				      z,
				      (unsigned long long) volume_index_zone->virtual_chapter_low,
				      (unsigned long long) volume_index_zone->virtual_chapter_high);
		}
	}
}

/**
 * Find the volume index zone associated with a chunk name
 *
 * @param volume_index The volume index
 * @param name         The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int
get_volume_index_zone_005(const struct volume_index *volume_index,
			  const struct uds_chunk_name *name)
{
	const struct volume_index5 *vi5 =
		const_container_of(volume_index, struct volume_index5, common);
	unsigned int delta_list_number = extract_dlist_num(vi5, name);

	return get_delta_zone_number(&vi5->delta_index, delta_list_number);
}

/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param volume_index     The volume index
 * @param name             The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static uint64_t
lookup_volume_index_name_005(const struct volume_index *volume_index
			     __always_unused,
			     const struct uds_chunk_name *name
			     __always_unused)
{
	return UINT64_MAX;
}

/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param volume_index     The volume index
 * @param name             The chunk name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static uint64_t
lookup_volume_index_sampled_name_005(const struct volume_index *volume_index,
				     const struct uds_chunk_name *name)
{
	const struct volume_index5 *vi5 =
		const_container_of(volume_index, struct volume_index5, common);
	int result;
	unsigned int address = extract_address(vi5, name);
	unsigned int delta_list_number = extract_dlist_num(vi5, name);
	unsigned int zone_number =
		get_volume_index_zone_005(volume_index, name);
	const struct volume_index_zone *zone = &vi5->zones[zone_number];
	uint64_t virtual_chapter;
	unsigned int index_chapter;
	unsigned int rolling_chapter;
	struct delta_index_entry delta_entry;

	result = get_delta_index_entry(&vi5->delta_index,
				       delta_list_number,
				       address,
				       name->name,
				       &delta_entry);
	if (result != UDS_SUCCESS) {
		return UINT64_MAX;
	}

	if (delta_entry.at_end || (delta_entry.key != address)) {
		return UINT64_MAX;
	}

	index_chapter = get_delta_entry_value(&delta_entry);
	rolling_chapter = ((index_chapter - zone->virtual_chapter_low) &
			   vi5->chapter_mask);

	virtual_chapter = zone->virtual_chapter_low + rolling_chapter;
	if (virtual_chapter > zone->virtual_chapter_high) {
		return UINT64_MAX;
	}

	return virtual_chapter;
}

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this
 * entry.
 *
 * @param volume_index  The volume index to search
 * @param name          The chunk name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_volume_index_record_005(struct volume_index *volume_index,
				       const struct uds_chunk_name *name,
				       struct volume_index_record *record)
{
	int result;
	const struct volume_index_zone *volume_index_zone;
	struct volume_index5 *vi5 =
		container_of(volume_index, struct volume_index5, common);
	unsigned int address = extract_address(vi5, name);
	unsigned int delta_list_number = extract_dlist_num(vi5, name);
	uint64_t flush_chapter = vi5->flush_chapters[delta_list_number];

	record->magic = volume_index_record_magic;
	record->volume_index = volume_index;
	record->mutex = NULL;
	record->name = name;
	record->zone_number =
		get_delta_zone_number(&vi5->delta_index, delta_list_number);
	volume_index_zone = get_zone_for_record(record);

	if (flush_chapter < volume_index_zone->virtual_chapter_low) {
		struct chapter_range range;
		uint64_t flush_count =
			volume_index_zone->virtual_chapter_low - flush_chapter;
		range.chapter_start =
			convert_virtual_to_index(vi5, flush_chapter);
		range.chapter_count = (flush_count > vi5->chapter_mask ?
					       vi5->chapter_mask + 1 :
					       flush_count);
		result = get_volume_index_entry(record, delta_list_number,
						address, &range);
		flush_chapter =
			convert_index_to_virtual(record, range.chapter_start);
		if (flush_chapter > volume_index_zone->virtual_chapter_high) {
			flush_chapter = volume_index_zone->virtual_chapter_high;
		}
		vi5->flush_chapters[delta_list_number] = flush_chapter;
	} else {
		result = get_delta_index_entry(&vi5->delta_index,
					       delta_list_number,
					       address,
					       name->name,
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

/**
 * Create a new record associated with a block name.
 *
 * @param record           The volume index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int put_volume_index_record(struct volume_index_record *record,
			    uint64_t virtual_chapter)
{
	int result;
	unsigned int address;
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
						       common);
	if (record->magic != volume_index_record_magic) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct volume_index_zone *volume_index_zone =
			get_zone_for_record(record);
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot put record into chapter number %llu that is out of the valid range %llu to %llu",
						(unsigned long long) virtual_chapter,
						(unsigned long long) volume_index_zone->virtual_chapter_low,
						(unsigned long long) volume_index_zone->virtual_chapter_high);
	}
	address = extract_address(vi5, record->name);
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = put_delta_index_entry(&record->delta_entry,
				       address,
				       convert_virtual_to_index(vi5, virtual_chapter),
				       record->is_found ? record->name->name : NULL);
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	switch (result) {
	case UDS_SUCCESS:
		record->virtual_chapter = virtual_chapter;
		record->is_collision = record->delta_entry.is_collision;
		record->is_found = true;
		break;
	case UDS_OVERFLOW:
		uds_log_ratelimit(uds_log_warning_strerror,
				  UDS_OVERFLOW,
				  "Volume index entry dropped due to overflow condition");
		log_delta_index_entry(&record->delta_entry);
		break;
	default:
		break;
	}
	return result;
}

static INLINE int validate_record(struct volume_index_record *record)
{
	if (record->magic != volume_index_record_magic) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
	}
	if (!record->is_found) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"illegal operation on new record");
	}
	return UDS_SUCCESS;
}

/**
 * Remove an existing record.
 *
 * @param record      The volume index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int remove_volume_index_record(struct volume_index_record *record)
{
	int result = validate_record(record);

	if (result != UDS_SUCCESS) {
		return result;
	}
	/* Mark the record so that it cannot be used again */
	record->magic = bad_magic;
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = remove_delta_index_entry(&record->delta_entry);
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	return result;
}

int set_volume_index_record_chapter(struct volume_index_record *record,
				    uint64_t virtual_chapter)
{
	const struct volume_index5 *vi5 = container_of(record->volume_index,
						       struct volume_index5,
						       common);
	int result = validate_record(record);

	if (result != UDS_SUCCESS) {
		return result;
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct volume_index_zone *volume_index_zone =
			get_zone_for_record(record);
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot set chapter number %llu that is out of the valid range %llu to %llu",
						(unsigned long long) virtual_chapter,
						(unsigned long long) volume_index_zone->virtual_chapter_low,
						(unsigned long long) volume_index_zone->virtual_chapter_high);
	}
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = set_delta_entry_value(&record->delta_entry,
				       convert_virtual_to_index(vi5,
								virtual_chapter));
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	record->virtual_chapter = virtual_chapter;
	return UDS_SUCCESS;
}

/**
 * Return the volume index stats.  There is only one portion of the volume
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param volume_index  The volume index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
static void get_volume_index_stats_005(const struct volume_index *volume_index,
				       struct volume_index_stats *dense,
				       struct volume_index_stats *sparse)
{
	const struct volume_index5 *vi5 =
		const_container_of(volume_index, struct volume_index5, common);
	struct delta_index_stats dis;
	unsigned int z;

	get_delta_index_stats(&vi5->delta_index, &dis);
	dense->memory_allocated =
		(dis.memory_allocated + sizeof(struct volume_index5) +
		 vi5->num_delta_lists * sizeof(uint64_t) +
		 vi5->num_zones * sizeof(struct volume_index_zone));
	dense->rebalance_time = dis.rebalance_time;
	dense->rebalance_count = dis.rebalance_count;
	dense->record_count = dis.record_count;
	dense->collision_count = dis.collision_count;
	dense->discard_count = dis.discard_count;
	dense->overflow_count = dis.overflow_count;
	dense->num_lists = dis.list_count;
	dense->early_flushes = 0;
	for (z = 0; z < vi5->num_zones; z++) {
		dense->early_flushes += vi5->zones[z].num_early_flushes;
	}
	memset(sparse, 0, sizeof(struct volume_index_stats));
}

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param volume_index   The volume index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static bool is_volume_index_sample_005(const struct volume_index *volume_index
				       __always_unused,
				       const struct uds_chunk_name *name
				       __always_unused)
{
	return false;
}

struct parameters005 {
	unsigned int address_bits;     /* Number of bits in address mask */
	unsigned int chapter_bits;     /* Number of bits in chapter number */
	unsigned int mean_delta;       /* The mean delta */
	unsigned long num_delta_lists; /* The number of delta lists */
	unsigned long num_chapters;    /* Number of chapters used */
	size_t num_bits_per_chapter;   /* Number of bits per chapter */
	size_t memory_size;            /* Number of bytes of delta list memory */
	size_t target_free_size;       /* Number of free bytes we desire */
};

static int
compute_volume_index_parameters005(const struct configuration *config,
				   struct parameters005 *params)
{
	enum { DELTA_LIST_SIZE = 256 };
	unsigned long invalid_chapters, address_span;
	unsigned long chapters_in_volume_index, entries_in_volume_index;
	unsigned long rounded_chapters;
	unsigned long delta_list_records;
	unsigned int num_addresses;
	uint64_t num_bits_per_index;
	size_t expected_index_size;
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
		(min_volume_index_delta_lists ? min_volume_index_delta_lists :
						MAX_ZONES * MAX_ZONES);

	struct geometry *geometry = config->geometry;
	unsigned long records_per_chapter = geometry->records_per_chapter;

	params->num_chapters = geometry->chapters_per_volume;
	/*
	 * Make sure that the number of delta list records in the
	 * volume index does not change when the volume is reduced by
	 * one chapter. This preserves the mapping from hash to volume
	 * index delta list.
	 */
	rounded_chapters = params->num_chapters;
	if (is_reduced_geometry(geometry))
		rounded_chapters += 1;
	delta_list_records = records_per_chapter * rounded_chapters;
	num_addresses = config->volume_index_mean_delta * DELTA_LIST_SIZE;
	params->num_delta_lists =
		max(delta_list_records / DELTA_LIST_SIZE, min_delta_lists);
	params->address_bits = compute_bits(num_addresses - 1);
	params->chapter_bits = compute_bits(rounded_chapters - 1);
	if ((unsigned int) params->num_delta_lists !=
	    params->num_delta_lists) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu delta lists",
						params->num_delta_lists);
	}
	if (params->address_bits > 31) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %u address bits",
						params->address_bits);
	}
	if (is_sparse_geometry(geometry)) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize dense volume index with %u sparse chapters",
						geometry->sparse_chapters_per_volume);
	}
	if (records_per_chapter == 0) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu records per chapter",
						records_per_chapter);
	}
	if (params->num_chapters == 0) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu chapters per volume",
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
	invalid_chapters = max(rounded_chapters / 256, 2UL);
	chapters_in_volume_index = rounded_chapters + invalid_chapters;
	entries_in_volume_index =
		records_per_chapter * chapters_in_volume_index;
	/* Compute the mean delta */
	address_span =
		(uint64_t) params->num_delta_lists << params->address_bits;
	params->mean_delta = address_span / entries_in_volume_index;
	/* Project how large we expect a chapter to be */
	params->num_bits_per_chapter =
		compute_delta_index_size(records_per_chapter,
					 params->mean_delta,
					 params->chapter_bits);
	/* Project how large we expect the index to be */
	num_bits_per_index =
		params->num_bits_per_chapter * chapters_in_volume_index;
	expected_index_size = num_bits_per_index / CHAR_BIT;
	/*
	 * Set the total memory to be 6% larger than the expected index size.
	 * We want this number to be large enough that the we do not do a great
	 * many rebalances as the list when the list is full.  We use
	 * VolumeIndex_p1 to tune this setting.
	 */
	params->memory_size = expected_index_size * 106 / 100;
	/* Set the target free size to 5% of the expected index size */
	params->target_free_size = expected_index_size / 20;
	return UDS_SUCCESS;
}

int compute_volume_index_save_bytes005(const struct configuration *config,
				       size_t *num_bytes)
{
	struct parameters005 params = { .address_bits = 0 };
	int result = compute_volume_index_parameters005(config, &params);

	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * Saving a volume index 005 needs a header plus one uint64_t per delta
	 * list plus the delta index.
	 */
	*num_bytes = (sizeof(struct vi005_data) +
		      params.num_delta_lists * sizeof(uint64_t) +
		      compute_delta_index_save_bytes(params.num_delta_lists,
						     params.memory_size));
	return UDS_SUCCESS;
}

int make_volume_index005(const struct configuration *config,
			 uint64_t volume_nonce,
			 struct volume_index **volume_index)
{
	struct volume_index5 *vi5;
	struct parameters005 params = { .address_bits = 0 };
	unsigned int num_zones = config->zone_count;
	int result = compute_volume_index_parameters005(config, &params);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct volume_index5, "volume index", &vi5);
	if (result != UDS_SUCCESS) {
		*volume_index = NULL;
		return result;
	}

	vi5->common.abort_restoring_volume_index =
		abort_restoring_volume_index_005;
	vi5->common.finish_restoring_volume_index =
		finish_restoring_volume_index_005;
	vi5->common.finish_saving_volume_index =
		finish_saving_volume_index_005;
	vi5->common.free_volume_index = free_volume_index_005;
	vi5->common.get_volume_index_record = get_volume_index_record_005;
	vi5->common.get_volume_index_stats = get_volume_index_stats_005;
	vi5->common.get_volume_index_zone = get_volume_index_zone_005;
	vi5->common.is_volume_index_sample = is_volume_index_sample_005;
	vi5->common.lookup_volume_index_name = lookup_volume_index_name_005;
	vi5->common.lookup_volume_index_sampled_name =
		lookup_volume_index_sampled_name_005;
	vi5->common.set_volume_index_open_chapter =
		set_volume_index_open_chapter_005;
	vi5->common.set_volume_index_tag = set_volume_index_tag_005;
	vi5->common.set_volume_index_zone_open_chapter =
		set_volume_index_zone_open_chapter_005;
	vi5->common.start_restoring_volume_index =
		start_restoring_volume_index_005;
	vi5->common.start_saving_volume_index = start_saving_volume_index_005;

	vi5->address_bits = params.address_bits;
	vi5->address_mask = (1u << params.address_bits) - 1;
	vi5->chapter_bits = params.chapter_bits;
	vi5->chapter_mask = (1u << params.chapter_bits) - 1;
	vi5->num_chapters = params.num_chapters;
	vi5->num_delta_lists = params.num_delta_lists;
	vi5->num_zones = num_zones;
	vi5->chapter_zone_bits = params.num_bits_per_chapter / num_zones;
	vi5->volume_nonce = volume_nonce;

	result = initialize_delta_index(&vi5->delta_index,
					num_zones,
					params.num_delta_lists,
					params.mean_delta,
					params.chapter_bits,
					params.memory_size);
	if (result == UDS_SUCCESS) {
		vi5->max_zone_bits =
			((get_delta_index_bits_allocated(&vi5->delta_index) -
				params.target_free_size * CHAR_BIT) / num_zones);
	}

	/*
	 * Initialize the chapter flush ranges to be empty.  This depends upon
	 * allocate returning zeroed memory.
	 */
	if (result == UDS_SUCCESS) {
		result = UDS_ALLOCATE(params.num_delta_lists,
				      uint64_t,
				      "first chapter to flush",
				      &vi5->flush_chapters);
	}

	/*
	 * Initialize the virtual chapter ranges to start at zero.  This
	 * depends upon allocate returning zeroed memory.
	 */
	if (result == UDS_SUCCESS) {
		result = UDS_ALLOCATE(num_zones,
				      struct volume_index_zone,
				      "volume index zones",
				      &vi5->zones);
	}

	if (result == UDS_SUCCESS) {
		*volume_index = &vi5->common;
	} else {
		free_volume_index_005(&vi5->common);
		*volume_index = NULL;
	}
	return result;
}
