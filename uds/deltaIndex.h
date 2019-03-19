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
 * $Id: //eng/uds-releases/gloria/src/uds/deltaIndex.h#1 $
 */

#ifndef DELTAINDEX_H
#define DELTAINDEX_H 1

#include "compiler.h"
#include "deltaMemory.h"

enum {
  // the number of extra bytes and bits needed to store a collision entry
  COLLISION_BYTES = UDS_CHUNK_NAME_SIZE,
  COLLISION_BITS  = COLLISION_BYTES * CHAR_BIT
};

typedef struct deltaIndex {
  DeltaMemory *deltaZones;    // The zones
  unsigned int numZones;      // The number of zones
  unsigned int numLists;      // The number of delta lists
  unsigned int listsPerZone;  // Lists per zone (last zone can be smaller)
  bool isMutable;             // True if this index is mutable
  byte tag;                   // Tag belonging to this delta index
} DeltaIndex;

/*
 * Notes on the DeltaIndexEntries:
 *
 * The fields documented as "public" can be read by any code that uses a
 * DeltaIndex.  The fields documented as "private" carry information
 * between DeltaIndex method calls and should not be used outside the
 * DeltaIndex module.
 *
 * (1) The DeltaIndexEntry is used like an iterator when searching a delta
 *     list.
 *
 * (2) And it is also the result of a successful search and can be used to
 *     refer to the element found by the search.
 *
 * (3) And it is also the result of an unsuccessful search and can be used
 *     to refer to the insertion point for a new record.
 *
 * (4) If atEnd==true, the DeltaListEntry can only be used as the insertion
 *     point for a new record at the end of the list.
 *
 * (5) If atEnd==false and isCollision==true, the DeltaListEntry fields
 *     refer to a collision entry in the list, and the DeltaListEntry can
 *     be used a a reference to this entry.
 *
 * (6) If atEnd==false and isCollision==false, the DeltaListEntry fields
 *     refer to a non-collision entry in the list.  Such DeltaListEntries
 *     can be used as a reference to a found entry, or an insertion point
 *     for a non-collision entry before this entry, or an insertion point
 *     for a collision entry that collides with this entry.
 */

typedef struct deltaIndexEntry {
  // Public fields
  unsigned int key;          // The key for this entry
  bool         atEnd;        // We are after the last entry in the list
  bool         isCollision;  // This record is a collision
  // Private fields (but DeltaIndex_t1 cheats and looks at them)
  bool           listOverflow;  // This delta list overflowed
  unsigned short valueBits;     // The number of bits used for the value
  unsigned short entryBits;     // The number of bits used for the entire entry
  DeltaMemory   *deltaZone;     // The delta index zone
  DeltaList     *deltaList;     // The delta list containing the entry,
  unsigned int   listNumber;    // The delta list number
  uint32_t       offset;        // Bit offset of this entry within the list
  unsigned int   delta;         // The delta between this and previous entry
  DeltaList      tempDeltaList; // Temporary delta list for immutable indices
} DeltaIndexEntry;

typedef struct {
  size_t memoryAllocated;  // Number of bytes allocated
  RelTime rebalanceTime;   // The time spent rebalancing
  int  rebalanceCount;     // Number of memory rebalances
  long recordCount;        // The number of records in the index
  long collisionCount;     // The number of collision records
  long discardCount;       // The number of records removed
  long overflowCount;      // The number of UDS_OVERFLOWs detected
  unsigned int numLists;   // The number of delta lists
} DeltaIndexStats;

/**
 * Initialize a delta index.
 *
 * @param deltaIndex      The delta index to initialize
 * @param numZones        The number of zones in the index
 * @param numLists        The number of delta lists in the index
 * @param meanDelta       The mean delta value
 * @param numPayloadBits  The number of bits in the payload or value
 * @param memorySize      The number of bytes in memory for the index
 *
 * @return error code or UDS_SUCCESS
 **/
int initializeDeltaIndex(DeltaIndex *deltaIndex, unsigned int numZones,
                         unsigned int numLists, unsigned int meanDelta,
                         unsigned int numPayloadBits, size_t memorySize)
  __attribute__((warn_unused_result));

/**
 * Initialize an immutable delta index page.
 *
 * @param deltaIndex      The delta index to initialize
 * @param deltaMemory     The delta memory used for the immutable delta zone
 * @param expectedNonce   If non-zero, the expected nonce.
 * @param meanDelta       The mean delta value
 * @param numPayloadBits  The number of bits in the payload or value
 * @param memory          The memory page
 * @param memSize         The size of the memory page
 *
 * @return error code or UDS_SUCCESS
 **/
int initializeDeltaIndexPage(DeltaIndex *deltaIndex, DeltaMemory *deltaMemory,
                             uint64_t expectedNonce, unsigned int meanDelta,
                             unsigned int numPayloadBits, byte *memory,
                             size_t memSize)
  __attribute__((warn_unused_result));

/**
 * Uninitialize a delta index.
 *
 * @param deltaIndex  The delta index to uninitialize
 **/
void uninitializeDeltaIndex(DeltaIndex *deltaIndex);

/**
 * Empty the delta index.
 *
 * @param deltaIndex  The delta index being emptied.
 **/
void emptyDeltaIndex(const DeltaIndex *deltaIndex);

/**
 * Empty a zone of the delta index.
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone being emptied
 **/
void emptyDeltaIndexZone(const DeltaIndex *deltaIndex,
                         unsigned int zoneNumber);

/**
 * Pack delta lists from a mutable delta index into an immutable delta
 * index page.  A range of delta lists (starting with a specified list
 * index) is copied from the mutable delta index into a memory page used
 * in the immutable index.  The number of lists copied onto the page is
 * returned to the caller.
 *
 * @param deltaIndex            The delta index being converted
 * @param headerNonce           The header nonce to store
 * @param memory                The memory page to use
 * @param memSize               The size of the memory page
 * @param virtualChapterNumber  The virtual chapter number
 * @param firstList             The first delta list number to be copied
 * @param numLists              The number of delta lists that were copied
 *
 * @return error code or UDS_SUCCESS.  On UDS_SUCCESS, the numLists
 *         argument contains the number of lists copied.
 **/
int packDeltaIndexPage(const DeltaIndex *deltaIndex, uint64_t headerNonce,
                       byte *memory, size_t memSize,
                       uint64_t virtualChapterNumber, unsigned int firstList,
                       unsigned int *numLists)
  __attribute__((warn_unused_result));

/**
 * Set the tag value used when saving and/or restoring a delta index.
 *
 * @param deltaIndex  The delta index
 * @param tag         The tag value
 **/
void setDeltaIndexTag(DeltaIndex *deltaIndex, byte tag);

/**
 * Start restoring a delta index from an input stream.
 *
 * @param deltaIndex       The delta index to read into
 * @param bufferedReaders  The buffered readers to read the delta index from
 * @param numReaders       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int startRestoringDeltaIndex(const DeltaIndex *deltaIndex,
                             BufferedReader **bufferedReaders, int numReaders)
  __attribute__((warn_unused_result));

/**
 * Have all the data been read while restoring a delta index from an
 * input stream?
 *
 * @param deltaIndex  The delta index
 *
 * @return true if all the data are read
 **/
bool isRestoringDeltaIndexDone(const DeltaIndex *deltaIndex);

/**
 * Restore a saved delta list
 *
 * @param deltaIndex  The delta index
 * @param dlsi        The DeltaListSaveInfo describing the delta list
 * @param data        The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
int restoreDeltaListToDeltaIndex(const DeltaIndex *deltaIndex,
                                 const DeltaListSaveInfo *dlsi,
                                 const byte data[DELTA_LIST_MAX_BYTE_COUNT])
  __attribute__((warn_unused_result));

/**
 * Abort restoring a delta index from an input stream.
 *
 * @param deltaIndex  The delta index
 **/
void abortRestoringDeltaIndex(const DeltaIndex *deltaIndex);

/**
 * Start saving a delta index zone to a buffered output stream.
 *
 * @param deltaIndex      The delta index
 * @param zoneNumber      The zone number
 * @param bufferedWriter  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int startSavingDeltaIndex(const DeltaIndex *deltaIndex,
                          unsigned int zoneNumber,
                          BufferedWriter *bufferedWriter)
  __attribute__((warn_unused_result));

/**
 * Have all the data been written while saving a delta index zone to an
 * output stream?  If the answer is yes, it is still necessary to call
 * finishSavingDeltaIndex(), which will return quickly.
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return true if all the data are written
 **/
bool isSavingDeltaIndexDone(const DeltaIndex *deltaIndex,
                            unsigned int zoneNumber);

/**
 * Finish saving a delta index zone to an output stream.  Force the writing
 * of all of the remaining data.  If an error occurred asynchronously
 * during the save operation, it will be returned here.
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int finishSavingDeltaIndex(const DeltaIndex *deltaIndex,
                           unsigned int zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Abort saving a delta index zone to an output stream.  If an error
 * occurred asynchronously during the save operation, it will be dropped.
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int abortSavingDeltaIndex(const DeltaIndex *deltaIndex,
                          unsigned int zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Compute the number of bytes required to save a delta index
 *
 * @param numLists    The number of delta lists in the index
 * @param memorySize  The number of bytes in memory for the index
 *
 * @return numBytes  The number of bytes required to save the master index
 **/
size_t computeDeltaIndexSaveBytes(unsigned int numLists, size_t memorySize)
  __attribute__((warn_unused_result));

/**
 * Validate the delta index
 *
 * @param deltaIndex  The delta index
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int validateDeltaIndex(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Prepare to search for an entry in the specified delta list.
 *
 * <p> This is always the first routine to be called when dealing with delta
 * index entries. It is always followed by calls to nextDeltaIndexEntry to
 * iterate through a delta list. The fields of the DeltaIndexEntry argument
 * will be set up for iteration, but will not contain an entry from the list.
 *
 * @param deltaIndex  The delta index to search
 * @param listNumber  The delta list number
 * @param key         First delta list key that the caller is interested in
 * @param readOnly    True if this is a read-only operation
 * @param iterator    The index entry being used to search through the list
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int startDeltaIndexSearch(const DeltaIndex *deltaIndex,
                          unsigned int listNumber, unsigned int key,
                          bool readOnly, DeltaIndexEntry *iterator)
  __attribute__((warn_unused_result));

/**
 * Find the next entry in the specified delta list
 *
 * @param deltaEntry  Info about an entry, which is updated to describe the
 *                    following entry
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int nextDeltaIndexEntry(DeltaIndexEntry *deltaEntry)
  __attribute__((warn_unused_result));

/**
 * Remember the position of a delta index entry, so that we can use it when
 * starting the next search.
 *
 * @param deltaEntry  Info about an entry found during a search.  This should
 *                    be the first entry that matches the key exactly (i.e.
 *                    not a collision entry), or the first entry with a key
 *                    greater than the entry sought for.
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int rememberDeltaIndexOffset(const DeltaIndexEntry *deltaEntry)
  __attribute__((warn_unused_result));

/**
 * Find the delta index entry, or the insertion point for a delta index
 * entry.
 *
 * @param deltaIndex  The delta index to search
 * @param listNumber  The delta list number
 * @param key         The key field being looked for
 * @param name        The 256 bit full name
 * @param readOnly    True if this is a read-only index search
 * @param deltaEntry  Updated to describe the entry being looked for
 *
 * @return UDS_SUCCESS or an error code
 **/
int getDeltaIndexEntry(const DeltaIndex *deltaIndex, unsigned int listNumber,
                       unsigned int key, const byte *name, bool readOnly,
                       DeltaIndexEntry *deltaEntry)
  __attribute__((warn_unused_result));

/**
 * Get the full name from a collision DeltaIndexEntry
 *
 * @param deltaEntry  The delta index record
 * @param name        The 256 bit full name
 *
 * @return UDS_SUCCESS or an error code
 **/
int getDeltaEntryCollision(const DeltaIndexEntry *deltaEntry, byte *name)
  __attribute__((warn_unused_result));

/**
 * Get the bit offset into delta memory of a delta index entry.
 *
 * @param deltaEntry  The delta index entry
 *
 * @return the bit offset into delta memory
 **/
static INLINE uint64_t getDeltaEntryOffset(const DeltaIndexEntry *deltaEntry)
{
  return getDeltaListStart(deltaEntry->deltaList) + deltaEntry->offset;
}

/**
 * Get the number of bits used to encode the entry key (the delta).
 *
 * @param entry         The delta index record
 *
 * @return the number of bits used to encode the key
 **/
static INLINE unsigned int getDeltaEntryKeyBits(const DeltaIndexEntry *entry)
{
  /*
   * Derive keyBits by subtracting the sizes of the other two fields from the
   * total. We don't actually use this for encoding/decoding, so it doesn't
   * need to be super-fast. We save time where it matters by not storing it.
   */
  return (entry->entryBits - entry->valueBits
          - (entry->isCollision ? COLLISION_BITS : 0));
}

/**
 * Get the value field of the DeltaIndexEntry
 *
 * @param deltaEntry  The delta index record
 *
 * @return the value
 **/
static INLINE unsigned int getDeltaEntryValue(const DeltaIndexEntry *deltaEntry)
{
  return getField(deltaEntry->deltaZone->memory,
                  getDeltaEntryOffset(deltaEntry), deltaEntry->valueBits);
}

/**
 * Set the value field of the DeltaIndexEntry
 *
 * @param deltaEntry  The delta index record
 * @param value       The new value
 *
 * @return UDS_SUCCESS or an error code
 **/
int setDeltaEntryValue(const DeltaIndexEntry *deltaEntry, unsigned int value)
  __attribute__((warn_unused_result));

/**
 * Create a new entry in the delta index
 *
 * @param deltaEntry  The delta index entry that indicates the insertion point
 *                    for the new record.  For a collision entry, this is the
 *                    non-collision entry that the new entry collides with.
 *                    For a non-collision entry, this new entry is inserted
 *                    before the specified entry.
 * @param key         The key field
 * @param value       The value field
 * @param name        For collision entries, the 256 bit full name;
 *                    Otherwise null
 *
 * @return UDS_SUCCESS or an error code
 **/
int putDeltaIndexEntry(DeltaIndexEntry *deltaEntry, unsigned int key,
                       unsigned int value, const byte *name)
  __attribute__((warn_unused_result));

/**
 * Remove an existing delta index entry, and advance to the next entry in
 * the delta list.
 *
 * @param deltaEntry  On call the delta index record to remove.  After
 *                    returning, the following entry in the delta list.
 *
 * @return UDS_SUCCESS or an error code
 **/
int removeDeltaIndexEntry(DeltaIndexEntry *deltaEntry)
  __attribute__((warn_unused_result));

/**
 * Map a delta list number to a delta zone number
 *
 * @param deltaIndex  The delta index
 * @param listNumber  The delta list number
 *
 * @return the zone number containing the delta list
 **/
static INLINE unsigned int getDeltaIndexZone(const DeltaIndex *deltaIndex,
                                             unsigned int listNumber)
{
  return listNumber / deltaIndex->listsPerZone;
}

/**
 * Get the first delta list number in a zone
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return the first delta list index in the zone
 **/
unsigned int getDeltaIndexZoneFirstList(const DeltaIndex *deltaIndex,
                                        unsigned int zoneNumber);

/**
 * Get the number of delta lists in a zone
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return the number of delta lists in the zone
 **/
unsigned int getDeltaIndexZoneNumLists(const DeltaIndex *deltaIndex,
                                        unsigned int zoneNumber);

/**
 * Get the number of bytes used for master index entries in a zone
 *
 * @param deltaIndex  The delta index
 * @param zoneNumber  The zone number
 *
 * @return The number of bits in use
 **/
uint64_t getDeltaIndexZoneDlistBitsUsed(const DeltaIndex *deltaIndex,
                                        unsigned int zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Get the number of bytes used for master index entries.
 *
 * @param deltaIndex  The delta index
 *
 * @return The number of bits in use
 **/
uint64_t getDeltaIndexDlistBitsUsed(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Get the number of bytes allocated for master index entries.
 *
 * @param deltaIndex  The delta index
 *
 * @return The number of bits allocated
 **/
uint64_t getDeltaIndexDlistBitsAllocated(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Get the delta index statistics.
 *
 * @param deltaIndex  The delta index
 * @param stats       The statistics
 **/
void getDeltaIndexStats(const DeltaIndex *deltaIndex, DeltaIndexStats *stats);

/**
 * Get the virtual chapter number for the given delta index page.
 *
 * @param deltaIndex    The delta index
 *
 * @return the virtual chapter number
 **/
uint64_t getDeltaIndexVirtualChapterNumber(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Get the lowest numbered delta list for the given index.
 *
 * @param deltaIndex    The delta index
 *
 * @return              The number of the first delta list in the index
 **/
unsigned int getDeltaIndexLowestListNumber(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Get the highest numbered delta list for the given index.
 *
 * @param deltaIndex    The delta index
 *
 * @return              The number of the last delta list in the index
 **/
unsigned int getDeltaIndexHighestListNumber(const DeltaIndex *deltaIndex)
  __attribute__((warn_unused_result));

/**
 * Get the number of pages needed for an immutable delta index.
 *
 * @param numEntries      The number of entries in the index
 * @param numLists        The number of delta lists
 * @param meanDelta       The mean delta value
 * @param numPayloadBits  The number of bits in the payload or value
 * @param bytesPerPage    The number of bytes in a page
 *
 * @return the number of pages needed for the index
 **/
unsigned int getDeltaIndexPageCount(unsigned int numEntries,
                                    unsigned int numLists,
                                    unsigned int meanDelta,
                                    unsigned int numPayloadBits,
                                    size_t bytesPerPage);

/**
 * Log a delta index entry, and any error conditions related to the entry.
 *
 * @param deltaEntry  The delta index entry.
 **/
void logDeltaIndexEntry(DeltaIndexEntry *deltaEntry);

#endif /* DELTAINDEX_H */
