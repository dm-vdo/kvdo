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
 * $Id: //eng/uds-releases/krusty/src/uds/openChapterZone.c#10 $
 */

#include "openChapterZone.h"

#include "compiler.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

/**********************************************************************/
static INLINE size_t records_size(const struct open_chapter_zone *open_chapter)
{
	return (sizeof(struct uds_chunk_record) *
		(1 + open_chapter->capacity));
}

/**********************************************************************/
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

/**********************************************************************/
int make_open_chapter(const struct geometry *geometry,
		      unsigned int zone_count,
		      struct open_chapter_zone **open_chapter_ptr)
{
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
		return logUnrecoverable(UDS_INVALID_ARGUMENT,
					"zone count: %u is larger than the records per chapter %u",
			zone_count,
			geometry->records_per_chapter);
	}
	size_t capacity = geometry->records_per_chapter / zone_count;

	// The slot count must be at least one greater than the capacity.
	// Using a power of two slot count guarantees that hash insertion
	// will never fail if the hash table is not full.
	size_t slot_count = next_power_of_two(capacity *
						geometry->open_chapter_load_ratio);
	struct open_chapter_zone *open_chapter;
	result = ALLOCATE_EXTENDED(struct open_chapter_zone,
				   slot_count,
				   struct open_chapter_zone_slot,
				   "open chapter",
				   &open_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	open_chapter->slot_count = slot_count;
	open_chapter->capacity = capacity;
	result = allocateCacheAligned(records_size(open_chapter),
				      "record pages",
				      &open_chapter->records);
	if (result != UDS_SUCCESS) {
		free_open_chapter(open_chapter);
		return result;
	}

	*open_chapter_ptr = open_chapter;
	return UDS_SUCCESS;
}

/**********************************************************************/
size_t open_chapter_size(const struct open_chapter_zone *open_chapter)
{
	return open_chapter->size - open_chapter->deleted;
}

/**********************************************************************/
void reset_open_chapter(struct open_chapter_zone *open_chapter)
{
	open_chapter->size = 0;
	open_chapter->deleted = 0;

	memset(open_chapter->records, 0, records_size(open_chapter));
	memset(open_chapter->slots, 0, slots_size(open_chapter->slot_count));
}

/**********************************************************************/
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

		// If the hash slot is empty, we've reached the end of a chain
		// without finding the record and should terminate the search.
		if (record_number == 0) {
			record = NULL;
			break;
		}

		// If the name of the record referenced by the slot matches and
		// has not been deleted, then we've found the requested name.
		record = &open_chapter->records[record_number];
		if ((memcmp(&record->name, name, UDS_CHUNK_NAME_SIZE) == 0) &&
		    !open_chapter->slots[record_number].record_deleted) {
			break;
		}

		// Quadratic probing: advance the probe by 1, 2, 3, etc. and
		// try again. This performs better than linear probing and
		// works best for 2^N slots.
		probe += probe_attempts;
		if (probe >= slots) {
			probe = probe % slots;
		}
	}

	// These NULL checks will be optimized away in callers who don't care
	// about the values when this function is inlined.
	if (slot_ptr != NULL) {
		*slot_ptr = probe_slot;
	}
	if (record_number_ptr != NULL) {
		*record_number_ptr = record_number;
	}

	return record;
}

/**********************************************************************/
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

/**********************************************************************/
int put_open_chapter(struct open_chapter_zone *open_chapter,
		     const struct uds_chunk_name *name,
		     const struct uds_chunk_data *metadata,
		     unsigned int *remaining)
{
	unsigned int slot;
	struct uds_chunk_record *record =
		probe_chapter_slots(open_chapter, name, &slot, NULL);

	if (record != NULL) {
		record->data = *metadata;
		*remaining = open_chapter->capacity - open_chapter->size;
		return UDS_SUCCESS;
	}

	if (open_chapter->size >= open_chapter->capacity) {
		return makeUnrecoverable(UDS_VOLUME_OVERFLOW);
	}

	unsigned int record_number = ++open_chapter->size;
	open_chapter->slots[slot].record_number = record_number;
	record = &open_chapter->records[record_number];
	record->name = *name;
	record->data = *metadata;

	*remaining = open_chapter->capacity - open_chapter->size;
	return UDS_SUCCESS;
}

/**********************************************************************/
void remove_from_open_chapter(struct open_chapter_zone *open_chapter,
			      const struct uds_chunk_name *name,
			      bool *removed)
{
	unsigned int record_number;
	struct uds_chunk_record *record =
		probe_chapter_slots(open_chapter, name, NULL, &record_number);

	if (record == NULL) {
		*removed = false;
		return;
	}

	// Set the deleted flag on the record_number in the slot array so
	// search won't find it and close won't index it.
	open_chapter->slots[record_number].record_deleted = true;
	open_chapter->deleted += 1;
	*removed = true;
}

/**********************************************************************/
void free_open_chapter(struct open_chapter_zone *open_chapter)
{
	if (open_chapter != NULL) {
		FREE(open_chapter->records);
		FREE(open_chapter);
	}
}
