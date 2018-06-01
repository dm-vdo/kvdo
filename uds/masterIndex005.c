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
 * $Id: //eng/uds-releases/gloria/src/uds/masterIndex005.c#3 $
 */
#include "masterIndex005.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "featureDefs.h"
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
 * the representation 0.  See convertIndexToVirtual() or
 * flushInvalidEntries() for an example of this technique.
 */

typedef struct __attribute__((aligned(CACHE_LINE_BYTES))) masterIndexZone {
  uint64_t virtualChapterLow;      // The lowest virtual chapter indexed
  uint64_t virtualChapterHigh;     // The highest virtual chapter indexed
  long     numEarlyFlushes;        // The number of early flushes
} MasterIndexZone;

typedef struct {
  MasterIndex common;              // Common master index methods
  DeltaIndex deltaIndex;           // The delta index
  uint64_t *flushChapters;         // The first chapter to be flushed
  MasterIndexZone *masterZones;    // The Zones
  uint64_t     volumeNonce;        // The volume nonce
  uint64_t     chapterZoneBits;    // Expected size of a chapter (per zone)
  uint64_t     maxZoneBits;        // Maximum size index (per zone)
  unsigned int addressBits;        // Number of bits in address mask
  unsigned int addressMask;        // Mask to get address within delta list
  unsigned int chapterBits;        // Number of bits in chapter number
  unsigned int chapterMask;        // Largest storable chapter number
  unsigned int numChapters;        // Number of chapters used
  unsigned int numDeltaLists;      // The number of delta lists
  unsigned int numZones;           // The number of zones
} MasterIndex5;

typedef struct chapterRange {
  unsigned int chapterStart;    // The first chapter
  unsigned int chapterCount;    // The number of chapters
} ChapterRange;

// Constants for the magic byte of a MasterIndexRecord
static const byte masterIndexRecordMagic = 0xAA;
static const byte badMagic = 0;

/*
 * In production, the default value for minMasterIndexDeltaLists will be
 * replaced by MAX_ZONES*MAX_ZONES.  Some unit tests will replace
 * minMasterIndexDeltaLists with the non-default value 1, because those
 * tests really want to run with a single delta list.
 */
unsigned int minMasterIndexDeltaLists;

/**
 * Maximum of two unsigned ints
 *
 * @param a  One unsigned int
 * @param b  Another unsigned int
 *
 * @return the bigger one
 **/
static INLINE unsigned int maxUint(unsigned int a, unsigned int b)
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
static INLINE unsigned int extractAddress(const MasterIndex5 *mi5,
                                          const UdsChunkName *name)
{
  return extractMasterIndexBytes(name) & mi5->addressMask;
}

/**
 * Extract the delta list number from a block name.
 *
 * @param mi5   The master index
 * @param name  The block name
 *
 * @return the delta list number
 **/
static INLINE unsigned int extractDListNum(const MasterIndex5 *mi5,
                                           const UdsChunkName *name)
{
  uint64_t bits = extractMasterIndexBytes(name);
  return (bits >> mi5->addressBits) % mi5->numDeltaLists;
}

/**
 * Get the master index zone containing a given master index record
 *
 * @param record  The master index record
 *
 * @return the master index zone
 **/
static INLINE const MasterIndexZone *getMasterZone(const MasterIndexRecord *record)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  return &mi5->masterZones[record->zoneNumber];
}

/**
 * Convert an index chapter number to a virtual chapter number.
 *
 * @param record        The master index record
 * @param indexChapter  The index chapter number
 *
 * @return the virtual chapter number
 **/
static INLINE uint64_t convertIndexToVirtual(const MasterIndexRecord *record,
                                             unsigned int indexChapter)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  const MasterIndexZone *masterZone = getMasterZone(record);
  unsigned int rollingChapter
    = ((indexChapter - masterZone->virtualChapterLow) & mi5->chapterMask);
  return masterZone->virtualChapterLow + rollingChapter;
}

/**
 * Convert a virtual chapter number to an index chapter number.
 *
 * @param mi5             The master index
 * @param virtualChapter  The virtual chapter number
 *
 * @return the index chapter number
 **/
static INLINE unsigned int convertVirtualToIndex(const MasterIndex5 *mi5,
                                                 uint64_t virtualChapter)
{
  return virtualChapter & mi5->chapterMask;
}

/**
 * Determine whether a virtual chapter number is in the range being indexed
 *
 * @param record          The master index record
 * @param virtualChapter  The virtual chapter number
 *
 * @return true if the virtual chapter number is being indexed
 **/
static INLINE bool isVirtualChapterIndexed(const MasterIndexRecord *record,
                                           uint64_t virtualChapter)
{
  const MasterIndexZone *masterZone = getMasterZone(record);
  return ((virtualChapter >= masterZone->virtualChapterLow)
          && (virtualChapter <= masterZone->virtualChapterHigh));
}

/***********************************************************************/
/**
 * Flush an invalid entry from the master index, advancing to the next
 * valid entry.
 *
 * @param record                   Updated to describe the next valid record
 * @param flushRange               Range of chapters to flush from the index
 * @param nextChapterToInvalidate  Updated to record the next chapter that we
 *                                 will need to invalidate
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int flushInvalidEntries(MasterIndexRecord *record,
                                      ChapterRange *flushRange,
                                      unsigned int *nextChapterToInvalidate)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  int result = nextDeltaIndexEntry(&record->deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  while (!record->deltaEntry.atEnd) {
    unsigned int indexChapter = getDeltaEntryValue(&record->deltaEntry);
    unsigned int relativeChapter = ((indexChapter - flushRange->chapterStart)
                                    & mi5->chapterMask);
    if (likely(relativeChapter >= flushRange->chapterCount)) {
      if (relativeChapter < *nextChapterToInvalidate) {
        *nextChapterToInvalidate = relativeChapter;
      }
      break;
    }
    result = removeDeltaIndexEntry(&record->deltaEntry);
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
 * @param record       Updated to describe the entry being looked for
 * @param listNumber   The delta list number
 * @param key          The address field being looked for
 * @param flushRange   The range of chapters to flush from the index
 *
 * @return UDS_SUCCESS or an error code
 **/
static int getMasterIndexEntry(MasterIndexRecord *record,
                               unsigned int       listNumber,
                               unsigned int       key,
                               ChapterRange      *flushRange)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  unsigned int nextChapterToInvalidate = mi5->chapterMask;

  int result = startDeltaIndexSearch(&mi5->deltaIndex, listNumber, 0,
                                     false, &record->deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  do {
    result = flushInvalidEntries(record, flushRange, &nextChapterToInvalidate);
    if (result != UDS_SUCCESS) {
      return result;
    }
  } while (!record->deltaEntry.atEnd && (key > record->deltaEntry.key));

  result = rememberDeltaIndexOffset(&record->deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // We probably found the record we want, but we need to keep going
  MasterIndexRecord otherRecord = *record;
  if (!otherRecord.deltaEntry.atEnd && (key == otherRecord.deltaEntry.key)) {
    for (;;) {
      result = flushInvalidEntries(&otherRecord, flushRange,
                                   &nextChapterToInvalidate);
      if (result != UDS_SUCCESS) {
        return result;
      }
      if (otherRecord.deltaEntry.atEnd
          || !otherRecord.deltaEntry.isCollision) {
        break;
      }
      byte collisionName[UDS_CHUNK_NAME_SIZE];
      result = getDeltaEntryCollision(&otherRecord.deltaEntry, collisionName);
      if (result != UDS_SUCCESS) {
        return result;
      }
      if (memcmp(collisionName, record->name, UDS_CHUNK_NAME_SIZE) == 0) {
        // This collision record is the one we are looking for
        *record = otherRecord;
        break;
      }
    }
  }
  while (!otherRecord.deltaEntry.atEnd) {
    result = flushInvalidEntries(&otherRecord, flushRange,
                                 &nextChapterToInvalidate);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  nextChapterToInvalidate += flushRange->chapterStart;
  nextChapterToInvalidate &= mi5->chapterMask;
  flushRange->chapterStart = nextChapterToInvalidate;
  flushRange->chapterCount = 0;
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Terminate and clean up the master index
 *
 * @param masterIndex The master index to terminate
 **/
static void freeMasterIndex_005(MasterIndex *masterIndex)
{
  if (masterIndex != NULL) {
    MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
    FREE(mi5->flushChapters);
    mi5->flushChapters = NULL;
    FREE(mi5->masterZones);
    mi5->masterZones = NULL;
    uninitializeDeltaIndex(&mi5->deltaIndex);
    FREE(masterIndex);
  }
}

/**
 * Constants and structures for the saved master index file.  "MI5" is for
 * masterIndex005, and "-XXXX" is a number to increment when the format of
 * the data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_MI_START[] = "MI5-0005";

struct mi005_data {
  char magic[MAGIC_SIZE];       // MAGIC_MI_START
  uint64_t volumeNonce;
  uint64_t virtualChapterLow;
  uint64_t virtualChapterHigh;
  unsigned int firstList;
  unsigned int numLists;
};

/***********************************************************************/
/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param masterIndex  The master index
 * @param tag          The tag value
 **/
static void setMasterIndexTag_005(MasterIndex *masterIndex, byte tag)
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  setDeltaIndexTag(&mi5->deltaIndex, tag);
}

/***********************************************************************/
__attribute__((warn_unused_result))
static int encodeMasterIndexHeader(Buffer *buffer, struct mi005_data *header)
{
  int result = putBytes(buffer, MAGIC_SIZE, MAGIC_MI_START);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, header->volumeNonce);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, header->virtualChapterLow);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, header->virtualChapterHigh);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, header->firstList);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, header->numLists);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == sizeof(struct mi005_data),
                           "%zu bytes of config written, of %zu expected",
                           contentLength(buffer), sizeof(struct mi005_data));
  return result;
}

/**
 * Start saving a master index to a buffered output stream.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The number of the zone to save
 * @param bufferedWriter  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int startSavingMasterIndex_005(const MasterIndex *masterIndex,
                                      unsigned int zoneNumber,
                                      BufferedWriter *bufferedWriter)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  MasterIndexZone *masterZone = &mi5->masterZones[zoneNumber];
  unsigned int firstList = getDeltaIndexZoneFirstList(&mi5->deltaIndex,
                                                      zoneNumber);
  unsigned int numLists = getDeltaIndexZoneNumLists(&mi5->deltaIndex,
                                                    zoneNumber);

  struct mi005_data header;
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, MAGIC_MI_START, MAGIC_SIZE);
  header.volumeNonce        = mi5->volumeNonce;
  header.virtualChapterLow  = masterZone->virtualChapterLow;
  header.virtualChapterHigh = masterZone->virtualChapterHigh;
  header.firstList          = firstList;
  header.numLists           = numLists;

  Buffer *buffer;
  int result = makeBuffer(sizeof(struct mi005_data), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = encodeMasterIndexHeader(buffer, &header);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(bufferedWriter, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "failed to write master index header");
  }
  result = makeBuffer(numLists * sizeof(uint64_t), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  uint64_t *firstFlushChapter = &mi5->flushChapters[firstList];
  result = putUInt64LEsIntoBuffer(buffer, numLists, firstFlushChapter);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(bufferedWriter, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "failed to write master index flush "
                                     "ranges");
  }

  return startSavingDeltaIndex(&mi5->deltaIndex, zoneNumber, bufferedWriter);
}

/***********************************************************************/
/**
 * Have all the data been written while saving a master index to an output
 * stream?  If the answer is yes, it is still necessary to call
 * finishSavingMasterIndex(), which will return quickly.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static bool isSavingMasterIndexDone_005(const MasterIndex *masterIndex,
                                        unsigned int zoneNumber)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  return isSavingDeltaIndexDone(&mi5->deltaIndex, zoneNumber);
}

/***********************************************************************/
/**
 * Finish saving a master index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int finishSavingMasterIndex_005(const MasterIndex *masterIndex,
                                       unsigned int zoneNumber)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  return finishSavingDeltaIndex(&mi5->deltaIndex, zoneNumber);
}

/***********************************************************************/
/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int abortSavingMasterIndex_005(const MasterIndex *masterIndex,
                                      unsigned int zoneNumber)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  return abortSavingDeltaIndex(&mi5->deltaIndex, zoneNumber);
}

/***********************************************************************/
__attribute__((warn_unused_result))
static int decodeMasterIndexHeader(Buffer *buffer, struct mi005_data *header)
{
  int result = getBytesFromBuffer(buffer, sizeof(header->magic),
                                  &header->magic);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &header->volumeNonce);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &header->virtualChapterLow);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &header->virtualChapterHigh);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &header->firstList);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &header->numLists);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           bufferLength(buffer) - contentLength(buffer),
                           bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    result = UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param masterIndex      The master index to restore into
 * @param bufferedReaders  The buffered readers to read the master index from
 * @param numReaders       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int startRestoringMasterIndex_005(MasterIndex *masterIndex,
                                         BufferedReader **bufferedReaders,
                                         int numReaders)
{
  if (masterIndex == NULL) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "cannot restore to null master index");
  }
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  emptyDeltaIndex(&mi5->deltaIndex);

  uint64_t virtualChapterLow = 0;
  uint64_t virtualChapterHigh = 0;
  for (int i = 0; i < numReaders; i++) {
    Buffer *buffer;
    int result = makeBuffer(sizeof(struct mi005_data), &buffer);
    if (result != UDS_SUCCESS)  {
      return result;
    }
    result = readFromBufferedReader(bufferedReaders[i],
                                    getBufferContents(buffer),
                                    bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return logWarningWithStringError(result,
                                       "failed to read master index header");
    }
    result = resetBufferEnd(buffer, bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return result;
    }
    struct mi005_data header;
    result = decodeMasterIndexHeader(buffer, &header);
    freeBuffer(&buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (memcmp(header.magic, MAGIC_MI_START, MAGIC_SIZE) != 0) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "master index file had bad magic"
                                       " number");
    }
    if (mi5->volumeNonce == 0) {
      mi5->volumeNonce = header.volumeNonce;
    } else if (header.volumeNonce != mi5->volumeNonce) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "master index volume nonce incorrect");
    }
    if (i == 0) {
      virtualChapterLow  = header.virtualChapterLow;
      virtualChapterHigh = header.virtualChapterHigh;
    } else if (virtualChapterHigh != header.virtualChapterHigh) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "Inconsistent master index zone files:"
                                       " Chapter range is [%" PRIu64 ",%"
                                       PRIu64 "], chapter range %d is [%"
                                       PRIu64 ",%" PRIu64 "]",
                                       virtualChapterLow, virtualChapterHigh,
                                       i, header.virtualChapterLow,
                                       header.virtualChapterHigh);
    } else if (virtualChapterLow < header.virtualChapterLow) {
      virtualChapterLow = header.virtualChapterLow;
    }
    uint64_t *firstFlushChapter = &mi5->flushChapters[header.firstList];
    result = makeBuffer(header.numLists * sizeof(uint64_t), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = readFromBufferedReader(bufferedReaders[i],
                                    getBufferContents(buffer),
                                    bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return logWarningWithStringError(result,
                                       "failed to read master index flush"
                                       " ranges");
    }
    result = resetBufferEnd(buffer, bufferLength(buffer));
    if (result != UDS_SUCCESS) {
      freeBuffer(&buffer);
      return result;
    }
    result = getUInt64LEsFromBuffer(buffer, header.numLists,
                                    firstFlushChapter);
    freeBuffer(&buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  for (unsigned int z = 0; z < mi5->numZones; z++) {
    memset(&mi5->masterZones[z], 0, sizeof(MasterIndexZone));
    mi5->masterZones[z].virtualChapterLow  = virtualChapterLow;
    mi5->masterZones[z].virtualChapterHigh = virtualChapterHigh;
  }

  int result = startRestoringDeltaIndex(&mi5->deltaIndex, bufferedReaders,
                                        numReaders);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result, "restoring delta index failed");
  }
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Have all the data been read while restoring a master index from an
 * input stream?
 *
 * @param masterIndex  The master index to restore into
 *
 * @return true if all the data are read
 **/
static bool isRestoringMasterIndexDone_005(const MasterIndex *masterIndex)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  return isRestoringDeltaIndexDone(&mi5->deltaIndex);
}

/***********************************************************************/
/**
 * Restore a saved delta list
 *
 * @param masterIndex  The master index to restore into
 * @param dlsi         The DeltaListSaveInfo describing the delta list
 * @param data         The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static int restoreDeltaListToMasterIndex_005(MasterIndex *masterIndex,
                                             const DeltaListSaveInfo *dlsi,
                                             const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  return restoreDeltaListToDeltaIndex(&mi5->deltaIndex, dlsi, data);
}

/***********************************************************************/
/**
 * Abort restoring a master index from an input stream.
 *
 * @param masterIndex  The master index
 **/
static void abortRestoringMasterIndex_005(MasterIndex *masterIndex)
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  abortRestoringDeltaIndex(&mi5->deltaIndex);
}

/***********************************************************************/
static void removeNewestChapters(MasterIndex5 *mi5,
                                 unsigned int zoneNumber,
                                 uint64_t virtualChapter)
{
  // Get the range of delta lists belonging to this zone
  unsigned int firstList = getDeltaIndexZoneFirstList(&mi5->deltaIndex,
                                                      zoneNumber);
  unsigned int numLists = getDeltaIndexZoneNumLists(&mi5->deltaIndex,
                                                    zoneNumber);
  unsigned int lastList = firstList + numLists - 1;

  if (virtualChapter > mi5->chapterMask) {
    // The virtual chapter number is large enough so that we can use the
    // normal LRU mechanism without an unsigned underflow.
    virtualChapter -= mi5->chapterMask + 1;
    // Eliminate the newest chapters by renumbering them to become the
    // oldest chapters
    for (unsigned int i = firstList; i <= lastList; i++) {
      if (virtualChapter < mi5->flushChapters[i]) {
        mi5->flushChapters[i] = virtualChapter;
      }
    }
  } else {
    // Underflow will prevent the fast path.  Do it the slow and painful way.
    MasterIndexZone *masterZone = &mi5->masterZones[zoneNumber];
    ChapterRange range;
    range.chapterStart = convertVirtualToIndex(mi5, virtualChapter);
    range.chapterCount = (mi5->chapterMask + 1
                          - (virtualChapter - masterZone->virtualChapterLow));
    UdsChunkName name;
    memset(&name, 0, sizeof(UdsChunkName));
    MasterIndexRecord record = (MasterIndexRecord) {
      .magic       = masterIndexRecordMagic,
      .masterIndex = &mi5->common,
      .name        = &name,
      .zoneNumber  = zoneNumber,
    };
    for (unsigned int i = firstList; i <= lastList; i++) {
      ChapterRange tempRange = range;
      getMasterIndexEntry(&record, i, 0, &tempRange);
    }
  }
}

/***********************************************************************/
/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The zone number
 * @param virtualChapter  The new open chapter number
 **/
static void setMasterIndexZoneOpenChapter_005(MasterIndex *masterIndex,
                                              unsigned int zoneNumber,
                                              uint64_t virtualChapter)
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  MasterIndexZone *masterZone = &mi5->masterZones[zoneNumber];
  // Take care here to avoid underflow of an unsigned value.  Note that
  // this is the smallest valid virtual low.  We may or may not actually
  // use this value.
  uint64_t newVirtualLow = (virtualChapter >= mi5->numChapters
                            ? virtualChapter - mi5->numChapters + 1
                            : 0);

  if (virtualChapter <= masterZone->virtualChapterLow) {
    /*
     * Moving backwards and the new range is totally before the old range.
     * Note that moving to the lowest virtual chapter counts as totally before
     * the old range, as we need to remove the entries in the open chapter.
     */
    emptyDeltaIndexZone(&mi5->deltaIndex, zoneNumber);
    masterZone->virtualChapterLow  = virtualChapter;
    masterZone->virtualChapterHigh = virtualChapter;
  } else if (virtualChapter <= masterZone->virtualChapterHigh) {
    // Moving backwards and the new range overlaps the old range.  Note
    // that moving to the same open chapter counts as backwards, as we need
    // to remove the entries in the open chapter.
    removeNewestChapters(mi5, zoneNumber, virtualChapter);
    masterZone->virtualChapterHigh = virtualChapter;
  } else if (newVirtualLow < masterZone->virtualChapterLow) {
    // Moving forwards and we can keep all the old chapters
    masterZone->virtualChapterHigh = virtualChapter;
  } else if (newVirtualLow <= masterZone->virtualChapterHigh) {
    // Moving forwards and we can keep some old chapters
    masterZone->virtualChapterLow  = newVirtualLow;
    masterZone->virtualChapterHigh = virtualChapter;
  } else {
    // Moving forwards and the new range is totally after the old range
    masterZone->virtualChapterLow  = virtualChapter;
    masterZone->virtualChapterHigh = virtualChapter;
  }
  // Check to see if the zone data has grown to be too large
  if (masterZone->virtualChapterLow < masterZone->virtualChapterHigh) {
    uint64_t usedBits = getDeltaIndexZoneDlistBitsUsed(&mi5->deltaIndex,
                                                       zoneNumber);
    if (usedBits > mi5->maxZoneBits) {
      // Expire enough chapters to free the desired space
      uint64_t expireCount
        = 1 + (usedBits - mi5->maxZoneBits) / mi5->chapterZoneBits;
      if (expireCount == 1) {
        logInfo("masterZone %u:  At chapter %" PRIu64 ", expiring chapter %"
                PRIu64 " early",
                zoneNumber, virtualChapter, masterZone->virtualChapterLow);
        masterZone->numEarlyFlushes++;
        masterZone->virtualChapterLow++;
      } else {
        uint64_t firstExpired = masterZone->virtualChapterLow;
        if (firstExpired + expireCount < masterZone->virtualChapterHigh) {
          masterZone->numEarlyFlushes += expireCount;
          masterZone->virtualChapterLow += expireCount;
        } else {
          masterZone->numEarlyFlushes
            += masterZone->virtualChapterHigh - masterZone->virtualChapterLow;
          masterZone->virtualChapterLow = masterZone->virtualChapterHigh;
        }
        logInfo("masterZone %u:  At chapter %" PRIu64 ", expiring chapters %"
                PRIu64 " to %" PRIu64 " early", zoneNumber, virtualChapter,
                firstExpired, masterZone->virtualChapterLow - 1);
      }
    }
  }
}

/***********************************************************************/
/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param masterIndex     The master index
 * @param virtualChapter  The new open chapter number
 **/
static void setMasterIndexOpenChapter_005(MasterIndex *masterIndex,
                                          uint64_t virtualChapter)
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  for (unsigned int z = 0; z < mi5->numZones; z++) {
    // In normal operation, we advance forward one chapter at a time.
    // Log all abnormal changes.
    MasterIndexZone *masterZone = &mi5->masterZones[z];
    bool logMove = virtualChapter != masterZone->virtualChapterHigh + 1;
    if (logMove) {
      logDebug("masterZone %u: The range of indexed chapters is moving from [%"
               PRIu64 ", %" PRIu64 "] ...",
               z,
               masterZone->virtualChapterLow,
               masterZone->virtualChapterHigh);
    }

    setMasterIndexZoneOpenChapter_005(masterIndex, z, virtualChapter);

    if (logMove) {
      logDebug("masterZone %u: ... and moving to [%" PRIu64 ", %" PRIu64 "]",
               z,
               masterZone->virtualChapterLow,
               masterZone->virtualChapterHigh);
    }
  }
}

/***********************************************************************/
/**
 * Find the master index zone associated with a chunk name
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int getMasterIndexZone_005(const MasterIndex *masterIndex,
                                           const UdsChunkName *name)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  unsigned int deltaListNumber = extractDListNum(mi5, name);
  return getDeltaIndexZone(&mi5->deltaIndex, deltaListNumber);
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name
 *
 * @return UDS_SUCCESS or an error code
 **/
static int lookupMasterIndexName_005(const MasterIndex *masterIndex,
                                     const UdsChunkName *name,
                                     MasterIndexTriage *triage)
{
  triage->isSample = false;
  triage->inSampledChapter = false;
  triage->zone = getMasterIndexZone_005(masterIndex, name);
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name.  The zone and
 *                    isSample fields are already filled in.  Set
 *                    inSampledChapter and virtualChapter if the chunk
 *                    name is found in the index.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int lookupMasterIndexSampledName_005(const MasterIndex *masterIndex,
                                            const UdsChunkName *name,
                                            MasterIndexTriage *triage)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  unsigned int address = extractAddress(mi5, name);
  unsigned int deltaListNumber = extractDListNum(mi5, name);
  DeltaIndexEntry deltaEntry;
  int result = getDeltaIndexEntry(&mi5->deltaIndex, deltaListNumber, address,
                                  name->name, true, &deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  triage->inSampledChapter = !deltaEntry.atEnd && (deltaEntry.key == address);
  if (triage->inSampledChapter) {
    const MasterIndexZone *masterZone = &mi5->masterZones[triage->zone];
    unsigned int indexChapter = getDeltaEntryValue(&deltaEntry);
    unsigned int rollingChapter = ((indexChapter
                                    - masterZone->virtualChapterLow)
                                   & mi5->chapterMask);
    triage->virtualChapter = masterZone->virtualChapterLow + rollingChapter;
    if (triage->virtualChapter > masterZone->virtualChapterHigh) {
      triage->inSampledChapter = false;
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
 * If isFound is false, then we did not find an entry for the block
 * name.  Information is saved in the MasterIndexRecord so that
 * putMasterIndexRecord() will insert an entry for that block name at
 * the proper place.
 *
 * If isFound is true, then we did find an entry for the block name.
 * Information is saved in the MasterIndexRecord so that the "chapter"
 * and "isCollision" fields reflect the entry found.
 * Calls to removeMasterIndexRecord() will remove the entry, calls to
 * setMasterIndexRecordChapter() can modify the entry, and calls to
 * putMasterIndexRecord() can insert a collision record with this
 * entry.
 *
 * @param masterIndex The master index to search
 * @param name        The chunk name
 * @param record      Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int getMasterIndexRecord_005(MasterIndex *masterIndex,
                                    const UdsChunkName *name,
                                    MasterIndexRecord *record)
{
  MasterIndex5 *mi5 = container_of(masterIndex, MasterIndex5, common);
  unsigned int address = extractAddress(mi5, name);
  unsigned int deltaListNumber = extractDListNum(mi5, name);
  uint64_t flushChapter = mi5->flushChapters[deltaListNumber];
  record->magic       = masterIndexRecordMagic;
  record->masterIndex = masterIndex;
  record->mutex       = NULL;
  record->name        = name;
  record->zoneNumber  = getDeltaIndexZone(&mi5->deltaIndex, deltaListNumber);
  const MasterIndexZone *masterZone = getMasterZone(record);

  int result;
  if (flushChapter < masterZone->virtualChapterLow) {
    ChapterRange range;
    uint64_t flushCount = masterZone->virtualChapterLow - flushChapter;
    range.chapterStart = convertVirtualToIndex(mi5, flushChapter);
    range.chapterCount = (flushCount > mi5->chapterMask
                          ? mi5->chapterMask + 1
                          : flushCount);
    result = getMasterIndexEntry(record, deltaListNumber, address, &range);
    flushChapter = convertIndexToVirtual(record, range.chapterStart);
    if (flushChapter > masterZone->virtualChapterHigh) {
      flushChapter = masterZone->virtualChapterHigh;
    }
    mi5->flushChapters[deltaListNumber] = flushChapter;
  } else {
    result = getDeltaIndexEntry(&mi5->deltaIndex, deltaListNumber, address,
                                name->name, false, &record->deltaEntry);
  }
  if (result != UDS_SUCCESS) {
    return result;
  }
  record->isFound = (!record->deltaEntry.atEnd
                     && (record->deltaEntry.key == address));
  if (record->isFound) {
    unsigned int indexChapter = getDeltaEntryValue(&record->deltaEntry);
    record->virtualChapter = convertIndexToVirtual(record, indexChapter);
  }
  record->isCollision = record->deltaEntry.isCollision;
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Create a new record associated with a block name.
 *
 * @param record          The master index record found by getRecord()
 * @param virtualChapter  The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int putMasterIndexRecord(MasterIndexRecord *record, uint64_t virtualChapter)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  if (record->magic != masterIndexRecordMagic) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "bad magic number in master index record");
  }
  if (!isVirtualChapterIndexed(record, virtualChapter)) {
    const MasterIndexZone *masterZone = getMasterZone(record);
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot put record into chapter number %"
                                     PRIu64 " that is out of the valid range %"
                                     PRIu64 " to %" PRIu64,
                                     virtualChapter,
                                     masterZone->virtualChapterLow,
                                     masterZone->virtualChapterHigh);
  }
  unsigned int address = extractAddress(mi5, record->name);
  if (unlikely(record->mutex != NULL)) {
    lockMutex(record->mutex);
  }
  int result = putDeltaIndexEntry(&record->deltaEntry, address,
                                  convertVirtualToIndex(mi5, virtualChapter),
                                  record->isFound ? record->name->name : NULL);
  if (unlikely(record->mutex != NULL)) {
    unlockMutex(record->mutex);
  }
  switch (result) {
  case UDS_SUCCESS:
    record->virtualChapter = virtualChapter;
    record->isCollision    = record->deltaEntry.isCollision;
    record->isFound        = true;
    break;
  case UDS_OVERFLOW:
    logWarningWithStringError(UDS_OVERFLOW,
                              "Master index entry dropped due to overflow "
                              "condition");
    logDeltaIndexEntry(&record->deltaEntry);
    break;
  default:
    break;
  }
  return result;
}

/**********************************************************************/
static INLINE int validateRecord(MasterIndexRecord *record)
{
  if (record->magic != masterIndexRecordMagic) {
    return logWarningWithStringError(
      UDS_BAD_STATE, "bad magic number in master index record");
  }
  if (!record->isFound) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "illegal operation on new record");
  }
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Remove an existing record.
 *
 * @param record      The master index record found by getRecord()
 *
 * @return UDS_SUCCESS or an error code
 **/
int removeMasterIndexRecord(MasterIndexRecord *record)
{
  int result = validateRecord(record);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Mark the record so that it cannot be used again
  record->magic = badMagic;
  if (unlikely(record->mutex != NULL)) {
    lockMutex(record->mutex);
  }
  result = removeDeltaIndexEntry(&record->deltaEntry);
  if (unlikely(record->mutex != NULL)) {
    unlockMutex(record->mutex);
  }
  return result;
}

/***********************************************************************/
/**
 * Set the chapter number associated with a block name.
 *
 * @param record         The master index record found by getRecord()
 * @param virtualChapter The chapter number where the block info is now found.
 *
 * @return UDS_SUCCESS or an error code
 **/
int setMasterIndexRecordChapter(MasterIndexRecord *record,
                                uint64_t virtualChapter)
{
  const MasterIndex5 *mi5 = container_of(record->masterIndex, MasterIndex5,
                                         common);
  int result = validateRecord(record);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (!isVirtualChapterIndexed(record, virtualChapter)) {
    const MasterIndexZone *masterZone = getMasterZone(record);
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot set chapter number %" PRIu64
                                     " that is out of the valid range %" PRIu64
                                     " to %" PRIu64,
                                     virtualChapter,
                                     masterZone->virtualChapterLow,
                                     masterZone->virtualChapterHigh);
  }
  if (unlikely(record->mutex != NULL)) {
    lockMutex(record->mutex);
  }
  result = setDeltaEntryValue(&record->deltaEntry,
                              convertVirtualToIndex(mi5, virtualChapter));
  if (unlikely(record->mutex != NULL)) {
    unlockMutex(record->mutex);
  }
  if (result != UDS_SUCCESS) {
    return result;
  }
  record->virtualChapter = virtualChapter;
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Get the number of bytes used for master index entries.
 *
 * @param masterIndex The master index
 *
 * @return The number of bytes in use
 **/
static size_t getMasterIndexMemoryUsed_005(const MasterIndex *masterIndex)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  uint64_t bits = getDeltaIndexDlistBitsUsed(&mi5->deltaIndex);
  return (bits + CHAR_BIT - 1) / CHAR_BIT;
}

/***********************************************************************/
/**
 * Return the master index stats.  There is only one portion of the master
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param masterIndex The master index
 * @param dense       Stats for the dense portion of the index
 * @param sparse      Stats for the sparse portion of the index
 **/
static void getMasterIndexStats_005(const MasterIndex *masterIndex,
                                    MasterIndexStats *dense,
                                    MasterIndexStats *sparse)
{
  const MasterIndex5 *mi5 = const_container_of(masterIndex, MasterIndex5,
                                               common);
  DeltaIndexStats dis;
  getDeltaIndexStats(&mi5->deltaIndex, &dis);
  dense->memoryAllocated = (dis.memoryAllocated
                            + sizeof(MasterIndex5)
                            + mi5->numDeltaLists * sizeof(uint64_t)
                            + mi5->numZones * sizeof(MasterIndexZone));
  dense->rebalanceTime   = dis.rebalanceTime;
  dense->rebalanceCount  = dis.rebalanceCount;
  dense->recordCount     = dis.recordCount;
  dense->collisionCount  = dis.collisionCount;
  dense->discardCount    = dis.discardCount;
  dense->overflowCount   = dis.overflowCount;
  dense->numLists        = dis.numLists;
  dense->earlyFlushes    = 0;
  for (unsigned int z = 0; z < mi5->numZones; z++) {
    dense->earlyFlushes += mi5->masterZones[z].numEarlyFlushes;
  }
  memset(sparse, 0, sizeof(MasterIndexStats));
}

/***********************************************************************/
/**
 * Determine whether a given chunk name is a hook.
 *
 * @param masterIndex    The master index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static bool isMasterIndexSample_005(const MasterIndex  *masterIndex
                                    __attribute__((unused)),
                                    const UdsChunkName *name
                                    __attribute__((unused)))
{
  return false;
}

/***********************************************************************/
typedef struct {
  unsigned int addressBits;    // Number of bits in address mask
  unsigned int chapterBits;    // Number of bits in chapter number
  unsigned int meanDelta;      // The mean delta
  unsigned long numDeltaLists; // The number of delta lists
  unsigned long numChapters;   // Number of chapters used
  size_t numBitsPerChapter;    // The number of bits per chapter
  size_t memorySize;           // The number of bytes of delta list memory
  size_t targetFreeSize;       // The number of free bytes we desire
} Parameters005;

/***********************************************************************/
static int computeMasterIndexParameters005(const Configuration *config,
                                           Parameters005 *params)
{
  enum { DELTA_LIST_SIZE = 256 };
  /*
   * For a given zone count, setting the the minimum number of delta lists
   * to the square of the number of zones ensures that the distribution of
   * delta lists over zones doesn't underflow, leaving the last zone with
   * an invalid number of delta lists. See the explanation in
   * initializeDeltaIndex(). Because we can restart with a different number
   * of zones but the number of delta lists is invariant across restart,
   * we must use the largest number of zones to compute this minimum.
   */
  unsigned long minDeltaLists = (minMasterIndexDeltaLists
                                 ? minMasterIndexDeltaLists
                                 : MAX_ZONES * MAX_ZONES);

  Geometry *geometry = config->geometry;
  unsigned long recordsPerChapter = geometry->recordsPerChapter;
  params->numChapters = geometry->chaptersPerVolume;
  unsigned long recordsPerVolume = recordsPerChapter * params->numChapters;
  unsigned int numAddresses = config->masterIndexMeanDelta * DELTA_LIST_SIZE;
  params->numDeltaLists
    = maxUint(recordsPerVolume / DELTA_LIST_SIZE, minDeltaLists);
  params->addressBits = computeBits(numAddresses - 1);
  params->chapterBits = computeBits(params->numChapters - 1);

  if ((unsigned int) params->numDeltaLists != params->numDeltaLists) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize master index with %lu"
                                     " delta lists",
                                     params->numDeltaLists);
  }
  if (params->addressBits > 31) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize master index with %u"
                                     " address bits",
                                     params->addressBits);
  }
  if (geometry->sparseChaptersPerVolume > 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize dense master index"
                                     " with %u sparse chapters",
                                     geometry->sparseChaptersPerVolume);
  }
  if (recordsPerChapter == 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize master index with %lu"
                                     " records per chapter",
                                     recordsPerChapter);
  }
  if (params->numChapters == 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize master index with %lu"
                                     " chapters per volume",
                                     params->numChapters);
  }

  /*
   * We can now compute the probability that a delta list is not touched during
   * the writing of an entire chapter.  The computation is:
   *
   * double pNotTouched = pow((double) (params->numDeltaLists - 1)
   *                          / params->numDeltaLists,
   *                          recordsPerChapter);
   *
   * For the standard index sizes, about 78% of the delta lists are not
   * touched, and therefore contain dead index entries that have not been
   * eliminated by the lazy LRU processing.  We can then compute how many dead
   * index entries accumulate over time.  The computation is:
   *
   * double invalidChapters = pNotTouched / (1.0 - pNotTouched);
   *
   * For the standard index sizes, we will need about 3.5 chapters of space for
   * the dead index entries in a 1K chapter index.  Since we do not want to do
   * that floating point computation, we use 4 chapters per 1K of chapters.
   */
  unsigned long invalidChapters = maxUint(params->numChapters / 256, 2);
  unsigned long chaptersInMasterIndex = params->numChapters + invalidChapters;
  unsigned long entriesInMasterIndex
    = recordsPerChapter * chaptersInMasterIndex;
  // Compute the mean delta
  unsigned long addressSpan = params->numDeltaLists << params->addressBits;
  params->meanDelta = addressSpan / entriesInMasterIndex;
  // Project how large we expect a chapter to be
  params->numBitsPerChapter = getDeltaMemorySize(recordsPerChapter,
                                                 params->meanDelta,
                                                 params->chapterBits);
  // Project how large we expect the index to be
  size_t numBitsPerIndex = params->numBitsPerChapter * chaptersInMasterIndex;
  size_t expectedIndexSize = numBitsPerIndex / CHAR_BIT;
  /*
   * Set the total memory to be 6% larger than the expected index size.  We
   * want this number to be large enough that the we do not do a great many
   * rebalances as the list when the list is full.  We use MasterIndex_p1
   * to tune this setting.
   */
  params->memorySize = expectedIndexSize * 106 / 100;
  // Set the target free size to 5% of the expected index size
  params->targetFreeSize = expectedIndexSize / 20;
  return UDS_SUCCESS;
}

/***********************************************************************/
int computeMasterIndexSaveBytes005(const Configuration *config,
                                   size_t *numBytes)
{
  Parameters005 params = { .addressBits = 0 };
  int result = computeMasterIndexParameters005(config, &params);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Saving a MasterIndex005 needs a header plus one uint64_t per delta
  // list plus the delta index.
  *numBytes = (sizeof(struct mi005_data)
               + params.numDeltaLists * sizeof(uint64_t)
               + computeDeltaIndexSaveBytes(params.numDeltaLists,
                                            params.memorySize));
  return UDS_SUCCESS;
}

/***********************************************************************/
int makeMasterIndex005(const Configuration *config, unsigned int numZones,
                       uint64_t volumeNonce, MasterIndex  **masterIndex)
{
  Parameters005 params = { .addressBits = 0 };
  int result = computeMasterIndexParameters005(config, &params);
  if (result != UDS_SUCCESS) {
    return result;
  }

  MasterIndex5 *mi5;
  result = ALLOCATE(1, MasterIndex5, "master index", &mi5);
  if (result != UDS_SUCCESS) {
    *masterIndex = NULL;
    return result;
  }

  mi5->common.abortRestoringMasterIndex     = abortRestoringMasterIndex_005;
  mi5->common.abortSavingMasterIndex        = abortSavingMasterIndex_005;
  mi5->common.finishSavingMasterIndex       = finishSavingMasterIndex_005;
  mi5->common.freeMasterIndex               = freeMasterIndex_005;
  mi5->common.getMasterIndexMemoryUsed      = getMasterIndexMemoryUsed_005;
  mi5->common.getMasterIndexRecord          = getMasterIndexRecord_005;
  mi5->common.getMasterIndexStats           = getMasterIndexStats_005;
  mi5->common.getMasterIndexZone            = getMasterIndexZone_005;
  mi5->common.isMasterIndexSample           = isMasterIndexSample_005;
  mi5->common.isRestoringMasterIndexDone    = isRestoringMasterIndexDone_005;
  mi5->common.isSavingMasterIndexDone       = isSavingMasterIndexDone_005;
  mi5->common.lookupMasterIndexName         = lookupMasterIndexName_005;
  mi5->common.lookupMasterIndexSampledName  = lookupMasterIndexSampledName_005;
  mi5->common.restoreDeltaListToMasterIndex = restoreDeltaListToMasterIndex_005;
  mi5->common.setMasterIndexOpenChapter     = setMasterIndexOpenChapter_005;
  mi5->common.setMasterIndexTag             = setMasterIndexTag_005;
  mi5->common.setMasterIndexZoneOpenChapter = setMasterIndexZoneOpenChapter_005;
  mi5->common.startRestoringMasterIndex     = startRestoringMasterIndex_005;
  mi5->common.startSavingMasterIndex        = startSavingMasterIndex_005;

  mi5->addressBits     = params.addressBits;
  mi5->addressMask     = (1u << params.addressBits) - 1;
  mi5->chapterBits     = params.chapterBits;
  mi5->chapterMask     = (1u << params.chapterBits) - 1;
  mi5->numChapters     = params.numChapters;
  mi5->numDeltaLists   = params.numDeltaLists;
  mi5->numZones        = numZones;
  mi5->chapterZoneBits = params.numBitsPerChapter / numZones;
  mi5->volumeNonce     = volumeNonce;

  result = initializeDeltaIndex(&mi5->deltaIndex, numZones,
                                params.numDeltaLists, params.meanDelta,
                                params.chapterBits, params.memorySize);
  if (result == UDS_SUCCESS) {
    mi5->maxZoneBits = ((getDeltaIndexDlistBitsAllocated(&mi5->deltaIndex)
                         - params.targetFreeSize * CHAR_BIT)
                        / numZones);
  }

  // Initialize the chapter flush ranges to be empty.  This depends upon
  // allocate returning zeroed memory.
  if (result == UDS_SUCCESS) {
    result = ALLOCATE(params.numDeltaLists, uint64_t,
                      "first chapter to flush", &mi5->flushChapters);
  }

  // Initialize the virtual chapter ranges to start at zero.  This depends
  // upon allocate returning zeroed memory.
  if (result == UDS_SUCCESS) {
    result = ALLOCATE(numZones, MasterIndexZone, "master index zones",
                      &mi5->masterZones);
  }

  if (result == UDS_SUCCESS) {
    *masterIndex = &mi5->common;
  } else {
    freeMasterIndex_005(&mi5->common);
    *masterIndex = NULL;
  }
  return result;
}
