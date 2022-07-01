// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "open-chapter.h"

#include "compiler.h"
#include "config.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

static const byte OPEN_CHAPTER_MAGIC[] = "ALBOC";
static const byte OPEN_CHAPTER_VERSION[] = "02.00";

enum {
	OPEN_CHAPTER_MAGIC_LENGTH = sizeof(OPEN_CHAPTER_MAGIC) - 1,
	OPEN_CHAPTER_VERSION_LENGTH = sizeof(OPEN_CHAPTER_VERSION) - 1
};

static INLINE size_t records_size(const struct open_chapter_zone *open_chapter)
{
	return (sizeof(struct uds_chunk_record) *
		(1 + open_chapter->capacity));
}

static INLINE size_t slots_size(size_t slot_count)
{
	return (sizeof(struct open_chapter_zone_slot) * slot_count);
}

/**
 * Round up to the first power of two greater than or equal
 * to the supplied number.
 *
 * @param val  the number to round up
 *
 * @return the first power of two not smaller than val for any
 *         val <= 2^63
 **/
static INLINE size_t next_power_of_two(size_t val)
{
	if (val == 0) {
		return 1;
	}
	return (1 << compute_bits(val - 1));
}

int make_open_chapter(const struct geometry *geometry,
		      unsigned int zone_count,
		      struct open_chapter_zone **open_chapter_ptr)
{
	struct open_chapter_zone *open_chapter;
	size_t capacity, slot_count;
	int result = ASSERT(zone_count > 0, "zone count must be > 0");

	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		ASSERT_WITH_ERROR_CODE(geometry->open_chapter_load_ratio > 1,
				       UDS_BAD_STATE,
				       "Open chapter hash table is too small");
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_WITH_ERROR_CODE((geometry->records_per_chapter <=
						OPEN_CHAPTER_MAX_RECORD_NUMBER),
					UDS_BAD_STATE,
					"Too many records (%u) for a single chapter",
					geometry->records_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (geometry->records_per_chapter < zone_count) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "zone count: %u is larger than the records per chapter %u",
					      zone_count,
					      geometry->records_per_chapter);
	}
	capacity = geometry->records_per_chapter / zone_count;

	/*
	 * The slot count must be at least one greater than the capacity.
	 * Using a power of two slot count guarantees that hash insertion
	 * will never fail if the hash table is not full.
	 */
	slot_count = next_power_of_two(capacity *
				       geometry->open_chapter_load_ratio);
	result = UDS_ALLOCATE_EXTENDED(struct open_chapter_zone,
				       slot_count,
				       struct open_chapter_zone_slot,
				       "open chapter",
				       &open_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	open_chapter->slot_count = slot_count;
	open_chapter->capacity = capacity;
	result = uds_allocate_cache_aligned(records_size(open_chapter),
					    "record pages",
					    &open_chapter->records);
	if (result != UDS_SUCCESS) {
		free_open_chapter(open_chapter);
		return result;
	}

	*open_chapter_ptr = open_chapter;
	return UDS_SUCCESS;
}

size_t open_chapter_size(const struct open_chapter_zone *open_chapter)
{
	return open_chapter->size - open_chapter->deleted;
}

void reset_open_chapter(struct open_chapter_zone *open_chapter)
{
	open_chapter->size = 0;
	open_chapter->deleted = 0;

	memset(open_chapter->records, 0, records_size(open_chapter));
	memset(open_chapter->slots, 0, slots_size(open_chapter->slot_count));
}

static struct uds_chunk_record *
probe_chapter_slots(struct open_chapter_zone *open_chapter,
		    const struct uds_chunk_name *name,
		    unsigned int *slot_ptr,
		    unsigned int *record_number_ptr)
{
	unsigned int slots = open_chapter->slot_count;
	unsigned int probe = name_to_hash_slot(name, slots);
	unsigned int first_slot = 0;

	struct uds_chunk_record *record;
	unsigned int probe_slot;
	unsigned int record_number;
	unsigned int probe_attempts;

	for (probe_attempts = 1;; ++probe_attempts) {
		probe_slot = first_slot + probe;
		record_number = open_chapter->slots[probe_slot].record_number;

		/*
		 * If the hash slot is empty, we've reached the end of a chain
		 * without finding the record and should terminate the search.
		 */
		if (record_number == 0) {
			record = NULL;
			break;
		}

		/*
		 * If the name of the record referenced by the slot matches and
		 * has not been deleted, then we've found the requested name.
		 */
		record = &open_chapter->records[record_number];
		if ((memcmp(&record->name, name, UDS_CHUNK_NAME_SIZE) == 0) &&
		    !open_chapter->slots[record_number].record_deleted) {
			break;
		}

		/*
		 * Quadratic probing: advance the probe by 1, 2, 3, etc. and
		 * try again. This performs better than linear probing and
		 * works best for 2^N slots.
		 */
		probe += probe_attempts;
		if (probe >= slots) {
			probe = probe % slots;
		}
	}

	/*
	 * These NULL checks will be optimized away in callers who don't care
	 * about the values when this function is inlined.
	 */
	if (slot_ptr != NULL) {
		*slot_ptr = probe_slot;
	}
	if (record_number_ptr != NULL) {
		*record_number_ptr = record_number;
	}

	return record;
}

void search_open_chapter(struct open_chapter_zone *open_chapter,
			 const struct uds_chunk_name *name,
			 struct uds_chunk_data *metadata,
			 bool *found)
{
	struct uds_chunk_record *record =
		probe_chapter_slots(open_chapter, name, NULL, NULL);

	if (record == NULL) {
		*found = false;
	} else {
		*found = true;
		if (metadata != NULL) {
			*metadata = record->data;
		}
	}
}

int put_open_chapter(struct open_chapter_zone *open_chapter,
		     const struct uds_chunk_name *name,
		     const struct uds_chunk_data *metadata,
		     unsigned int *remaining)
{
	unsigned int slot, record_number;
	struct uds_chunk_record *record =
		probe_chapter_slots(open_chapter, name, &slot, NULL);

	if (record != NULL) {
		record->data = *metadata;
		*remaining = open_chapter->capacity - open_chapter->size;
		return UDS_SUCCESS;
	}

	if (open_chapter->size >= open_chapter->capacity) {
		return UDS_VOLUME_OVERFLOW;
	}

	record_number = ++open_chapter->size;
	open_chapter->slots[slot].record_number = record_number;
	record = &open_chapter->records[record_number];
	record->name = *name;
	record->data = *metadata;

	*remaining = open_chapter->capacity - open_chapter->size;
	return UDS_SUCCESS;
}

void remove_from_open_chapter(struct open_chapter_zone *open_chapter,
			      const struct uds_chunk_name *name)
{
	unsigned int record_number;
	struct uds_chunk_record *record =
		probe_chapter_slots(open_chapter, name, NULL, &record_number);

	if (record == NULL) {
		return;
	}

	/*
	 * Set the deleted flag on the record_number in the slot array so
	 * search won't find it and close won't index it.
	 */
	open_chapter->slots[record_number].record_deleted = true;
	open_chapter->deleted += 1;
}

void free_open_chapter(struct open_chapter_zone *open_chapter)
{
	if (open_chapter != NULL) {
		UDS_FREE(open_chapter->records);
		UDS_FREE(open_chapter);
	}
}

static int fill_delta_chapter_index(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct open_chapter_index *index,
				    struct uds_chunk_record *collated_records)
{
	/*
	 * Find a record to replace any deleted records, and fill the chapter
	 * if it was closed early. The last record in any filled zone is
	 * guaranteed to not have been deleted in this chapter, so use one of
	 * those.
	 */
	struct open_chapter_zone *fill_chapter_zone = NULL;
	struct uds_chunk_record *fill_record = NULL;
	unsigned int z, pages_per_chapter, records_per_page, page;
	unsigned int records_added = 0, zone = 0;
	int result, overflow_count = 0;
	const struct geometry *geometry;

	for (z = 0; z < zone_count; ++z) {
		fill_chapter_zone = chapter_zones[z];
		if (fill_chapter_zone->size == fill_chapter_zone->capacity) {
			fill_record =
				&fill_chapter_zone
					 ->records[fill_chapter_zone->size];
			break;
		}
	}
	result =
		ASSERT((fill_record != NULL), "some open chapter zone filled");
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT(!fill_chapter_zone->slots[fill_chapter_zone->size]
				 .record_deleted,
			"chapter fill record not deleted");
	if (result != UDS_SUCCESS) {
		return result;
	}

	geometry = index->geometry;
	pages_per_chapter = geometry->record_pages_per_chapter;
	records_per_page = geometry->records_per_page;

	for (page = 0; page < pages_per_chapter; page++) {
		unsigned int i;

		for (i = 0; i < records_per_page;
		     i++, records_added++, zone = (zone + 1) % zone_count) {
			struct uds_chunk_record *next_record;
			/* The record arrays are 1-based. */
			unsigned int record_number =
				1 + (records_added / zone_count);

			/*
			 * If the zone has been exhausted, or the record was
			 * deleted, add the fill record to the chapter.
			 */
			if (record_number > chapter_zones[zone]->size ||
			    chapter_zones[zone]
				    ->slots[record_number]
				    .record_deleted) {
				collated_records[1 + records_added] =
					*fill_record;
				continue;
			}

			next_record =
				&chapter_zones[zone]->records[record_number];
			collated_records[1 + records_added] = *next_record;

			result = put_open_chapter_index_record(index,
								   &next_record->name,
								   page);
			switch (result) {
			case UDS_SUCCESS:
				break;
			case UDS_OVERFLOW:
				overflow_count++;
				break;
			default:
				uds_log_error_strerror(result,
						       "failed to build open chapter index");
				return result;
			}
		}
	}
	if (overflow_count > 0) {
		uds_log_warning("Failed to add %d entries to chapter index",
				overflow_count);
	}
	return UDS_SUCCESS;
}

int close_open_chapter(struct open_chapter_zone **chapter_zones,
		       unsigned int zone_count,
		       struct volume *volume,
		       struct open_chapter_index *chapter_index,
		       struct uds_chunk_record *collated_records,
		       uint64_t virtual_chapter_number)
{
	int result;

	/*
	 * Empty the delta chapter index, and prepare it for the new virtual
	 * chapter.
	 */
	empty_open_chapter_index(chapter_index, virtual_chapter_number);

	/*
	 * Map each non-deleted record name to its record page number in the
	 * delta chapter index.
	 */
	result = fill_delta_chapter_index(chapter_zones, zone_count,
					  chapter_index, collated_records);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Pass the populated chapter index and the records to the volume,
	 * which will generate and write the index and record pages for the
	 * chapter.
	 */
	return write_chapter(volume, chapter_index, collated_records);
}

int save_open_chapters(struct uds_index *index, struct buffered_writer *writer)
{
	uint32_t total_records = 0, records_added = 0;
	unsigned int i, record_index;
	byte total_record_data[sizeof(total_records)];
	int result = write_to_buffered_writer(writer, OPEN_CHAPTER_MAGIC,
					      OPEN_CHAPTER_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = write_to_buffered_writer(writer, OPEN_CHAPTER_VERSION,
					  OPEN_CHAPTER_VERSION_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < index->zone_count; i++) {
		total_records +=
			open_chapter_size(index->zones[i]->open_chapter);
	}

	/* Store the record count in little-endian order. */
	put_unaligned_le32(total_records, total_record_data);

	result = write_to_buffered_writer(writer, total_record_data,
					  sizeof(total_record_data));
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Only write out the records that have been added and not deleted. */
	record_index = 1;
	while (records_added < total_records) {
		unsigned int i;

		for (i = 0; i < index->zone_count; i++) {
			struct open_chapter_zone *open_chapter =
				index->zones[i]->open_chapter;
			struct uds_chunk_record *record;

			if (record_index > open_chapter->size) {
				continue;
			}
			if (open_chapter->slots[record_index].record_deleted) {
				continue;
			}
			record = &open_chapter->records[record_index];
			result = write_to_buffered_writer(writer,
							  record,
							  sizeof(struct uds_chunk_record));
			if (result != UDS_SUCCESS) {
				return result;
			}
			records_added++;
		}
		record_index++;
	}

	return flush_buffered_writer(writer);
}

uint64_t compute_saved_open_chapter_size(struct geometry *geometry)
{
	return OPEN_CHAPTER_MAGIC_LENGTH + OPEN_CHAPTER_VERSION_LENGTH +
	       sizeof(uint32_t) +
	       geometry->records_per_chapter * sizeof(struct uds_chunk_record);
}

/**
 * Read the version field from a buffered reader, checking whether it is a
 * supported version. Returns (via a pointer parameter) the matching
 * version constant, which can be used by comparing to the version
 * constants using simple pointer equality.
 *
 * @param [in]  reader  A buffered reader.
 * @param [out] version The version constant that was matched.
 *
 * @return UDS_SUCCESS or an error code if the file could not be read or
 *         the version is invalid or unsupported
 **/
static int read_version(struct buffered_reader *reader, const byte **version)
{
	byte buffer[OPEN_CHAPTER_VERSION_LENGTH];
	int result = read_from_buffered_reader(reader, buffer, sizeof(buffer));

	if (result != UDS_SUCCESS) {
		return result;
	}
	if (memcmp(OPEN_CHAPTER_VERSION, buffer, sizeof(buffer)) != 0) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "Invalid open chapter version: %.*s",
					      (int) sizeof(buffer),
					      buffer);
	}
	*version = OPEN_CHAPTER_VERSION;
	return UDS_SUCCESS;
}

static int load_version20(struct uds_index *index,
			  struct buffered_reader *reader)
{
	uint32_t num_records, records;
	byte num_records_data[sizeof(uint32_t)];
	struct uds_chunk_record record;

	/* Keep track of which zones cannot accept any more records. */
	bool full_flags[MAX_ZONES] = {
		false,
	};

	int result = read_from_buffered_reader(reader, &num_records_data,
					       sizeof(num_records_data));
	if (result != UDS_SUCCESS) {
		return result;
	}
	num_records = get_unaligned_le32(num_records_data);

	/* Assign records to the correct zones. */
	for (records = 0; records < num_records; records++) {
		unsigned int zone = 0;

		result = read_from_buffered_reader(reader, &record,
						   sizeof(struct uds_chunk_record));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (index->zone_count > 1) {
			/*
			 * A read-only index has no volume index, but it also
			 * has only one zone.
			 */
			zone = get_volume_index_zone(index->volume_index,
						     &record.name);
		}
		/*
		 * Add records until the open chapter zone almost runs out of
		 * space. The chapter can't be closed here, so don't add the
		 * last record.
		 */
		if (!full_flags[zone]) {
			unsigned int remaining;

			result = put_open_chapter(index->zones[zone]->open_chapter,
						  &record.name,
						  &record.data,
						  &remaining);
			full_flags[zone] = (remaining <= 1);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	}

	return UDS_SUCCESS;
}

int load_open_chapters(struct uds_index *index, struct buffered_reader *reader)
{
	const byte *version = NULL;
	/* Read and check the magic number. */
	int result = verify_buffered_data(reader, OPEN_CHAPTER_MAGIC,
					  OPEN_CHAPTER_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Read and check the version. */
	result = read_version(reader, &version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return load_version20(index, reader);
}
