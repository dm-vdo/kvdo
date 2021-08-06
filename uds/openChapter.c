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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/openChapter.c#1 $
 */

#include "openChapter.h"

#include "compiler.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "zone.h"

static int read_open_chapters(struct read_portal *portal);
static int write_open_chapters(struct index_component *component,
			       struct buffered_writer *writer,
			       unsigned int zone);

const struct index_component_info OPEN_CHAPTER_INFO = {
	.kind = RL_KIND_OPEN_CHAPTER,
	.name = "open chapter",
	.save_only = true,
	.chapter_sync = false,
	.multi_zone = false,
	.io_storage = true,
	.loader = read_open_chapters,
	.saver = write_open_chapters,
	.incremental = NULL,
};

static const byte OPEN_CHAPTER_MAGIC[] = "ALBOC";
static const byte OPEN_CHAPTER_VERSION[] = "02.00";

enum {
	OPEN_CHAPTER_MAGIC_LENGTH = sizeof(OPEN_CHAPTER_MAGIC) - 1,
	OPEN_CHAPTER_VERSION_LENGTH = sizeof(OPEN_CHAPTER_VERSION) - 1
};

/**********************************************************************/
static int fill_delta_chapter_index(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct open_chapter_index *index,
				    struct uds_chunk_record *collated_records)
{
	// Find a record to replace any deleted records, and fill the chapter
	// if it was closed early. The last record in any filled zone is
	// guaranteed to not have been deleted in this chapter, so use one of
	// those.
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
			// The record arrays are 1-based.
			unsigned int record_number =
				1 + (records_added / zone_count);

			// If the zone has been exhausted, or the record was
			// deleted, add the fill record to the chapter.
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

/**********************************************************************/
int close_open_chapter(struct open_chapter_zone **chapter_zones,
		       unsigned int zone_count,
		       struct volume *volume,
		       struct open_chapter_index *chapter_index,
		       struct uds_chunk_record *collated_records,
		       uint64_t virtual_chapter_number)
{
	int result;

	// Empty the delta chapter index, and prepare it for the new virtual
	// chapter.
	empty_open_chapter_index(chapter_index, virtual_chapter_number);

	// Map each non-deleted record name to its record page number in the
	// delta chapter index.
	result = fill_delta_chapter_index(chapter_zones, zone_count,
					  chapter_index, collated_records);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Pass the populated chapter index and the records to the volume,
	// which will generate and write the index and record pages for the
	// chapter.
	return write_chapter(volume, chapter_index, collated_records);
}

/**********************************************************************/
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

	// Store the record count in little-endian order.
	put_unaligned_le32(total_records, total_record_data);

	result = write_to_buffered_writer(writer, total_record_data,
					  sizeof(total_record_data));
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Only write out the records that have been added and not deleted.
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

/**********************************************************************/
uint64_t compute_saved_open_chapter_size(struct geometry *geometry)
{
	return OPEN_CHAPTER_MAGIC_LENGTH + OPEN_CHAPTER_VERSION_LENGTH +
	       sizeof(uint32_t) +
	       geometry->records_per_chapter * sizeof(struct uds_chunk_record);
}

/**********************************************************************/
static int write_open_chapters(struct index_component *component,
			       struct buffered_writer *writer,
			       unsigned int zone)
{
	struct uds_index *index;
	int result = ASSERT((zone == 0), "open chapter write not zoned");
	if (result != UDS_SUCCESS) {
		return result;
	}

	index = index_component_data(component);
	return save_open_chapters(index, writer);
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
		return uds_log_error_strerror(UDS_CORRUPT_COMPONENT,
					      "Invalid open chapter version: %.*s",
					      (int) sizeof(buffer),
					      buffer);
	}
	*version = OPEN_CHAPTER_VERSION;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int load_version20(struct uds_index *index,
			  struct buffered_reader *reader)
{
	uint32_t num_records, records;
	byte num_records_data[sizeof(uint32_t)];
	struct uds_chunk_record record;

	// Keep track of which zones cannot accept any more records.
	bool full_flags[MAX_ZONES] = {
		false,
	};

	int result = read_from_buffered_reader(reader, &num_records_data,
					       sizeof(num_records_data));
	if (result != UDS_SUCCESS) {
		return result;
	}
	num_records = get_unaligned_le32(num_records_data);

	// Assign records to the correct zones.
	for (records = 0; records < num_records; records++) {
		unsigned int zone = 0;
		result = read_from_buffered_reader(reader, &record,
						   sizeof(struct uds_chunk_record));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (index->zone_count > 1) {
			// A read-only index has no volume index, but it also
			// has only one zone.
			zone = get_volume_index_zone(index->volume_index,
						     &record.name);
		}
		// Add records until the open chapter zone almost runs out of
		// space. The chapter can't be closed here, so don't add the
		// last record.
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

/**********************************************************************/
int load_open_chapters(struct uds_index *index, struct buffered_reader *reader)
{
	const byte *version = NULL;
	// Read and check the magic number.
	int result = verify_buffered_data(reader, OPEN_CHAPTER_MAGIC,
					  OPEN_CHAPTER_MAGIC_LENGTH);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Read and check the version.
	result = read_version(reader, &version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return load_version20(index, reader);
}

/**********************************************************************/
int read_open_chapters(struct read_portal *portal)
{
	struct uds_index *index = index_component_data(portal->component);

	struct buffered_reader *reader;
	int result = get_buffered_reader_for_portal(portal, 0, &reader);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return load_open_chapters(index, reader);
}
