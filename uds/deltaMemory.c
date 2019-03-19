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
 * $Id: //eng/uds-releases/gloria/src/uds/deltaMemory.c#4 $
 */
#include "deltaMemory.h"

#include "bits.h"
#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "featureDefs.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "timeUtils.h"
#include "typeDefs.h"
#include "uds.h"

/*
 * The DeltaMemory structure manages the memory that stores delta lists.
 *
 * The "mutable" form of DeltaMemory is used for the master index and for
 * an open chapter index.  The "immutable" form of DeltaMemory is used for
 * regular chapter indices.
 */

// This is the number of guard bits that are needed in the tail guard list
enum { GUARD_BITS = POST_FIELD_GUARD_BYTES * CHAR_BIT };

/**
 * Get the offset of the first byte that a delta list bit stream resides in
 *
 * @param deltaList  The delta list
 *
 * @return the number byte offset
 **/
static INLINE uint64_t getDeltaListByteStart(const DeltaList *deltaList)
{
  return getDeltaListStart(deltaList) / CHAR_BIT;
}

/**
 * Get the actual number of bytes that a delta list bit stream resides in
 *
 * @param deltaList  The delta list
 *
 * @return the number of bytes
 **/
static INLINE uint16_t getDeltaListByteSize(const DeltaList *deltaList)
{
  uint16_t startBitOffset = getDeltaListStart(deltaList) % CHAR_BIT;
  uint16_t bitSize = getDeltaListSize(deltaList);
  return ((unsigned int) startBitOffset + bitSize + CHAR_BIT - 1) / CHAR_BIT;
}

/**
 * Get the number of bytes in the delta lists headers.
 *
 * @param numLists  The number of delta lists
 *
 * @return the number of bytes in the delta lists headers
 **/
static INLINE size_t getSizeOfDeltaLists(unsigned int numLists)
{
  return (numLists + 2) * sizeof(DeltaList);
}

/**
 * Get the size of the flags array (in bytes)
 *
 * @param numLists  The number of delta lists
 *
 * @return the number of bytes for an array that has one bit per delta
 *         list, plus the necessary guard bytes.
 **/
static INLINE size_t getSizeOfFlags(unsigned int numLists)
{
  return (numLists + CHAR_BIT - 1) / CHAR_BIT + POST_FIELD_GUARD_BYTES;
}

/**
 * Get the number of bytes of scratch memory for the delta lists.
 *
 * @param numLists  The number of delta lists
 *
 * @return the number of bytes of scratch memory for the delta lists
 **/
static INLINE size_t getSizeOfTempOffsets(unsigned int numLists)
{
  return (numLists + 2) * sizeof(uint64_t);
}

/**********************************************************************/

/**
 * Clear the transfers flags.
 *
 * @param deltaMemory  The delta memory
 **/
static void clearTransferFlags(DeltaMemory *deltaMemory)
{
  memset(deltaMemory->flags, 0, getSizeOfFlags(deltaMemory->numLists));
  deltaMemory->numTransfers = 0;
  deltaMemory->transferStatus = UDS_SUCCESS;
}

/**********************************************************************/

/**
 * Set the transfer flags for delta lists that are not empty, and count how
 * many there are.
 *
 * @param deltaMemory  The delta memory
 **/
static void flagNonEmptyDeltaLists(DeltaMemory *deltaMemory)
{
  clearTransferFlags(deltaMemory);
  for (unsigned int i = 0; i < deltaMemory->numLists; i++) {
    if (getDeltaListSize(&deltaMemory->deltaLists[i + 1]) > 0) {
      setOne(deltaMemory->flags, i, 1);
      deltaMemory->numTransfers++;
    }
  }
}

/**********************************************************************/
void emptyDeltaLists(DeltaMemory *deltaMemory)
{
  // Zero all the delta list headers
  DeltaList *deltaLists = deltaMemory->deltaLists;
  memset(deltaLists, 0, getSizeOfDeltaLists(deltaMemory->numLists));

  /*
   * Initialize delta lists to be empty. We keep 2 extra delta list
   * descriptors, one before the first real entry and one after so that we
   * don't need to bounds check the array access when calculating
   * preceeding and following gap sizes.
   *
   * Because the delta list headers were zeroed, the head guard list is
   * already at offset zero and size zero.
   *
   * The end guard list contains guard bytes so that the bit field
   * utilities can safely read past the end of any byte we are interested
   * in.
   */
  uint64_t numBits = (uint64_t) deltaMemory->size * CHAR_BIT;
  deltaLists[deltaMemory->numLists + 1].startOffset = numBits - GUARD_BITS;
  deltaLists[deltaMemory->numLists + 1].size        = GUARD_BITS;

  // Set all the bits in the end guard list.  Do not use the bit field
  // utilities.
  memset(deltaMemory->memory + deltaMemory->size - POST_FIELD_GUARD_BYTES,
         ~0, POST_FIELD_GUARD_BYTES);

  // Evenly space out the real delta lists.  The sizes are already zero, so
  // we just need to set the starting offsets.
  uint64_t spacing = (numBits - GUARD_BITS) / deltaMemory->numLists;
  uint64_t offset = spacing / 2;
  for (unsigned int i = 1; i <= deltaMemory->numLists; i++) {
    deltaLists[i].startOffset = offset;
    offset += spacing;
  }

  // Update the statistics
  deltaMemory->discardCount  += deltaMemory->recordCount;
  deltaMemory->recordCount    = 0;
  deltaMemory->collisionCount = 0;
}

/**********************************************************************/
/**
 * Compute the Huffman coding parameters for the given mean delta
 *
 * @param meanDelta  The mean delta value
 * @param minBits    The number of bits in the minimal key code
 * @param minKeys    The number of keys used in a minimal code
 * @param incrKeys   The number of keys used for another code bit
 **/
static void computeCodingConstants(unsigned int    meanDelta,
                                   unsigned short *minBits,
                                   unsigned int   *minKeys,
                                   unsigned int   *incrKeys)
{
  // We want to compute the rounded value of log(2) * meanDelta.  Since we
  // cannot always use floating point, use a really good integer approximation.
  *incrKeys = (836158UL * meanDelta + 603160UL) / 1206321UL;
  *minBits  = computeBits(*incrKeys + 1);
  *minKeys  = (1 << *minBits) - *incrKeys;
}

/**********************************************************************/
/**
 * Rebalance a range of delta lists within memory.
 *
 * @param deltaMemory  A delta memory structure
 * @param first        The first delta list index
 * @param last         The last delta list index
 **/
static void rebalanceDeltaMemory(const DeltaMemory *deltaMemory,
                                 unsigned int first, unsigned int last)
{
  if (first == last) {
    DeltaList *deltaList = &deltaMemory->deltaLists[first];
    uint64_t newStart = deltaMemory->tempOffsets[first];
    // We need to move only one list, and we know it is safe to do so
    if (getDeltaListStart(deltaList) != newStart) {
      // Compute the first source byte
      uint64_t source = getDeltaListByteStart(deltaList);
      // Update the delta list location
      deltaList->startOffset = newStart;
      // Now use the same computation to locate the first destination byte
      uint64_t destination = getDeltaListByteStart(deltaList);
      memmove(deltaMemory->memory + destination, deltaMemory->memory + source,
              getDeltaListByteSize(deltaList));
    }
  } else {
    // There is more than one list.  Divide the problem in half, and use
    // recursive calls to process each half.  Note that after this
    // computation, first <= middle, and middle < last.
    unsigned int middle = (first + last) / 2;
    const DeltaList *deltaList = &deltaMemory->deltaLists[middle];
    uint64_t newStart = deltaMemory->tempOffsets[middle];
    // The direction that our middle list is moving determines which half
    // of the problem must be processed first.
    if (newStart > getDeltaListStart(deltaList)) {
      rebalanceDeltaMemory(deltaMemory, middle + 1, last);
      rebalanceDeltaMemory(deltaMemory, first, middle);
    } else {
      rebalanceDeltaMemory(deltaMemory, first, middle);
      rebalanceDeltaMemory(deltaMemory, middle + 1, last);
    }
  }
}

/**********************************************************************/
int initializeDeltaMemory(DeltaMemory *deltaMemory, size_t size,
                          unsigned int firstList, unsigned int numLists,
                          unsigned int meanDelta, unsigned int numPayloadBits)
{
  if (numLists == 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot initialize delta memory with 0 "
                                     "delta lists");
  }
  byte *memory = NULL;
  int result = ALLOCATE(size, byte, "delta list", &memory);
  if (result != UDS_SUCCESS) {
    return result;
  }
  uint64_t *tempOffsets = NULL;
  result = ALLOCATE(numLists + 2, uint64_t, "delta list temp",
                    &tempOffsets);
  if (result != UDS_SUCCESS) {
    FREE(memory);
    return result;
  }
  byte *flags = NULL;
  result = ALLOCATE(getSizeOfFlags(numLists), byte, "delta list flags",
                    &flags);
  if (result != UDS_SUCCESS) {
    FREE(memory);
    FREE(tempOffsets);
    return result;
  }

  computeCodingConstants(meanDelta, &deltaMemory->minBits,
                         &deltaMemory->minKeys, &deltaMemory->incrKeys);
  deltaMemory->valueBits       = numPayloadBits;
  deltaMemory->memory          = memory;
  deltaMemory->deltaLists      = NULL;
  deltaMemory->tempOffsets     = tempOffsets;
  deltaMemory->flags           = flags;
  deltaMemory->bufferedWriter  = NULL;
  deltaMemory->size            = size;
  deltaMemory->rebalanceTime   = 0;
  deltaMemory->rebalanceCount  = 0;
  deltaMemory->recordCount     = 0;
  deltaMemory->collisionCount  = 0;
  deltaMemory->discardCount    = 0;
  deltaMemory->overflowCount   = 0;
  deltaMemory->firstList       = firstList;
  deltaMemory->numLists        = numLists;
  deltaMemory->numTransfers    = 0;
  deltaMemory->transferStatus  = UDS_SUCCESS;
  deltaMemory->tag             = 'm';

  // Allocate the delta lists.
  result = ALLOCATE(deltaMemory->numLists + 2, DeltaList,
                    "delta lists", &deltaMemory->deltaLists);
  if (result != UDS_SUCCESS) {
    uninitializeDeltaMemory(deltaMemory);
    return result;
  }

  emptyDeltaLists(deltaMemory);
  return UDS_SUCCESS;
}

/**********************************************************************/
void uninitializeDeltaMemory(DeltaMemory *deltaMemory)
{
  FREE(deltaMemory->flags);
  deltaMemory->flags = NULL;
  FREE(deltaMemory->tempOffsets);
  deltaMemory->tempOffsets = NULL;
  FREE(deltaMemory->deltaLists);
  deltaMemory->deltaLists = NULL;
  FREE(deltaMemory->memory);
  deltaMemory->memory = NULL;
}

/**********************************************************************/
void initializeDeltaMemoryPage(DeltaMemory *deltaMemory, byte *memory,
                               size_t size, unsigned int numLists,
                               unsigned int meanDelta,
                               unsigned int numPayloadBits)
{
  computeCodingConstants(meanDelta, &deltaMemory->minBits,
                         &deltaMemory->minKeys, &deltaMemory->incrKeys);
  deltaMemory->valueBits       = numPayloadBits;
  deltaMemory->memory          = memory;
  deltaMemory->deltaLists      = NULL;
  deltaMemory->tempOffsets     = NULL;
  deltaMemory->flags           = NULL;
  deltaMemory->bufferedWriter  = NULL;
  deltaMemory->size            = size;
  deltaMemory->rebalanceTime   = 0;
  deltaMemory->rebalanceCount  = 0;
  deltaMemory->recordCount     = 0;
  deltaMemory->collisionCount  = 0;
  deltaMemory->discardCount    = 0;
  deltaMemory->overflowCount   = 0;
  deltaMemory->firstList       = 0;
  deltaMemory->numLists        = numLists;
  deltaMemory->numTransfers    = 0;
  deltaMemory->transferStatus  = UDS_SUCCESS;
  deltaMemory->tag             = 'p';
}

/**********************************************************************/
bool areDeltaMemoryTransfersDone(const DeltaMemory *deltaMemory)
{
  return deltaMemory->numTransfers == 0;
}

/**********************************************************************/
int startRestoringDeltaMemory(DeltaMemory *deltaMemory)
{
  // Extend and balance memory to receive the delta lists
  int result = extendDeltaMemory(deltaMemory, 0, 0, false);
  if (result != UDS_SUCCESS) {
    return UDS_SUCCESS;
  }

  // The tail guard list needs to be set to ones
  DeltaList *deltaList = &deltaMemory->deltaLists[deltaMemory->numLists + 1];
  setOne(deltaMemory->memory, getDeltaListStart(deltaList),
         getDeltaListSize(deltaList));

  flagNonEmptyDeltaLists(deltaMemory);
  return UDS_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int readDeltaListSaveInfo(BufferedReader *reader,
                                 DeltaListSaveInfo *dlsi)
{
  byte buffer[sizeof(DeltaListSaveInfo)];
  int result = readFromBufferedReader(reader, buffer, sizeof(buffer));
  if (result != UDS_SUCCESS) {
    return result;
  }
  dlsi->tag =  buffer[0];
  dlsi->bitOffset = buffer[1];
  dlsi->byteCount = getUInt16LE(&buffer[2]);
  dlsi->index = getUInt32LE(&buffer[4]);
  return result;
}

/**********************************************************************/
int readSavedDeltaList(DeltaListSaveInfo *dlsi,
                       byte data[DELTA_LIST_MAX_BYTE_COUNT],
                       BufferedReader *bufferedReader)
{
  int result = readDeltaListSaveInfo(bufferedReader, dlsi);
  if (result == UDS_END_OF_FILE) {
    return UDS_END_OF_FILE;
  }
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result, "failed to read delta list data");
  }
  if ((dlsi->bitOffset >= CHAR_BIT)
      || (dlsi->byteCount > DELTA_LIST_MAX_BYTE_COUNT)) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "corrupt delta list data");
  }
  if (dlsi->tag == 'z') {
    return UDS_END_OF_FILE;
  }
  result = readFromBufferedReader(bufferedReader, data, dlsi->byteCount);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result, "failed to read delta list data");
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int restoreDeltaList(DeltaMemory *deltaMemory, const DeltaListSaveInfo *dlsi,
                     const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
  unsigned int listNumber = dlsi->index - deltaMemory->firstList;
  if (listNumber >= deltaMemory->numLists) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "invalid delta list number %u not in"
                                     " range [%u,%u)",
                                     dlsi->index, deltaMemory->firstList,
                                     deltaMemory->firstList
                                     + deltaMemory->numLists);
  }

  if (getField(deltaMemory->flags, listNumber, 1) == 0) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "unexpected delta list number %u",
                                     dlsi->index);
  }

  DeltaList *deltaList = &deltaMemory->deltaLists[listNumber + 1];
  uint16_t bitSize = getDeltaListSize(deltaList);
  unsigned int byteCount
    = ((unsigned int) dlsi->bitOffset + bitSize + CHAR_BIT - 1) / CHAR_BIT;
  if (dlsi->byteCount != byteCount) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "unexpected delta list size %u != %u",
                                     dlsi->byteCount, byteCount);
  }

  moveBits(data, dlsi->bitOffset, deltaMemory->memory,
           getDeltaListStart(deltaList), bitSize);
  setZero(deltaMemory->flags, listNumber, 1);
  deltaMemory->numTransfers--;
  return UDS_SUCCESS;
}

/**********************************************************************/
void abortRestoringDeltaMemory(DeltaMemory *deltaMemory)
{
  clearTransferFlags(deltaMemory);
  emptyDeltaLists(deltaMemory);
}

/**********************************************************************/
void startSavingDeltaMemory(DeltaMemory *deltaMemory,
                            BufferedWriter *bufferedWriter)
{
  flagNonEmptyDeltaLists(deltaMemory);
  deltaMemory->bufferedWriter = bufferedWriter;
}

/**********************************************************************/
int finishSavingDeltaMemory(DeltaMemory *deltaMemory)
{
  for (unsigned int i = 0;
       !areDeltaMemoryTransfersDone(deltaMemory)
         && (i < deltaMemory->numLists);
       i++) {
    lazyFlushDeltaList(deltaMemory, i);
  }
  if (deltaMemory->numTransfers > 0) {
    deltaMemory->transferStatus
      = logWarningWithStringError(UDS_CORRUPT_DATA,
                                  "Not all delta lists written");
  }
  deltaMemory->bufferedWriter = NULL;
  return deltaMemory->transferStatus;
}

/**********************************************************************/
void abortSavingDeltaMemory(DeltaMemory *deltaMemory)
{
  clearTransferFlags(deltaMemory);
  deltaMemory->bufferedWriter = NULL;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int writeDeltaListSaveInfo(BufferedWriter *bufferedWriter,
                                  DeltaListSaveInfo *dlsi)
{
  byte buffer[sizeof(DeltaListSaveInfo)];
  buffer[0] = dlsi->tag;
  buffer[1] = dlsi->bitOffset;
  storeUInt16LE(&buffer[2], dlsi->byteCount);
  storeUInt32LE(&buffer[4], dlsi->index);
  return writeToBufferedWriter(bufferedWriter, buffer, sizeof(buffer));
}

/**********************************************************************/
void flushDeltaList(DeltaMemory *deltaMemory, unsigned int flushIndex)
{
  ASSERT_LOG_ONLY((getField(deltaMemory->flags, flushIndex, 1) != 0),
                  "flush bit is set");
  setZero(deltaMemory->flags, flushIndex, 1);
  deltaMemory->numTransfers--;

  DeltaList *deltaList = &deltaMemory->deltaLists[flushIndex + 1];
  DeltaListSaveInfo dlsi;
  dlsi.tag       = deltaMemory->tag;
  dlsi.bitOffset = getDeltaListStart(deltaList) % CHAR_BIT;
  dlsi.byteCount = getDeltaListByteSize(deltaList);
  dlsi.index     = deltaMemory->firstList + flushIndex;

  int result = writeDeltaListSaveInfo(deltaMemory->bufferedWriter, &dlsi);
  if (result != UDS_SUCCESS) {
    if (deltaMemory->transferStatus == UDS_SUCCESS) {
      logWarningWithStringError(result, "failed to write delta list memory");
      deltaMemory->transferStatus = result;
    }
  }
  result = writeToBufferedWriter(deltaMemory->bufferedWriter,
                                 deltaMemory->memory
                                 + getDeltaListByteStart(deltaList),
                                 dlsi.byteCount);
  if (result != UDS_SUCCESS) {
    if (deltaMemory->transferStatus == UDS_SUCCESS) {
      logWarningWithStringError(result, "failed to write delta list memory");
      deltaMemory->transferStatus = result;
    }
  }
}

/**********************************************************************/
int writeGuardDeltaList(BufferedWriter *bufferedWriter)
{
  DeltaListSaveInfo dlsi;
  dlsi.tag       = 'z';
  dlsi.bitOffset = 0;
  dlsi.byteCount = 0;
  dlsi.index     = 0;
  int result = writeToBufferedWriter(bufferedWriter, (const byte *) &dlsi,
                                     sizeof(DeltaListSaveInfo));
  if (result != UDS_SUCCESS) {
    logWarningWithStringError(result, "failed to write guard delta list");
  }
  return result;
}

/**********************************************************************/
int extendDeltaMemory(DeltaMemory *deltaMemory, unsigned int growingIndex,
                      size_t growingSize, bool doCopy)
{
  if (!isMutable(deltaMemory)) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "Attempt to read into an immutable delta"
                                   " list memory");
  }

  AbsTime startTime = currentTime(CT_MONOTONIC);

  // Calculate the amount of space that is in use.  Include the space that
  // has a planned use.
  DeltaList *deltaLists = deltaMemory->deltaLists;
  size_t usedSpace = growingSize;
  for (unsigned int i = 0; i <= deltaMemory->numLists + 1; i++) {
    usedSpace += getDeltaListByteSize(&deltaLists[i]);
  }

  if (deltaMemory->size < usedSpace) {
    return UDS_OVERFLOW;
  }

  // Compute the new offsets of the delta lists
  size_t spacing = (deltaMemory->size - usedSpace) / deltaMemory->numLists;
  deltaMemory->tempOffsets[0] = 0;
  for (unsigned int i = 0; i <= deltaMemory->numLists; i++) {
    deltaMemory->tempOffsets[i + 1] = (deltaMemory->tempOffsets[i]
                                       + getDeltaListByteSize(&deltaLists[i])
                                       + spacing);
    deltaMemory->tempOffsets[i] *= CHAR_BIT;
    deltaMemory->tempOffsets[i]
      += getDeltaListStart(&deltaLists[i]) % CHAR_BIT;
    if (i == 0) {
      deltaMemory->tempOffsets[i + 1] -= spacing / 2;
    }
    if (i + 1 == growingIndex) {
      deltaMemory->tempOffsets[i + 1] += growingSize;
    }
  }
  deltaMemory->tempOffsets[deltaMemory->numLists + 1]
    = (deltaMemory->size * CHAR_BIT
       - getDeltaListSize(&deltaLists[deltaMemory->numLists + 1]));
  // When we rebalance the delta list, we will include the end guard list
  // in the rebalancing.  It contains the end guard data, which must be
  // copied.
  if (doCopy) {
    rebalanceDeltaMemory(deltaMemory, 1, deltaMemory->numLists + 1);
    AbsTime endTime = currentTime(CT_MONOTONIC);
    deltaMemory->rebalanceCount++;
    deltaMemory->rebalanceTime += timeDifference(endTime, startTime);
  } else {
    for (unsigned int i = 1; i <= deltaMemory->numLists + 1; i++) {
      deltaLists[i].startOffset = deltaMemory->tempOffsets[i];
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int validateDeltaLists(const DeltaMemory *deltaMemory)
{
  // Validate the delta index fields set by restoring a delta index
  if (deltaMemory->collisionCount > deltaMemory->recordCount) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "delta index contains more collisions"
                                     " (%ld) than records (%ld)",
                                     deltaMemory->collisionCount,
                                     deltaMemory->recordCount);
  }

  // Validate the delta lists
  DeltaList *deltaLists = deltaMemory->deltaLists;
  if (getDeltaListStart(&deltaLists[0]) != 0) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "the head guard delta list does not start"
                                     " at 0: %" PRIu64,
                                     getDeltaListStart(&deltaLists[0]));
  }
  uint64_t numBits = getDeltaListEnd(&deltaLists[deltaMemory->numLists + 1]);
  if (numBits != deltaMemory->size * CHAR_BIT) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "the tail guard delta list does not end "
                                     "at end of allocated memory:  %" PRIu64
                                     " != %zd",
                                     numBits, deltaMemory->size * CHAR_BIT);
  }
  int numGuardBits = getDeltaListSize(&deltaLists[deltaMemory->numLists + 1]);
  if (numGuardBits < GUARD_BITS) {
    return logWarningWithStringError(UDS_BAD_STATE,
                                     "the tail guard delta list does not "
                                     "contain sufficient guard bits:  %d < %d",
                                     numGuardBits, GUARD_BITS);
  }
  for (unsigned int i = 0; i <= deltaMemory->numLists + 1; i++) {
    if (getDeltaListStart(&deltaLists[i]) > getDeltaListEnd(&deltaLists[i])) {
      return logWarningWithStringError(UDS_BAD_STATE,
                                       "invalid delta list %u: [%" PRIu64
                                       ", %" PRIu64 ")",
                                       i,
                                       getDeltaListStart(&deltaLists[i]),
                                       getDeltaListEnd(&deltaLists[i]));
    }
    if (i > deltaMemory->numLists) {
      // The rest of the checks do not apply to the tail guard list
      continue;
    }
    if (getDeltaListEnd(&deltaLists[i])
        > getDeltaListStart(&deltaLists[i + 1])) {
      return logWarningWithStringError(UDS_BAD_STATE,
                                       "delta lists %u and %u overlap:  %"
                                       PRIu64 " > %" PRIu64,
                                       i, i + 1,
                                       getDeltaListEnd(&deltaLists[i]),
                                       getDeltaListStart(&deltaLists[i + 1]));
    }
    if (i == 0) {
      // The rest of the checks do not apply to the head guard list
      continue;
    }
    if (deltaLists[i].saveOffset > getDeltaListSize(&deltaLists[i])) {
      return logWarningWithStringError(UDS_BAD_STATE,
                                       "delta lists %u saved offset is larger"
                                       " than the list:  %u > %u",
                                       i, deltaLists[i].saveOffset,
                                       getDeltaListSize(&deltaLists[i]));
    }
  }

  return UDS_SUCCESS;
}

/**********************************************************************/
size_t getDeltaMemoryAllocated(const DeltaMemory *deltaMemory)
{
  return (deltaMemory->size
          + getSizeOfDeltaLists(deltaMemory->numLists)
          + getSizeOfFlags(deltaMemory->numLists)
          + getSizeOfTempOffsets(deltaMemory->numLists));
}

/**********************************************************************/
size_t getDeltaMemorySize(unsigned long numEntries, unsigned int meanDelta,
                          unsigned int numPayloadBits)
{
  unsigned short minBits;
  unsigned int incrKeys, minKeys;
  computeCodingConstants(meanDelta, &minBits, &minKeys, &incrKeys);
  // On average, each delta is encoded into about minBits+1.5 bits.
  return (numEntries * (numPayloadBits + minBits + 1) + numEntries / 2);
}
