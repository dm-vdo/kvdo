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
 * $Id: //eng/uds-releases/gloria/src/uds/deltaMemory.h#1 $
 */

#ifndef DELTAMEMORY_H
#define DELTAMEMORY_H 1

#include "bits.h"
#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "cpu.h"
#include "timeUtils.h"

/*
 * We encode the delta list information into 16 bytes per list.
 *
 * Because the master index has 1 million delta lists, each byte of header
 * information ends up costing us 1MB.  We have an incentive to keep the
 * size down.
 *
 * The master index delta list memory is currently about 780MB in size,
 * which is more than 6 gigabits.  Therefore we need at least 33 bits to
 * address the master index memory and we use the uint64_t type.
 *
 * The master index delta lists have 256 entries of about 24 bits each,
 * which is 6K bits.  The index needs 13 bits to represent the size of a
 * delta list and we use the uint16_t type.
 */

typedef struct deltaList {
  uint64_t startOffset;  // The offset of the delta list start within memory
  uint16_t size;         // The number of bits in the delta list
  uint16_t saveOffset;   // Where the last search "found" the key
  unsigned int saveKey;  // The key for the record just before saveOffset.
} DeltaList;

typedef struct __attribute__((aligned(CACHE_LINE_BYTES))) deltaMemory {
  byte *memory;                   // The delta list memory
  DeltaList *deltaLists;          // The delta list headers
  uint64_t *tempOffsets;          // Temporary starts of delta lists
  byte *flags;                    // Transfer flags
  BufferedWriter *bufferedWriter; // Buffered writer for saving an index
  size_t size;                 // The size of delta list memory
  RelTime rebalanceTime;       // The time spent rebalancing
  int rebalanceCount;          // Number of memory rebalances
  unsigned short valueBits;    // The number of bits of value
  unsigned short minBits;      // The number of bits in the minimal key code
  unsigned int minKeys;        // The number of keys used in a minimal code
  unsigned int incrKeys;       // The number of keys used for another code bit
  long recordCount;            // The number of records in the index
  long collisionCount;         // The number of collision records
  long discardCount;           // The number of records removed
  long overflowCount;          // The number of UDS_OVERFLOWs detected
  unsigned int firstList;      // The index of the first delta list
  unsigned int numLists;       // The number of delta lists
  unsigned int numTransfers;   // Number of transfer flags that are set
  int transferStatus;          // Status of the transfers in progress
  byte tag;                    // Tag belonging to this delta index
} DeltaMemory;

typedef struct deltaListSaveInfo {
  uint8_t tag;         // Tag identifying which delta index this list is in
  uint8_t bitOffset;   // Bit offset of the start of the list data
  uint16_t byteCount;  // Number of bytes of list data
  uint32_t index;      // The delta list number within the delta index
} DeltaListSaveInfo;

// The maximum size of a single delta list (in bytes).  We add guard bytes
// to this because such a buffer can be used with moveBits.
enum { DELTA_LIST_MAX_BYTE_COUNT = ((UINT16_MAX + CHAR_BIT) / CHAR_BIT
                                    + POST_FIELD_GUARD_BYTES) };

/**
 * Initialize delta list memory.
 *
 * @param deltaMemory     A delta memory structure
 * @param size            The initial size of the memory array
 * @param firstList       The index of the first delta list
 * @param numLists        The number of delta lists
 * @param meanDelta       The mean delta
 * @param numPayloadBits  The number of payload bits
 *
 * @return error code or UDS_SUCCESS
 **/
int initializeDeltaMemory(DeltaMemory *deltaMemory, size_t size,
                          unsigned int firstList, unsigned int numLists,
                          unsigned int meanDelta, unsigned int numPayloadBits)
  __attribute__((warn_unused_result));

/**
 * Uninitialize delta list memory.
 *
 * @param deltaMemory  A delta memory structure
 **/
void uninitializeDeltaMemory(DeltaMemory *deltaMemory);

/**
 * Initialize delta list memory to refer to a cached page.
 *
 * @param deltaMemory     A delta memory structure
 * @param memory          The memory page
 * @param size            The size of the memory page
 * @param numLists        The number of delta lists
 * @param meanDelta       The mean delta
 * @param numPayloadBits  The number of payload bits
 **/
void initializeDeltaMemoryPage(DeltaMemory *deltaMemory, byte *memory,
                               size_t size, unsigned int numLists,
                               unsigned int meanDelta,
                               unsigned int numPayloadBits);

/**
 * Empty the delta lists.
 *
 * @param deltaMemory  The delta memory
 **/
void emptyDeltaLists(DeltaMemory *deltaMemory);

/**
 * Is there a delta list memory save or restore in progress?
 *
 * @param deltaMemory  A delta memory structure
 *
 * @return true if there are no delta lists that need to be saved or
 *         restored
 **/
bool areDeltaMemoryTransfersDone(const DeltaMemory *deltaMemory);

/**
 * Start restoring delta list memory from a file descriptor
 *
 * @param deltaMemory     A delta memory structure
 *
 * @return error code or UDS_SUCCESS
 **/
int startRestoringDeltaMemory(DeltaMemory *deltaMemory)
  __attribute__((warn_unused_result));

/**
 * Read a saved delta list from a file descriptor
 *
 * @param dlsi            The DeltaListSaveInfo describing the delta list
 * @param data            The saved delta list bit stream
 * @param bufferedReader  The buffered reader to read the delta list from
 *
 * @return error code or UDS_SUCCESS
 *         or UDS_END_OF_FILE at end of the data stream
 **/
int readSavedDeltaList(DeltaListSaveInfo *dlsi,
                       byte data[DELTA_LIST_MAX_BYTE_COUNT],
                       BufferedReader *bufferedReader)
  __attribute__((warn_unused_result));

/**
 * Restore a saved delta list
 *
 * @param deltaMemory  A delta memory structure
 * @param dlsi         The DeltaListSaveInfo describing the delta list
 * @param data         The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
int restoreDeltaList(DeltaMemory *deltaMemory, const DeltaListSaveInfo *dlsi,
                     const byte data[DELTA_LIST_MAX_BYTE_COUNT])
  __attribute__((warn_unused_result));

/**
 * Abort restoring delta list memory from an input stream.
 *
 * @param deltaMemory  A delta memory structure
 **/
void abortRestoringDeltaMemory(DeltaMemory *deltaMemory);

/**
 * Start saving delta list memory to a buffered output stream
 *
 * @param deltaMemory     A delta memory structure
 * @param bufferedWriter  The index state component being written
 **/
void startSavingDeltaMemory(DeltaMemory *deltaMemory,
                            BufferedWriter *bufferedWriter);

/**
 * Finish saving delta list memory to an output stream.  Force the writing
 * of all of the remaining data.  If an error occurred asynchronously
 * during the save operation, it will be returned here.
 *
 * @param deltaMemory  A delta memory structure
 *
 * @return error code or UDS_SUCCESS
 **/
int finishSavingDeltaMemory(DeltaMemory *deltaMemory)
  __attribute__((warn_unused_result));

/**
 * Abort saving delta list memory to an output stream.  If an error
 * occurred asynchronously during the save operation, it will be dropped.
 *
 * @param deltaMemory  A delta memory structure
 **/
void abortSavingDeltaMemory(DeltaMemory *deltaMemory);

/**
 * Flush a delta list to an output stream
 *
 * @param deltaMemory  A delta memory structure
 * @param flushIndex   Index of the delta list that may need to be flushed.
 **/
void flushDeltaList(DeltaMemory *deltaMemory, unsigned int flushIndex);

/**
 * Write a guard delta list to mark the end of the saved data
 *
 * @param bufferedWriter  The buffered writer to write the guard delta list to
 *
 * @return error code or UDS_SUCCESS
 **/
int writeGuardDeltaList(BufferedWriter *bufferedWriter)
  __attribute__((warn_unused_result));

/**
 * Extend the memory used by the delta lists and rebalance the lists in the
 * new chunk.
 *
 * <p> The delta memory contains N delta lists, which are guarded by two
 * empty delta lists.  The valid delta lists are numbered 1 to N, and the
 * guards are numbered 0 and (N+1);
 *
 * <p> When the delta lista are bit streams, it is possible that the tail
 * of list J and the head of list (J+1) are in the same byte.  In this case
 * oldOffsets[j]+sizes[j]==oldOffset[j]-1.  We handle this correctly.
 *
 * @param deltaMemory   A delta memory structure
 * @param growingIndex  Index of the delta list that needs additional space
 *                      left before it (from 1 to N+1).
 * @param growingSize   Number of additional bytes needed before growingIndex
 * @param doCopy        True to copy the data, False to just balance the space
 *
 * @return UDS_SUCCESS or an error code
 **/
int extendDeltaMemory(DeltaMemory *deltaMemory, unsigned int growingIndex,
                      size_t growingSize, bool doCopy)
  __attribute__((warn_unused_result));

/**
 * Validate the delta list headers.
 *
 * @param deltaMemory   A delta memory structure
 *
 * @return UDS_SUCCESS or an error code
 **/
int validateDeltaLists(const DeltaMemory *deltaMemory)
  __attribute__((warn_unused_result));

/**
 * Get the number of bytes allocated for delta index entries and any
 * associated overhead.
 *
 * @param deltaMemory   A delta memory structure
 *
 * @return The number of bytes allocated
 **/
size_t getDeltaMemoryAllocated(const DeltaMemory *deltaMemory);

/**
 * Get the expected number of bits used in a delta index
 *
 * @param numEntries      The number of index entries
 * @param meanDelta       The mean delta value
 * @param numPayloadBits  The number of bits in the payload or value
 *
 * @return  The expected size of a delta index in bits
 **/
size_t getDeltaMemorySize(unsigned long numEntries, unsigned int meanDelta,
                          unsigned int numPayloadBits)
  __attribute__((warn_unused_result));

/**
 * Get the bit offset to the start of the delta list bit stream
 *
 * @param deltaList  The delta list header
 *
 * @return the start of the delta list
 **/
static INLINE uint64_t getDeltaListStart(const DeltaList *deltaList)
{
  return deltaList->startOffset;
}

/**
 * Get the number of bits in a delta list bit stream
 *
 * @param deltaList  The delta list header
 *
 * @return the size of the delta list
 **/
static INLINE uint16_t getDeltaListSize(const DeltaList *deltaList)
{
  return deltaList->size;
}

/**
 * Get the bit offset to the end of the delta list bit stream
 *
 * @param deltaList  The delta list header
 *
 * @return the end of the delta list
 **/
static INLINE uint64_t getDeltaListEnd(const DeltaList *deltaList)
{
  return getDeltaListStart(deltaList) + getDeltaListSize(deltaList);
}

/**
 * Identify mutable vs. immutable delta memory
 *
 * Mutable delta memory contains delta lists that can be modified, and is
 * initialized using initializeDeltaMemory().
 *
 * Immutable delta memory contains packed delta lists, cannot be modified,
 * and is initialized using initializeDeltaMemoryPage().
 *
 * For mutable delta memory, all of the following expressions are true.
 * And for immutable delta memory, all of the following expressions are
 * false.
 *             deltaLists != NULL
 *             tempOffsets != NULL
 *             flags != NULL
 *
 * @param deltaMemory  A delta memory structure
 *
 * @return true if the delta memory is mutable
 **/
static INLINE bool isMutable(const DeltaMemory *deltaMemory)
{
  return deltaMemory->deltaLists != NULL;
}

/**
 * Lazily flush a delta list to an output stream
 *
 * @param deltaMemory  A delta memory structure
 * @param flushIndex   Index of the delta list that may need to be flushed.
 **/
static INLINE void lazyFlushDeltaList(DeltaMemory *deltaMemory,
                                      unsigned int flushIndex)
{
  if (getField(deltaMemory->flags, flushIndex, 1) != 0) {
    flushDeltaList(deltaMemory, flushIndex);
  }
}
#endif /* DELTAMEMORY_H */
