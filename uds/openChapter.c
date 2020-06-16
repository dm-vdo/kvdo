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
 * $Id: //eng/uds-releases/krusty/src/uds/openChapter.c#17 $
 */

#include "openChapter.h"

#include "compiler.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "zone.h"

static int readOpenChapters(struct read_portal *portal);
static int writeOpenChapters(struct index_component *component,
                             struct buffered_writer *writer,
                             unsigned int            zone);

const struct index_component_info OPEN_CHAPTER_INFO = {
  .kind         = RL_KIND_OPEN_CHAPTER,
  .name         = "open chapter",
  .save_only    = true,
  .chapter_sync = false,
  .multi_zone   = false,
  .io_storage   = true,
  .loader       = readOpenChapters,
  .saver        = writeOpenChapters,
  .incremental  = NULL,
};

static const byte OPEN_CHAPTER_MAGIC[]       = "ALBOC";
static const byte OPEN_CHAPTER_VERSION[]     = "02.00";

enum {
  OPEN_CHAPTER_MAGIC_LENGTH   = sizeof(OPEN_CHAPTER_MAGIC) - 1,
  OPEN_CHAPTER_VERSION_LENGTH = sizeof(OPEN_CHAPTER_VERSION) - 1
};

/**********************************************************************/
static int fillDeltaChapterIndex(struct open_chapter_zone **chapterZones,
                                 unsigned int                zoneCount,
                                 struct open_chapter_index  *index,
                                 struct uds_chunk_record    *collatedRecords)
{
  // Find a record to replace any deleted records, and fill the chapter if
  // it was closed early. The last record in any filled zone is guaranteed
  // to not have been deleted in this chapter, so use one of those.
  struct open_chapter_zone *fillChapterZone = NULL;
  struct uds_chunk_record  *fillRecord = NULL;
  unsigned int z;
  for (z = 0; z < zoneCount; ++z) {
    fillChapterZone = chapterZones[z];
    if (fillChapterZone->size == fillChapterZone->capacity) {
      fillRecord = &fillChapterZone->records[fillChapterZone->size];
      break;
    }
  }
  int result = ASSERT((fillRecord != NULL),
                      "some open chapter zone filled");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT(!fillChapterZone->slots[fillChapterZone->size].recordDeleted,
                  "chapter fill record not deleted");
  if (result != UDS_SUCCESS) {
    return result;
  }

  const struct geometry *geometry = index->geometry;
  unsigned int pagesPerChapter    = geometry->record_pages_per_chapter;
  unsigned int recordsPerPage     = geometry->records_per_page;
  int          overflowCount      = 0;
  unsigned int recordsAdded       = 0;
  unsigned int zone               = 0;

  unsigned int page;
  for (page = 0; page < pagesPerChapter; page++) {
    unsigned int i;
    for (i = 0;
         i < recordsPerPage;
         i++, recordsAdded++, zone = (zone + 1) % zoneCount) {

      // The record arrays are 1-based.
      unsigned int recordNumber = 1 + (recordsAdded / zoneCount);

      // If the zone has been exhausted, or the record was deleted,
      // add the fill record to the chapter.
      if (recordNumber > chapterZones[zone]->size
          || chapterZones[zone]->slots[recordNumber].recordDeleted) {
        collatedRecords[1 + recordsAdded] = *fillRecord;
        continue;
      }

      struct uds_chunk_record *nextRecord
        = &chapterZones[zone]->records[recordNumber];
      collatedRecords[1 + recordsAdded] = *nextRecord;

      int result = put_open_chapter_index_record(index, &nextRecord->name, page);
      switch (result) {
      case UDS_SUCCESS:
        break;
      case UDS_OVERFLOW:
        overflowCount++;
        break;
      default:
        logErrorWithStringError(result, "failed to build open chapter index");
        return result;
      }
    }
  }
  if (overflowCount > 0) {
    logWarning("Failed to add %d entries to chapter index", overflowCount);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int closeOpenChapter(struct open_chapter_zone  **chapterZones,
                     unsigned int                zoneCount,
                     Volume                     *volume,
                     struct open_chapter_index  *chapterIndex,
                     struct uds_chunk_record    *collatedRecords,
                     uint64_t                    virtualChapterNumber)
{
  // Empty the delta chapter index, and prepare it for the new virtual chapter.
  empty_open_chapter_index(chapterIndex, virtualChapterNumber);

  // Map each non-deleted record name to its record page number in the delta
  // chapter index.
  int result = fillDeltaChapterIndex(chapterZones, zoneCount, chapterIndex,
                                     collatedRecords);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Pass the populated chapter index and the records to the volume, which
  // will generate and write the index and record pages for the chapter.
  return writeChapter(volume, chapterIndex, collatedRecords);
}

/**********************************************************************/
int saveOpenChapters(struct index *index, struct buffered_writer *writer)
{
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

  uint32_t totalRecords = 0;
  unsigned int i;
  for (i = 0; i < index->zone_count; i++) {
    totalRecords += openChapterSize(index->zones[i]->open_chapter);
  }

  // Store the record count in little-endian order.
  byte totalRecordData[sizeof(totalRecords)];
  put_unaligned_le32(totalRecords, totalRecordData);

  result = write_to_buffered_writer(writer, totalRecordData,
                                    sizeof(totalRecordData));
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Only write out the records that have been added and not deleted.
  uint32_t recordsAdded = 0;
  unsigned int recordIndex = 1;
  while(recordsAdded < totalRecords) {
    unsigned int i;
    for (i = 0; i < index->zone_count; i++) {
      if (recordIndex > index->zones[i]->open_chapter->size) {
        continue;
      }
      if (index->zones[i]->open_chapter->slots[recordIndex].recordDeleted) {
        continue;
      }
      struct uds_chunk_record *record
        = &index->zones[i]->open_chapter->records[recordIndex];
      result = write_to_buffered_writer(writer, record,
                                        sizeof(struct uds_chunk_record));
      if (result != UDS_SUCCESS) {
        return result;
      }
      recordsAdded++;
    }
    recordIndex++;
  }

  return flush_buffered_writer(writer);
}

/**********************************************************************/
uint64_t computeSavedOpenChapterSize(struct geometry *geometry)
{
  return OPEN_CHAPTER_MAGIC_LENGTH + OPEN_CHAPTER_VERSION_LENGTH +
    sizeof(uint32_t) + geometry->records_per_chapter
      * sizeof(struct uds_chunk_record);
}

/**********************************************************************/
static int writeOpenChapters(struct index_component *component,
                             struct buffered_writer *writer,
                             unsigned int            zone)
{
  int result = ASSERT((zone == 0), "open chapter write not zoned");
  if (result != UDS_SUCCESS) {
    return result;
  }

  struct index *index = index_component_data(component);
  return saveOpenChapters(index, writer);
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
static int readVersion(struct buffered_reader *reader, const byte **version)
{
  byte buffer[OPEN_CHAPTER_VERSION_LENGTH];
  int result = read_from_buffered_reader(reader, buffer, sizeof(buffer));
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (memcmp(OPEN_CHAPTER_VERSION, buffer, sizeof(buffer)) != 0) {
    return logErrorWithStringError(UDS_CORRUPT_COMPONENT,
                                   "Invalid open chapter version: %.*s",
                                   (int) sizeof(buffer), buffer);
  }
  *version = OPEN_CHAPTER_VERSION;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int loadVersion20(struct index *index, struct buffered_reader *reader)
{
  byte numRecordsData[sizeof(uint32_t)];
  int result
    = read_from_buffered_reader(reader, &numRecordsData, sizeof(numRecordsData));
  if (result != UDS_SUCCESS) {
    return result;
  }
  uint32_t numRecords = get_unaligned_le32(numRecordsData);

  // Keep track of which zones cannot accept any more records.
  bool fullFlags[MAX_ZONES] = { false, };

  // Assign records to the correct zones.
  struct uds_chunk_record record;
  uint32_t records;
  for (records = 0; records < numRecords; records++) {
    result = read_from_buffered_reader(reader, &record,
                                       sizeof(struct uds_chunk_record));
    if (result != UDS_SUCCESS) {
      return result;
    }

    unsigned int zone = 0;
    if (index->zone_count > 1) {
      // A read-only index has no master index, but it also has only one zone.
      zone = getMasterIndexZone(index->master_index, &record.name);
    }
    // Add records until the open chapter zone almost runs out of space.
    // The chapter can't be closed here, so don't add the last record.
    if (!fullFlags[zone]) {
      unsigned int remaining;
      result = putOpenChapter(index->zones[zone]->open_chapter,
                              &record.name, &record.data, &remaining);
      fullFlags[zone] = (remaining <= 1);
      if (result != UDS_SUCCESS) {
        return result;
      }
    }
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
int loadOpenChapters(struct index *index, struct buffered_reader *reader)
{
  // Read and check the magic number.
  int result =
    verify_buffered_data(reader, OPEN_CHAPTER_MAGIC, OPEN_CHAPTER_MAGIC_LENGTH);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Read and check the version.
  const byte *version = NULL;
  result = readVersion(reader, &version);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return loadVersion20(index, reader);
}

/**********************************************************************/
int readOpenChapters(struct read_portal *portal)
{
  struct index *index = index_component_data(portal->component);

  struct buffered_reader *reader;
  int result = get_buffered_reader_for_portal(portal, 0, &reader);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return loadOpenChapters(index, reader);
}
