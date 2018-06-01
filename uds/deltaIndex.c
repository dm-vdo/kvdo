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
 * $Id: //eng/uds-releases/gloria/src/uds/deltaIndex.c#3 $
 */
#include "deltaIndex.h"

#include "bits.h"
#include "buffer.h"
#include "compiler.h"
#include "cpu.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"
#include "zone.h"

/*
 * A delta index is a key-value store, where each entry maps an address
 * (the key) to a payload (the value).  The entries are sorted by address,
 * and only the delta between successive addresses is stored in the entry.
 * The addresses are assumed to be uniformly distributed,and the deltas are
 * therefore exponentially distributed.
 *
 * The entries could be stored in a single DeltaList, but for efficiency we
 * use multiple DeltaLists.  These lists are stored in a single chunk of
 * memory managed by the DeltaMemory module.  The DeltaMemory module can
 * move the data around in memory, so we never keep any byte pointers into
 * DeltaList memory.  We only keep offsets into the memory.
 *
 * The delta lists are stored as bit streams.  These bit streams are stored
 * in little endian order, and all offsets into DeltaMemory are bit
 * offsets.
 *
 * All entries are stored as a fixed length payload (the value) followed by a
 * variable length key (the delta). Always strictly in little endian order.
 *
 * A collision entry is used when two block names have the same delta list
 * address.  A collision entry is encoded with DELTA==0, and has 256
 * extension bits containing the full block name.
 *
 * There is a special exception to be noted.  The DELTA==0 encoding usually
 * indicates a collision with the preceding entry.  But for the first entry
 * in any delta list there is no preceding entry, so the DELTA==0 encoding
 * at the beginning of a delta list indicates a normal entry.
 *
 * The Huffman code is driven by 3 parameters:
 *
 *  MINBITS   This is the number of bits in the smallest code
 *
 *  BASE      This is the number of values coded using a code of length MINBITS
 *
 *  INCR      This is the number of values coded by using one additional bit.
 *
 * These parameters are related by:
 *
 *       BASE + INCR == 1 << MINBITS
 *
 * When we create an index, we need to know the mean delta.  From the mean
 * delta, we compute these three parameters.  The math for the Huffman code
 * of an exponential distribution says that we compute:
 *
 *      INCR = log(2) * MEAN_DELTA
 *
 * Then we find the smallest MINBITS so that
 *
 *      1 << MINBITS  >  INCR
 *
 * And then:
 *
 *       BASE = (1 << MINBITS) - INCR
 *
 * Now we need a code such that
 *
 * - The first BASE values code using MINBITS bits
 * - The next INCR values code using MINBITS+1 bits.
 * - The next INCR values code using MINBITS+2 bits.
 * - The next INCR values code using MINBITS+3 bits.
 * - (and so on).
 *
 * ENCODE(DELTA):
 *
 *   if (DELTA < BASE) {
 *       put DELTA in MINBITS bits;
 *   } else {
 *       T1 = (DELTA - BASE) % INCR + BASE;
 *       T2 = (DELTA - BASE) / INCR;
 *       put T1 in MINBITS bits;
 *       put 0 in T2 bits;
 *       put 1 in 1 bit;
 *   }
 *
 * DECODE(BIT_STREAM):
 *
 *   T1 = next MINBITS bits of stream;
 *   if (T1 < BASE) {
 *       DELTA = T1;
 *   } else {
 *       Scan bits in the stream until reading a 1,
 *         setting T2 to the number of 0 bits read;
 *       DELTA = T2 * INCR + T1;
 *   }
 *
 * The bit field utilities that we use on the delta lists assume that it is
 * possible to read a few bytes beyond the end of the bit field.  So we
 * make sure to allocates some extra bytes at the end of memory containing
 * the delta lists.  Look for POST_FIELD_GUARD_BYTES to find the code
 * related to this.
 *
 * And note that the decode bit stream code includes a step that skips over
 * 0 bits until the first 1 bit is found.  A corrupted delta list could
 * cause this step to run off the end of the delta list memory.  As an
 * extra protection against this happening, the guard bytes at the end
 * should be set to all ones.
 */

/**
 * Constants and structures for the saved delta index. "DI" is for
 * deltaIndex, and -##### is a number to increment when the format of the
 * data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_DI_START[] = "DI-00002";

struct di_header {
  char     magic[MAGIC_SIZE];   // MAGIC_DI_START
  uint32_t zoneNumber;
  uint32_t numZones;
  uint32_t firstList;
  uint32_t numLists;
  uint64_t recordCount;
  uint64_t collisionCount;
};

//**********************************************************************
//  Methods for dealing with mutable delta list headers
//**********************************************************************

/**
 * Move the start of the delta list bit stream without moving the end.
 *
 * @param deltaList  The delta list header
 * @param increment  The change in the start of the delta list
 **/
static INLINE void moveDeltaListStart(DeltaList *deltaList, int increment)
{
  deltaList->startOffset += increment;
  deltaList->size        -= increment;
}

/**
 * Move the end of the delta list bit stream without moving the start.
 *
 * @param deltaList  The delta list header
 * @param increment  The change in the end of the delta list
 **/
static INLINE void moveDeltaListEnd(DeltaList *deltaList, int increment)
{
  deltaList->size += increment;
}

//**********************************************************************
//  Methods for dealing with immutable delta list headers packed
//**********************************************************************

// Header data used for immutable delta index pages.  These data are
// followed by the delta list offset table.
typedef struct __attribute__((packed)) deltaPageHeader {
  uint64_t nonce;                 // Externally-defined nonce
  uint64_t virtualChapterNumber;  // The virtual chapter number
  uint16_t firstList;             // Index of the first delta list on the page
  uint16_t numLists;              // Number of delta lists on the page
} DeltaPageHeader;

// Immutable delta lists are packed into pages containing a header that
// encodes the delta list information into 19 bits per list (64KB bit offset)

enum { IMMUTABLE_HEADER_SIZE = 19 };

/**
 * Get the bit offset to the immutable delta list header
 *
 * @param listNumber  The delta list number
 *
 * @return the offset of immutable delta list header
 **/
static INLINE unsigned int getImmutableHeaderOffset(unsigned int listNumber)
{
  return (sizeof(DeltaPageHeader) * CHAR_BIT
          + listNumber * IMMUTABLE_HEADER_SIZE);
}

/**
 * Get the bit offset to the start of the immutable delta list bit stream
 *
 * @param memory      The memory page containing the delta lists
 * @param listNumber  The delta list number
 *
 * @return the start of the delta list
 **/
static INLINE unsigned int getImmutableStart(const byte *memory,
                                             unsigned int listNumber)
{
  return getField(memory, getImmutableHeaderOffset(listNumber),
                  IMMUTABLE_HEADER_SIZE);
}

/**
 * Set the bit offset to the start of the immutable delta list bit stream
 *
 * @param memory       The memory page containing the delta lists
 * @param listNumber   The delta list number
 * @param startOffset  The start of the delta list
 **/
static INLINE void setImmutableStart(byte *memory, unsigned int listNumber,
                                     unsigned int startOffset)
{
  setField(startOffset, memory, getImmutableHeaderOffset(listNumber),
           IMMUTABLE_HEADER_SIZE);
}

//**********************************************************************
//  Methods for dealing with Delta List Entries
//**********************************************************************

/**
 * Decode a delta index entry delta value. The DeltaIndexEntry basically
 * describes the previous list entry, and has had its offset field changed to
 * point to the subsequent entry. We decode the bit stream and update the
 * DeltaListEntry to describe the entry.
 *
 * @param deltaEntry  The delta index entry
 **/
static INLINE void decodeDelta(DeltaIndexEntry *deltaEntry)
{
  const DeltaMemory *deltaZone = deltaEntry->deltaZone;
  const byte *memory = deltaZone->memory;
  uint64_t deltaOffset
    = getDeltaEntryOffset(deltaEntry) + deltaEntry->valueBits;
  const byte *addr = memory + deltaOffset / CHAR_BIT;
  int offset = deltaOffset % CHAR_BIT;
  uint32_t data = getUInt32LE(addr) >> offset;
  addr += sizeof(uint32_t);
  int keyBits = deltaZone->minBits;
  unsigned int delta = data & ((1 << keyBits) - 1);
  if (delta >= deltaZone->minKeys) {
    data >>= keyBits;
    if (data == 0) {
      keyBits = sizeof(uint32_t) * CHAR_BIT - offset;
      while ((data = getUInt32LE(addr)) == 0) {
        addr += sizeof(uint32_t);
        keyBits += sizeof(uint32_t) * CHAR_BIT;
      }
    }
    keyBits += ffs(data);
    delta += (keyBits - deltaZone->minBits - 1) * deltaZone->incrKeys;
  }
  deltaEntry->delta = delta;
  deltaEntry->key += delta;

  // Check for a collision, a delta of zero not at the start of the list.
  if (unlikely((delta == 0) && (deltaEntry->offset > 0))) {
    deltaEntry->isCollision = true;
    // The small duplication of this math in the two arms of this if statement
    // makes a tiny but measurable difference in performance.
    deltaEntry->entryBits = deltaEntry->valueBits + keyBits + COLLISION_BITS;
  } else {
    deltaEntry->isCollision = false;
    deltaEntry->entryBits = deltaEntry->valueBits + keyBits;
  }
}

/**
 * Delete bits from a delta list at the offset of the specified delta index
 * entry.
 *
 * @param deltaEntry  The delta index entry
 * @param size        The number of bits to delete
 **/
static void deleteBits(const DeltaIndexEntry *deltaEntry, int size)
{
  DeltaList *deltaList = deltaEntry->deltaList;
  byte *memory = deltaEntry->deltaZone->memory;
  // Compute how many bits are retained before and after the deleted bits
  uint32_t totalSize = getDeltaListSize(deltaList);
  uint32_t beforeSize = deltaEntry->offset;
  uint32_t afterSize = totalSize - deltaEntry->offset - size;

  // Determine whether to add to the available space either before or after
  // the delta list.  We prefer to move the least amount of data.  If it is
  // exactly the same, try to add to the smaller amount of free space.
  bool beforeFlag;
  if (beforeSize < afterSize) {
    beforeFlag = true;
  } else if (afterSize < beforeSize) {
    beforeFlag = false;
  } else {
    uint64_t freeBefore
      = getDeltaListStart(&deltaList[0]) - getDeltaListEnd(&deltaList[-1]);
    uint64_t freeAfter
      = getDeltaListStart(&deltaList[1]) - getDeltaListEnd(&deltaList[ 0]);
    beforeFlag = freeBefore < freeAfter;
  }

  uint64_t source, destination;
  uint32_t count;
  if (beforeFlag) {
    source = getDeltaListStart(deltaList);
    destination = source + size;
    moveDeltaListStart(deltaList, size);
    count = beforeSize;
  } else {
    moveDeltaListEnd(deltaList, -size);
    destination = getDeltaListStart(deltaList) + deltaEntry->offset;
    source = destination + size;
    count = afterSize;
  }
  moveBits(memory, source, memory, destination, count);
}

/**
 * Get the offset of the collision field in a DeltaIndexEntry
 *
 * @param entry  The delta index record
 *
 * @return the offset of the start of the collision name
 **/
static INLINE uint64_t getCollisionOffset(const DeltaIndexEntry *entry)
{
  return (getDeltaEntryOffset(entry) + entry->entryBits - COLLISION_BITS);
}

/**
 * Encode a delta index entry delta.
 *
 * @param deltaEntry  The delta index entry
 **/
static void encodeDelta(const DeltaIndexEntry *deltaEntry)
{
  const DeltaMemory *deltaZone = deltaEntry->deltaZone;
  byte *memory = deltaZone->memory;
  uint64_t offset = getDeltaEntryOffset(deltaEntry) + deltaEntry->valueBits;
  if (deltaEntry->delta < deltaZone->minKeys) {
    setField(deltaEntry->delta, memory, offset, deltaZone->minBits);
    return;
  }
  unsigned int temp = deltaEntry->delta - deltaZone->minKeys;
  unsigned int t1   = (temp % deltaZone->incrKeys) + deltaZone->minKeys;
  unsigned int t2   = temp / deltaZone->incrKeys;
  setField(t1, memory, offset, deltaZone->minBits);
  setZero(memory, offset + deltaZone->minBits, t2);
  setOne(memory, offset + deltaZone->minBits + t2, 1);
}

/**
 * Encode a delta index entry.
 *
 * @param deltaEntry  The delta index entry
 * @param value       The value associated with the entry
 * @param name        For collision entries, the 256 bit full name.
 **/
static void encodeEntry(const DeltaIndexEntry *deltaEntry, unsigned int value,
                        const byte *name)
{
  byte *memory = deltaEntry->deltaZone->memory;
  uint64_t offset = getDeltaEntryOffset(deltaEntry);
  setField(value, memory, offset, deltaEntry->valueBits);
  encodeDelta(deltaEntry);
  if (name != NULL) {
    setBytes(memory, getCollisionOffset(deltaEntry), name, COLLISION_BYTES);
  }
}

/**
 * Insert bits into a delta list at the offset of the specified delta index
 * entry.
 *
 * @param deltaEntry  The delta index entry
 * @param size        The number of bits to insert
 *
 * @return UDS_SUCCESS or an error code
 **/
static int insertBits(DeltaIndexEntry *deltaEntry, int size)
{
  DeltaMemory *deltaZone = deltaEntry->deltaZone;
  DeltaList  *deltaList  = deltaEntry->deltaList;
  // Compute how many bits are in use before and after the inserted bits
  uint32_t totalSize = getDeltaListSize(deltaList);
  uint32_t beforeSize = deltaEntry->offset;
  uint32_t afterSize = totalSize - deltaEntry->offset;
  if ((unsigned int) (totalSize + size) > UINT16_MAX) {
    deltaEntry->listOverflow = true;
    deltaZone->overflowCount++;
    return UDS_OVERFLOW;
  }

  // Compute how many bits are available before and after the delta list
  uint64_t freeBefore
    = getDeltaListStart(&deltaList[0]) - getDeltaListEnd(&deltaList[-1]);
  uint64_t freeAfter
    = getDeltaListStart(&deltaList[1]) - getDeltaListEnd(&deltaList[ 0]);

  bool beforeFlag;
  if (((unsigned int) size <= freeBefore)
      && ((unsigned int) size <= freeAfter)) {
    // We have enough space to use either before or after the list.  Prefer
    // to move the least amount of data.  If it is exactly the same, try to
    // take from the larger amount of free space.
    if (beforeSize < afterSize) {
      beforeFlag = true;
    } else if (afterSize < beforeSize) {
      beforeFlag = false;
    } else {
      beforeFlag = freeBefore > freeAfter;
    }
  } else if ((unsigned int) size <= freeBefore) {
    // There is space before but not after
    beforeFlag = true;
  } else if ((unsigned int) size <= freeAfter) {
    // There is space after but not before
    beforeFlag = false;
  } else {
    // Neither of the surrounding spaces is large enough for this request,
    // Extend and/or rebalance the delta list memory choosing to move the
    // least amount of data.
    unsigned int growingIndex = deltaEntry->listNumber + 1;
    beforeFlag = beforeSize < afterSize;
    if (!beforeFlag) {
      growingIndex++;
    }
    int result = extendDeltaMemory(deltaZone, growingIndex,
                                   (size + CHAR_BIT - 1) / CHAR_BIT, true);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  uint64_t source, destination;
  uint32_t count;
  if (beforeFlag) {
    source = getDeltaListStart(deltaList);
    destination = source -  size;
    moveDeltaListStart(deltaList, -size);
    count = beforeSize;
  } else {
    moveDeltaListEnd(deltaList, size);
    source = getDeltaListStart(deltaList) + deltaEntry->offset;
    destination = source + size;
    count = afterSize;
  }
  byte *memory = deltaZone->memory;
  moveBits(memory, source, memory, destination, count);
  return UDS_SUCCESS;
}

/**
 * Get the amount of memory to allocate for each zone
 *
 * @param numZones    The number of zones in the index
 * @param memorySize  The number of bytes in memory for the index
 *
 * @return the number of bytes to allocate for a single zone
 **/
static INLINE size_t getZoneMemorySize(unsigned int numZones,
                                       size_t memorySize)
{
  size_t zoneSize = memorySize / numZones;
  // Round the size up so that each zone is a multiple of 64K in size.
  enum { ALLOC_BOUNDARY = 64 * KILOBYTE };
  return (zoneSize + ALLOC_BOUNDARY - 1) & -ALLOC_BOUNDARY;
}

/**
 * Validate delta index parameters
 *
 * @param meanDelta       The mean delta value
 * @param numPayloadBits  The number of bits in the payload or value
 **/
static bool invalidParameters(unsigned int meanDelta,
                              unsigned int numPayloadBits)
{
  const unsigned int minDelta = 10;
  const unsigned int maxDelta = 1 << MAX_FIELD_BITS;
  if ((meanDelta < minDelta) || (meanDelta > maxDelta)) {
    logWarning("error initializing delta index: "
               "meanDelta (%u) is not in the range %u to %u",
               meanDelta, minDelta, maxDelta);
    return true;
  }
  if (numPayloadBits > MAX_FIELD_BITS) {
    logWarning("error initializing delta index: Too many payload bits (%u)",
               numPayloadBits);
    return true;
  }
  return false;
}

/**
 * Set a delta index entry to be a collision
 *
 * @param deltaEntry  The delta index entry
 **/
static void setCollision(DeltaIndexEntry *deltaEntry)
{
  deltaEntry->isCollision = true;
  deltaEntry->entryBits += COLLISION_BITS;
}

/**
 * Set the delta in a delta index entry.
 *
 * @param deltaEntry  The delta index entry
 * @param delta       The new delta
 **/
static void setDelta(DeltaIndexEntry *deltaEntry, unsigned int delta)
{
  const DeltaMemory *deltaZone = deltaEntry->deltaZone;
  deltaEntry->delta = delta;
  int keyBits = (deltaZone->minBits
                 + ((deltaZone->incrKeys - deltaZone->minKeys + delta)
                    / deltaZone->incrKeys));
  deltaEntry->entryBits = deltaEntry->valueBits + keyBits;
}

//**********************************************************************
//  External functions declared in deltaIndex.h
//**********************************************************************

int initializeDeltaIndex(DeltaIndex *deltaIndex, unsigned int numZones,
                         unsigned int numLists, unsigned int meanDelta,
                         unsigned int numPayloadBits, size_t memorySize)
{
  size_t memSize = getZoneMemorySize(numZones, memorySize);
  if (invalidParameters(meanDelta, numPayloadBits)) {
    return UDS_INVALID_ARGUMENT;
  }

  int result = ALLOCATE(numZones, DeltaMemory, "Delta Index Zones",
                        &deltaIndex->deltaZones);
  if (result != UDS_SUCCESS) {
    return result;
  }

  deltaIndex->numZones     = numZones;
  deltaIndex->numLists     = numLists;
  deltaIndex->listsPerZone = (numLists + numZones - 1) / numZones;
  deltaIndex->isMutable    = true;
  deltaIndex->tag          = 'm';

  for (unsigned int z = 0; z < numZones; z++) {
    unsigned int firstListInZone = z * deltaIndex->listsPerZone;
    unsigned int numListsInZone = deltaIndex->listsPerZone;
    if (z == numZones - 1) {
      /*
       * The last zone gets fewer lists if numZones doesn't evenly divide
       * numLists. We'll have an underflow if the assertion below doesn't
       * hold. (And it turns out that the assertion is equivalent to
       *   numZones <= 1 + (numLists / numZones) + (numLists % numZones)
       * in the case that numZones doesn't evenly divide numlists.
       * If numLists >= numZones * numZones, then the above inequality
       * will always hold.)
       */
      if (deltaIndex->numLists <= firstListInZone) {
        uninitializeDeltaIndex(deltaIndex);
        return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                       "%u delta-lists not enough for %u zones",
                                       numLists, numZones);
      }
      numListsInZone = deltaIndex->numLists - firstListInZone;
    }
    int result = initializeDeltaMemory(&deltaIndex->deltaZones[z], memSize,
                                       firstListInZone, numListsInZone,
                                       meanDelta, numPayloadBits);
    if (result != UDS_SUCCESS) {
      uninitializeDeltaIndex(deltaIndex);
      return result;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int initializeDeltaIndexPage(DeltaIndex *deltaIndex, DeltaMemory *deltaMemory,
                             uint64_t expectedNonce, unsigned int meanDelta,
                             unsigned int numPayloadBits, byte *memory,
                             size_t memSize)
{
  const DeltaPageHeader *header = (const DeltaPageHeader *) memory;
  unsigned int numLists = header->numLists;

  if ((expectedNonce != 0) && (header->nonce != expectedNonce)) {
    // Do not log this as an error.  It happens in normal operation when we
    // are doing a rebuild but haven't written the entire volume once.
    return UDS_CORRUPT_COMPONENT;
  }

  // Verify that the memory page is valid
  if (numLists >
      (memSize - sizeof(DeltaPageHeader)) * CHAR_BIT / IMMUTABLE_HEADER_SIZE) {
    return logWarningWithStringError(
      UDS_CORRUPT_COMPONENT, "error initializing delta index page: "
      "%u lists will not fit on a %zu byte page", numLists, memSize);
  }
  if (getImmutableStart(memory, 0) != getImmutableHeaderOffset(numLists + 1)) {
    return logWarningWithStringError(
      UDS_CORRUPT_COMPONENT, "error initializing delta index page: "
      "the first list is at offset %u, but should be at %u",
      getImmutableStart(memory, 0),
      getImmutableHeaderOffset(numLists + 1));
  }

  for (unsigned int i = 0; i < numLists; i++) {
    if (getImmutableStart(memory, i) > getImmutableStart(memory, i + 1)) {
      return logWarningWithStringError(
        UDS_CORRUPT_COMPONENT, "error initializing delta index page: "
        "list %u at offset %u is after list %u at %u",
        i,     getImmutableStart(memory, i),
        i + 1, getImmutableStart(memory, i + 1));
    }
  }
  if (getImmutableStart(memory, numLists) > memSize * CHAR_BIT) {
    return logWarningWithStringError(
      UDS_CORRUPT_COMPONENT,
      "error initializing delta index page: The last list ends at "
      "%u, which is after the end of the %zu byte page",
      getImmutableStart(memory, numLists), memSize);
  }
  if (getImmutableStart(memory, numLists)
      > (memSize - POST_FIELD_GUARD_BYTES) * CHAR_BIT) {
    return logWarningWithStringError(
      UDS_CORRUPT_COMPONENT,
      "error initializing delta index page: The last list ends at "
      "%u, which does not allow the %u guard bits at the end of "
      "the %zu byte page",
      getImmutableStart(memory, numLists),
      POST_FIELD_GUARD_BYTES * CHAR_BIT, memSize);
  }
  for (int i = 0; i < POST_FIELD_GUARD_BYTES; i++) {
    byte guardByte = memory[memSize - POST_FIELD_GUARD_BYTES + i];
    if (guardByte != (byte) ~0) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "guard byte %d has invalid value 0x%X",
                                       i, guardByte);
    }
  }

  if (invalidParameters(meanDelta, numPayloadBits)) {
    return UDS_INVALID_ARGUMENT;
  }

  deltaIndex->deltaZones   = deltaMemory;
  deltaIndex->numZones     = 1;
  deltaIndex->numLists     = numLists;
  deltaIndex->listsPerZone = numLists;
  deltaIndex->isMutable    = false;
  deltaIndex->tag          = 'p';

  initializeDeltaMemoryPage(deltaMemory, (byte *) memory, memSize, numLists,
                            meanDelta, numPayloadBits);
  return UDS_SUCCESS;
}

/**********************************************************************/
void uninitializeDeltaIndex(DeltaIndex *deltaIndex)
{
  if (deltaIndex != NULL) {
    for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
      uninitializeDeltaMemory(&deltaIndex->deltaZones[z]);
    }
    FREE(deltaIndex->deltaZones);
    memset(deltaIndex, 0, sizeof(DeltaIndex));
  }
}

/**********************************************************************/
void emptyDeltaIndex(const DeltaIndex *deltaIndex)
{
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    emptyDeltaLists(&deltaIndex->deltaZones[z]);
  }
}

/**********************************************************************/
void emptyDeltaIndexZone(const DeltaIndex *deltaIndex, unsigned int zoneNumber)
{
  emptyDeltaLists(&deltaIndex->deltaZones[zoneNumber]);
}

/**********************************************************************/
int packDeltaIndexPage(const DeltaIndex *deltaIndex, uint64_t headerNonce,
                       byte *memory, size_t memSize,
                       uint64_t virtualChapterNumber, unsigned int firstList,
                       unsigned int *numLists)
{
  if (!deltaIndex->isMutable) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "Cannot pack an immutable index");
  }
  if (deltaIndex->numZones != 1) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "Cannot pack a delta index page when the"
                                   " index has %u zones",
                                   deltaIndex->numZones);
  }
  if (firstList > deltaIndex->numLists) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "Cannot pack a delta index page when the"
                                   " first list (%u) is larger than the number"
                                   " of lists (%u)",
                                   firstList, deltaIndex->numLists);
  }

  const DeltaMemory *deltaZone = &deltaIndex->deltaZones[0];
  DeltaList *deltaLists = &deltaZone->deltaLists[firstList + 1];
  unsigned int maxLists = deltaIndex->numLists - firstList;

  // Compute how many lists will fit on the page
  int numBits = memSize * CHAR_BIT;
  // Subtract the size of the fixed header and 1 delta list offset
  numBits -= getImmutableHeaderOffset(1);
  // Subtract the guard bytes of memory so that allow us to freely read a
  // short distance past the end of any byte we are interested in.
  numBits -= POST_FIELD_GUARD_BYTES * CHAR_BIT;
  if (numBits < IMMUTABLE_HEADER_SIZE) {
    // This page is too small to contain even one empty delta list
    return logErrorWithStringError(UDS_OVERFLOW,
                                   "Chapter Index Page of %zu bytes is too"
                                   " small",
                                   memSize);
  }

  unsigned int nLists = 0;
  while (nLists < maxLists) {
    // Each list requires 1 delta list offset and the list data
    int bits = IMMUTABLE_HEADER_SIZE + getDeltaListSize(&deltaLists[nLists]);
    if (bits > numBits) {
      break;
    }
    nLists++;
    numBits -= bits;
  }
  *numLists = nLists;

  // Construct the page header
  DeltaPageHeader *header = (DeltaPageHeader *) memory;
  header->nonce                = headerNonce;
  header->virtualChapterNumber = virtualChapterNumber;
  header->firstList            = firstList;
  header->numLists             = nLists;

  // Construct the delta list offset table, making sure that the memory
  // page is large enough.
  unsigned int offset = getImmutableHeaderOffset(nLists + 1);
  setImmutableStart(memory, 0, offset);
  for (unsigned int i = 0; i < nLists; i++) {
    offset += getDeltaListSize(&deltaLists[i]);
    setImmutableStart(memory, i + 1, offset);
  }

  // Copy the delta list data onto the memory page
  for (unsigned int i = 0; i < nLists; i++) {
    DeltaList *deltaList = &deltaLists[i];
    moveBits(deltaZone->memory, getDeltaListStart(deltaList), memory,
             getImmutableStart(memory, i), getDeltaListSize(deltaList));
  }

  // Set all the bits in the guard bytes.  Do not use the bit field
  // utilities.
  memset(memory + memSize - POST_FIELD_GUARD_BYTES, ~0,
         POST_FIELD_GUARD_BYTES);
  return UDS_SUCCESS;
}

/**********************************************************************/
void setDeltaIndexTag(DeltaIndex *deltaIndex, byte tag)
{
  deltaIndex->tag = tag;
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    deltaIndex->deltaZones[z].tag = tag;
  }
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeDeltaIndexHeader(Buffer *buffer, struct di_header *header)
{
  int result = getBytesFromBuffer(buffer, MAGIC_SIZE, &header->magic);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &header->zoneNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt32LEFromBuffer(buffer, &header->numZones);
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
  result = getUInt64LEFromBuffer(buffer, &header->recordCount);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = getUInt64LEFromBuffer(buffer, &header->collisionCount);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           bufferLength(buffer) - contentLength(buffer),
                           bufferLength(buffer));
  return result;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int readDeltaIndexHeader(BufferedReader *reader,
                                struct di_header *header)
{
  Buffer *buffer;
  
  int result = makeBuffer(sizeof(*header), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = readFromBufferedReader(reader, getBufferContents(buffer),
                                  bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return logWarningWithStringError(result,
                                     "failed to read delta index header");
  }
  result = resetBufferEnd(buffer, bufferLength(buffer));
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = decodeDeltaIndexHeader(buffer, header);
  freeBuffer(&buffer);
  return result;
}

/**********************************************************************/
int startRestoringDeltaIndex(const DeltaIndex *deltaIndex,
                             BufferedReader **bufferedReaders, int numReaders)
{
  if (!deltaIndex->isMutable) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "Cannot restore to an immutable index");
  }
  if (numReaders <= 0) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "No delta index files");
  }

  unsigned long recordCount = 0;
  unsigned long collisionCount = 0;
  unsigned int numZones = numReaders;
  unsigned int firstList[numZones], numLists[numZones];
  BufferedReader *reader[numZones];
  bool zoneFlags[numZones];
  memset(zoneFlags, false, numZones);

  // Read the header from each file, and make sure we have a matching set
  for (unsigned int z = 0; z < numZones; z++) {
    struct di_header header;
    int result = readDeltaIndexHeader(bufferedReaders[z], &header);
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to read delta index header");
    }
    if (memcmp(header.magic, MAGIC_DI_START, MAGIC_SIZE) != 0) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "delta index file has bad magic"
                                       " number");
    }
    if (numZones != header.numZones) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "delta index files contain mismatched"
                                       " zone counts (%u,%u)",
                                       numZones, header.numZones);
    }
    if (header.zoneNumber >= numZones) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "delta index files contains zone %u of"
                                       " %u zones",
                                       header.zoneNumber, numZones);
    }
    if (zoneFlags[header.zoneNumber]) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "delta index files contain two of zone"
                                       " %u",
                                       header.zoneNumber);
    }
    reader[header.zoneNumber] = bufferedReaders[z];
    firstList[header.zoneNumber] = header.firstList;
    numLists[header.zoneNumber] = header.numLists;
    zoneFlags[header.zoneNumber] = true;
    recordCount += header.recordCount;
    collisionCount += header.collisionCount;
  }
  unsigned int listNext = 0;
  for (unsigned int z = 0; z < numZones; z++) {
    if (firstList[z] != listNext) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "delta index file for zone %u starts"
                                       " with list %u instead of list %u",
                                       z, firstList[z], listNext);
    }
    listNext += numLists[z];
  }
  if (listNext != deltaIndex->numLists) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "delta index files contain %u delta lists"
                                     " instead of %u delta lists",
                                     listNext, deltaIndex->numLists);
  }
  if (collisionCount > recordCount) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "delta index files contain %ld collisions"
                                     " and %ld records",
                                     collisionCount, recordCount);
  }

  emptyDeltaIndex(deltaIndex);
  deltaIndex->deltaZones[0].recordCount    = recordCount;
  deltaIndex->deltaZones[0].collisionCount = collisionCount;

  // Read the delta list sizes from the files, and distribute each of them
  // to proper zone
  for (unsigned int z = 0; z < numZones; z++) {
    for (unsigned int i = 0; i < numLists[z]; i++) {
      byte deltaListSizeData[sizeof(uint16_t)];
      int result = readFromBufferedReader(reader[z], deltaListSizeData,
                                          sizeof(deltaListSizeData));
      if (result != UDS_SUCCESS) {
        return logWarningWithStringError(result,
                                         "failed to read delta index size");
      }
      uint16_t deltaListSize = getUInt16LE(deltaListSizeData);
      unsigned int listNumber = firstList[z] + i;
      unsigned int zoneNumber = getDeltaIndexZone(deltaIndex, listNumber);
      const DeltaMemory *deltaZone = &deltaIndex->deltaZones[zoneNumber];
      listNumber -= deltaZone->firstList;
      deltaZone->deltaLists[listNumber + 1].size = deltaListSize;
    }
  }

  // Prepare each zone to start receiving the delta list data
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    int result = startRestoringDeltaMemory(&deltaIndex->deltaZones[z]);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
bool isRestoringDeltaIndexDone(const DeltaIndex *deltaIndex)
{
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    if (!areDeltaMemoryTransfersDone(&deltaIndex->deltaZones[z])) {
      return false;
    }
  }
  return true;
}

/**********************************************************************/
int restoreDeltaListToDeltaIndex(const DeltaIndex *deltaIndex,
                                 const DeltaListSaveInfo *dlsi,
                                 const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
  // Make sure the data are intended for this delta list.  Do not
  // log an error, as this may be valid data for another delta index.
  if (dlsi->tag != deltaIndex->tag) {
    return UDS_CORRUPT_COMPONENT;
  }

  if (dlsi->index >= deltaIndex->numLists) {
    return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                     "invalid delta list number %u of %u",
                                     dlsi->index, deltaIndex->numLists);
  }

  unsigned int zoneNumber = getDeltaIndexZone(deltaIndex, dlsi->index);
  return restoreDeltaList(&deltaIndex->deltaZones[zoneNumber], dlsi, data);
}

/**********************************************************************/
void abortRestoringDeltaIndex(const DeltaIndex *deltaIndex)
{
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    abortRestoringDeltaMemory(&deltaIndex->deltaZones[z]);
  }
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int encodeDeltaIndexHeader(Buffer *buffer, struct di_header *header)
{
  int result = putBytes(buffer, MAGIC_SIZE, MAGIC_DI_START);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, header->zoneNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt32LEIntoBuffer(buffer, header->numZones);
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
  result = putUInt64LEIntoBuffer(buffer, header->recordCount);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = putUInt64LEIntoBuffer(buffer, header->collisionCount);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(contentLength(buffer) == sizeof(*header),
                           "%zu bytes encoded of %zu expected",
                           contentLength(buffer), sizeof(*header));

  return result;
}

/**********************************************************************/
int startSavingDeltaIndex(const DeltaIndex *deltaIndex,
                          unsigned int zoneNumber,
                          BufferedWriter *bufferedWriter)
{
  DeltaMemory *deltaZone = &deltaIndex->deltaZones[zoneNumber];
  struct di_header header;
  memcpy(header.magic, MAGIC_DI_START, MAGIC_SIZE);
  header.zoneNumber     = zoneNumber;
  header.numZones       = deltaIndex->numZones;
  header.firstList      = deltaZone->firstList;
  header.numLists       = deltaZone->numLists;
  header.recordCount    = deltaZone->recordCount;
  header.collisionCount = deltaZone->collisionCount;

  Buffer *buffer;
  int result = makeBuffer(sizeof(struct di_header), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = encodeDeltaIndexHeader(buffer, &header);
  if (result != UDS_SUCCESS) {
    freeBuffer(&buffer);
    return result;
  }
  result = writeToBufferedWriter(bufferedWriter, getBufferContents(buffer),
                                 contentLength(buffer));
  freeBuffer(&buffer);
  if (result != UDS_SUCCESS) {
    return logWarningWithStringError(result,
                                     "failed to write delta index header");
  }

  for (unsigned int i = 0; i < deltaZone->numLists; i++) {
    uint16_t deltaListSize = getDeltaListSize(&deltaZone->deltaLists[i + 1]);
    byte data[2];
    storeUInt16LE(data, deltaListSize);
    result = writeToBufferedWriter(bufferedWriter, data, sizeof(data));
    if (result != UDS_SUCCESS) {
      return logWarningWithStringError(result,
                                       "failed to write delta list size");
    }
  }

  startSavingDeltaMemory(deltaZone, bufferedWriter);
  return UDS_SUCCESS;
}

/**********************************************************************/
bool isSavingDeltaIndexDone(const DeltaIndex *deltaIndex,
                            unsigned int zoneNumber)
{
  return areDeltaMemoryTransfersDone(&deltaIndex->deltaZones[zoneNumber]);
}

/**********************************************************************/
int finishSavingDeltaIndex(const DeltaIndex *deltaIndex,
                           unsigned int zoneNumber)
{
  return finishSavingDeltaMemory(&deltaIndex->deltaZones[zoneNumber]);
}

/**********************************************************************/
int abortSavingDeltaIndex(const DeltaIndex *deltaIndex,
                          unsigned int zoneNumber)
{
  abortSavingDeltaMemory(&deltaIndex->deltaZones[zoneNumber]);
  return UDS_SUCCESS;
}

/**********************************************************************/
size_t computeDeltaIndexSaveBytes(unsigned int numLists, size_t memorySize)
{
  // The exact amount of memory used depends upon the number of zones.
  // Compute the maximum potential memory size.
  size_t maxMemSize = memorySize;
  for (unsigned int numZones = 1; numZones <= MAX_ZONES; numZones++) {
    size_t memSize = getZoneMemorySize(numZones, memorySize);
    if (memSize > maxMemSize) {
      maxMemSize = memSize;
    }
  }
  // Saving a delta index requires a header ...
  return (sizeof(struct di_header)
          // ... plus a DeltaListSaveInfo per delta list
          // plus an extra byte per delta list ...
          + numLists * (sizeof(DeltaListSaveInfo) + 1)
          // ... plus the delta list memory
          + maxMemSize);
}

/**********************************************************************/
int validateDeltaIndex(const DeltaIndex *deltaIndex)
{
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    int result = validateDeltaLists(&deltaIndex->deltaZones[z]);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
static int assertNotAtEnd(const DeltaIndexEntry *deltaEntry, int errorCode)
{
  return ASSERT_WITH_ERROR_CODE(!deltaEntry->atEnd, errorCode,
                                "operation is invalid because the list entry "
                                "is at the end of the delta list");
}

/**********************************************************************/
static void prefetchDeltaList(const DeltaMemory *deltaZone,
                              const DeltaList *deltaList)
{
  const byte *memory = deltaZone->memory;
  const byte *addr = &memory[getDeltaListStart(deltaList) / CHAR_BIT];
  unsigned int size = getDeltaListSize(deltaList) / CHAR_BIT;
  prefetchRange(addr, size, false);
}

/**********************************************************************/
int startDeltaIndexSearch(const DeltaIndex *deltaIndex,
                          unsigned int listNumber, unsigned int key,
                          bool readOnly, DeltaIndexEntry *deltaEntry)
{
  int result
    = ASSERT_WITH_ERROR_CODE((listNumber < deltaIndex->numLists),
                             UDS_CORRUPT_DATA,
                             "Delta list number (%u) is out of range (%u)",
                             listNumber, deltaIndex->numLists);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int zoneNumber = getDeltaIndexZone(deltaIndex, listNumber);
  DeltaMemory *deltaZone = &deltaIndex->deltaZones[zoneNumber];
  listNumber -= deltaZone->firstList;
  result = ASSERT_WITH_ERROR_CODE((listNumber < deltaZone->numLists),
                                  UDS_CORRUPT_DATA,
                                  "Delta list number (%u)"
                                  " is out of range (%u) for zone (%u)",
                                  listNumber, deltaZone->numLists, zoneNumber);
  if (result != UDS_SUCCESS) {
    return result;
  }

  DeltaList *deltaList;
  if (deltaIndex->isMutable) {
    deltaList = &deltaZone->deltaLists[listNumber + 1];
    if (!readOnly) {
      // Here is the lazy writing of the index for a checkpoint
      lazyFlushDeltaList(deltaZone, listNumber);
    }
  } else {
    // Translate the immutable delta list header into a temporary full
    // delta list header
    deltaList = &deltaEntry->tempDeltaList;
    deltaList->startOffset = getImmutableStart(deltaZone->memory, listNumber);
    unsigned int endOffset = getImmutableStart(deltaZone->memory,
                                               listNumber + 1);
    deltaList->size = endOffset - deltaList->startOffset;
    deltaList->saveKey = 0;
    deltaList->saveOffset = 0;
  }

  if (key > deltaList->saveKey) {
    deltaEntry->key    = deltaList->saveKey;
    deltaEntry->offset = deltaList->saveOffset;
  } else {
    deltaEntry->key    = 0;
    deltaEntry->offset = 0;
    if (key == 0) {
      // This usually means we're about to walk the entire delta list, so get
      // all of it into the CPU cache.
      prefetchDeltaList(deltaZone, deltaList);
    }
  }

  deltaEntry->atEnd        = false;
  deltaEntry->deltaZone    = deltaZone;
  deltaEntry->deltaList    = deltaList;
  deltaEntry->entryBits    = 0;
  deltaEntry->isCollision  = false;
  deltaEntry->listNumber   = listNumber;
  deltaEntry->listOverflow = false;
  deltaEntry->valueBits    = deltaZone->valueBits;
  return UDS_SUCCESS;
}

/**********************************************************************/
__attribute__((__noinline__))
int nextDeltaIndexEntry(DeltaIndexEntry *deltaEntry)
{
  int result = assertNotAtEnd(deltaEntry, UDS_BAD_STATE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  const DeltaList *deltaList = deltaEntry->deltaList;
  deltaEntry->offset += deltaEntry->entryBits;
  unsigned int size = getDeltaListSize(deltaList);
  if (unlikely(deltaEntry->offset >= size)) {
    deltaEntry->atEnd       = true;
    deltaEntry->delta       = 0;
    deltaEntry->isCollision = false;
    return ASSERT_WITH_ERROR_CODE((deltaEntry->offset == size),
                                  UDS_CORRUPT_DATA,
                                  "next offset past end of delta list");
  }

  decodeDelta(deltaEntry);

  unsigned int nextOffset = deltaEntry->offset + deltaEntry->entryBits;
  if (nextOffset > size) {
    // This is not an assertion because validateChapterIndexPage() wants to
    // handle this error.
    logWarning("Decoded past the end of the delta list");
    return UDS_CORRUPT_DATA;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int rememberDeltaIndexOffset(const DeltaIndexEntry *deltaEntry)
{
  int result = ASSERT(!deltaEntry->isCollision, "entry is not a collision");
  if (result != UDS_SUCCESS) {
    return result;
  }

  DeltaList *deltaList = deltaEntry->deltaList;
  deltaList->saveKey = deltaEntry->key - deltaEntry->delta;
  deltaList->saveOffset = deltaEntry->offset;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getDeltaIndexEntry(const DeltaIndex *deltaIndex, unsigned int listNumber,
                       unsigned int key, const byte *name, bool readOnly,
                       DeltaIndexEntry *deltaEntry)
{
  int result = startDeltaIndexSearch(deltaIndex, listNumber, key, readOnly,
                                     deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  do {
    result = nextDeltaIndexEntry(deltaEntry);
    if (result != UDS_SUCCESS) {
      return result;
    }
  } while (!deltaEntry->atEnd && (key > deltaEntry->key));

  result = rememberDeltaIndexOffset(deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (!deltaEntry->atEnd && (key == deltaEntry->key)) {
    DeltaIndexEntry collisionEntry;
    collisionEntry = *deltaEntry;
    for (;;) {
      result = nextDeltaIndexEntry(&collisionEntry);
      if (result != UDS_SUCCESS) {
        return result;
      }
      if (collisionEntry.atEnd || !collisionEntry.isCollision) {
        break;
      }
      byte collisionName[COLLISION_BYTES];
      getBytes(deltaEntry->deltaZone->memory,
               getCollisionOffset(&collisionEntry), collisionName,
               COLLISION_BYTES);
      if (memcmp(collisionName, name, COLLISION_BYTES) == 0) {
        *deltaEntry = collisionEntry;
        break;
      }
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int getDeltaEntryCollision(const DeltaIndexEntry *deltaEntry, byte *name)
{
  int result = assertNotAtEnd(deltaEntry, UDS_BAD_STATE);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_WITH_ERROR_CODE(deltaEntry->isCollision, UDS_BAD_STATE,
                                  "Cannot get full block name from a"
                                  " non-collision delta index entry");
  if (result != UDS_SUCCESS) {
    return result;
  }

  getBytes(deltaEntry->deltaZone->memory, getCollisionOffset(deltaEntry),
           name, COLLISION_BYTES);
  return UDS_SUCCESS;
}

/**********************************************************************/
static int assertMutableEntry(const DeltaIndexEntry *deltaEntry)
{
  return ASSERT_WITH_ERROR_CODE(deltaEntry->deltaList
                                != &deltaEntry->tempDeltaList,
                                UDS_BAD_STATE,
                                "delta index is mutable");
}

/**********************************************************************/
int setDeltaEntryValue(const DeltaIndexEntry *deltaEntry, unsigned int value)
{
  int result = assertMutableEntry(deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = assertNotAtEnd(deltaEntry, UDS_BAD_STATE);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT_WITH_ERROR_CODE(((value & ((1 << deltaEntry->valueBits) - 1))
                                   == value), UDS_INVALID_ARGUMENT,
                                  "Value (%u) being set in a delta index is "
                                  "too large (must fit in %u bits)",
                                  value, deltaEntry->valueBits);
  if (result != UDS_SUCCESS) {
    return result;
  }

  setField(value, deltaEntry->deltaZone->memory,
           getDeltaEntryOffset(deltaEntry), deltaEntry->valueBits);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putDeltaIndexEntry(DeltaIndexEntry *deltaEntry, unsigned int key,
                       unsigned int value, const byte *name)
{
  int result = assertMutableEntry(deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (deltaEntry->isCollision) {
    /*
     * The caller wants us to insert a collision entry onto a collision
     * entry.  This happens when we find a collision and attempt to add the
     * name again to the index.  This is normally a fatal error unless we
     * are replaying a closed chapter while we are rebuilding a master
     * index.
     */
    return UDS_DUPLICATE_NAME;
  }

  if (deltaEntry->offset < deltaEntry->deltaList->saveOffset) {
    // The saved entry offset is after the new entry and will no longer be
    // valid, so replace it with the insertion point.
    result = rememberDeltaIndexOffset(deltaEntry);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  if (name != NULL) {
    // We are inserting a collision entry which is placed after this entry
    result = assertNotAtEnd(deltaEntry, UDS_BAD_STATE);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = ASSERT((key == deltaEntry->key),
                    "incorrect key for collision entry");
    if (result != UDS_SUCCESS) {
      return result;
    }

    deltaEntry->offset += deltaEntry->entryBits;
    setDelta(deltaEntry, 0);
    setCollision(deltaEntry);
    result = insertBits(deltaEntry, deltaEntry->entryBits);
  } else if (deltaEntry->atEnd) {
    // We are inserting a new entry at the end of the delta list
    result = ASSERT((key >= deltaEntry->key), "key past end of list");
    if (result != UDS_SUCCESS) {
      return result;
    }

    setDelta(deltaEntry, key - deltaEntry->key);
    deltaEntry->key   = key;
    deltaEntry->atEnd = false;
    result = insertBits(deltaEntry, deltaEntry->entryBits);
  } else {
    // We are inserting a new entry which requires the delta in the
    // following entry to be updated.
    result = ASSERT((key < deltaEntry->key), "key precedes following entry");
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = ASSERT((key >= deltaEntry->key - deltaEntry->delta),
                    "key effects following entry's delta");
    if (result != UDS_SUCCESS) {
      return result;
    }

    int oldEntrySize = deltaEntry->entryBits;
    DeltaIndexEntry nextEntry = *deltaEntry;
    unsigned int nextValue = getDeltaEntryValue(&nextEntry);
    setDelta(deltaEntry, key - (deltaEntry->key - deltaEntry->delta));
    deltaEntry->key = key;
    setDelta(&nextEntry, nextEntry.key - key);
    nextEntry.offset += deltaEntry->entryBits;
    // The 2 new entries are always bigger than the 1 entry we are replacing
    int additionalSize
      = deltaEntry->entryBits + nextEntry.entryBits - oldEntrySize;
    result = insertBits(deltaEntry, additionalSize);
    if (result != UDS_SUCCESS) {
      return result;
    }
    encodeEntry(&nextEntry, nextValue, NULL);
  }
  if (result != UDS_SUCCESS) {
    return result;
  }
  encodeEntry(deltaEntry, value, name);

  DeltaMemory *deltaZone = deltaEntry->deltaZone;
  deltaZone->recordCount++;
  deltaZone->collisionCount += deltaEntry->isCollision ? 1 : 0;
  return UDS_SUCCESS;
}

/**********************************************************************/
int removeDeltaIndexEntry(DeltaIndexEntry *deltaEntry)
{
  int result = assertMutableEntry(deltaEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  DeltaIndexEntry nextEntry = *deltaEntry;
  result = nextDeltaIndexEntry(&nextEntry);
  if (result != UDS_SUCCESS) {
    return result;
  }

  DeltaMemory *deltaZone = deltaEntry->deltaZone;

  if (deltaEntry->isCollision) {
    // This is a collision entry, so just remove it
    deleteBits(deltaEntry, deltaEntry->entryBits);
    nextEntry.offset = deltaEntry->offset;
    deltaZone->collisionCount -= 1;
  } else if (nextEntry.atEnd) {
    // This entry is at the end of the list, so just remove it
    deleteBits(deltaEntry, deltaEntry->entryBits);
    nextEntry.key -= deltaEntry->delta;
    nextEntry.offset = deltaEntry->offset;
  } else {
    // The delta in the next entry needs to be updated.
    unsigned int nextValue = getDeltaEntryValue(&nextEntry);
    int oldSize = deltaEntry->entryBits + nextEntry.entryBits;
    if (nextEntry.isCollision) {
      // The next record is a collision. It needs to be rewritten as a
      // non-collision with a larger delta.
      nextEntry.isCollision = false;
      deltaZone->collisionCount -= 1;
    }
    setDelta(&nextEntry, deltaEntry->delta + nextEntry.delta);
    nextEntry.offset = deltaEntry->offset;
    // The 1 new entry is always smaller than the 2 entries we are replacing
    deleteBits(deltaEntry, oldSize - nextEntry.entryBits);
    encodeEntry(&nextEntry, nextValue, NULL);
  }
  deltaZone->recordCount--;
  deltaZone->discardCount++;
  *deltaEntry = nextEntry;

  DeltaList *deltaList = deltaEntry->deltaList;
  if (deltaEntry->offset < deltaList->saveOffset) {
    // The saved entry offset is after the entry we just removed and it
    // will no longer be valid.  We must force the next search to start at
    // the beginning.
    deltaList->saveKey = 0;
    deltaList->saveOffset = 0;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
unsigned int getDeltaIndexZoneFirstList(const DeltaIndex *deltaIndex,
                                        unsigned int zoneNumber)
{
  return deltaIndex->deltaZones[zoneNumber].firstList;
}

/**********************************************************************/
unsigned int getDeltaIndexZoneNumLists(const DeltaIndex *deltaIndex,
                                       unsigned int zoneNumber)
{
  return deltaIndex->deltaZones[zoneNumber].numLists;
}

/**********************************************************************/
uint64_t getDeltaIndexZoneDlistBitsUsed(const DeltaIndex *deltaIndex,
                                        unsigned int zoneNumber)
{
  uint64_t bitCount = 0;
  const DeltaMemory *deltaZone = &deltaIndex->deltaZones[zoneNumber];
  for (unsigned int i = 0; i < deltaZone->numLists; i++) {
    bitCount += getDeltaListSize(&deltaZone->deltaLists[i + 1]);
  }
  return bitCount;
}

/**********************************************************************/
uint64_t getDeltaIndexDlistBitsUsed(const DeltaIndex *deltaIndex)
{
  uint64_t bitCount = 0;
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    bitCount += getDeltaIndexZoneDlistBitsUsed(deltaIndex, z);
  }
  return bitCount;
}

/**********************************************************************/
uint64_t getDeltaIndexDlistBitsAllocated(const DeltaIndex *deltaIndex)
{
  uint64_t byteCount = 0;
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    const DeltaMemory *deltaZone = &deltaIndex->deltaZones[z];
    byteCount += deltaZone->size;
  }
  return byteCount * CHAR_BIT;
}

/**********************************************************************/
void getDeltaIndexStats(const DeltaIndex *deltaIndex, DeltaIndexStats *stats)
{
  memset(stats, 0, sizeof(DeltaIndexStats));
  stats->memoryAllocated = deltaIndex->numZones * sizeof(DeltaMemory);
  for (unsigned int z = 0; z < deltaIndex->numZones; z++) {
    const DeltaMemory *deltaZone = &deltaIndex->deltaZones[z];
    stats->memoryAllocated += getDeltaMemoryAllocated(deltaZone);
    stats->rebalanceTime   += deltaZone->rebalanceTime;
    stats->rebalanceCount  += deltaZone->rebalanceCount;
    stats->recordCount     += deltaZone->recordCount;
    stats->collisionCount  += deltaZone->collisionCount;
    stats->discardCount    += deltaZone->discardCount;
    stats->overflowCount   += deltaZone->overflowCount;
    stats->numLists        += deltaZone->numLists;
  }
}

/**********************************************************************/
uint64_t getDeltaIndexVirtualChapterNumber(const DeltaIndex *deltaIndex)
{
  const DeltaPageHeader *header
    = (const DeltaPageHeader *) deltaIndex->deltaZones[0].memory;
  return header->virtualChapterNumber;
}

/**********************************************************************/
unsigned int getDeltaIndexLowestListNumber(const DeltaIndex *deltaIndex)
{
  const DeltaMemory *deltaZone = &deltaIndex->deltaZones[0];
  return (deltaIndex->isMutable
          ? deltaZone->firstList
          : ((DeltaPageHeader *) deltaZone->memory)->firstList);
}

/**********************************************************************/
unsigned int getDeltaIndexHighestListNumber(const DeltaIndex *deltaIndex)
{
  return getDeltaIndexLowestListNumber(deltaIndex) + deltaIndex->numLists - 1;
}

/**********************************************************************/
unsigned int getDeltaIndexPageCount(unsigned int numEntries,
                                    unsigned int numLists,
                                    unsigned int meanDelta,
                                    unsigned int numPayloadBits,
                                    size_t bytesPerPage)
{
  // Compute the number of bits needed for all the entries
  size_t bitsPerIndex
    = getDeltaMemorySize(numEntries, meanDelta, numPayloadBits);
  // Compute the number of bits needed for a single delta list
  unsigned int bitsPerDeltaList = bitsPerIndex / numLists;
  // Adjust the bits per index, adding the immutable delta list headers
  bitsPerIndex += numLists * IMMUTABLE_HEADER_SIZE;
  // Compute the number of usable bits on an immutable index page
  unsigned int bitsPerPage
    = (bytesPerPage - sizeof(DeltaPageHeader)) * CHAR_BIT;
  // Adjust the bits per page, taking away one immutable delta list header
  // and one delta list representing internal fragmentation
  bitsPerPage -= IMMUTABLE_HEADER_SIZE + bitsPerDeltaList;
  // Now compute the number of pages needed
  return (bitsPerIndex + bitsPerPage - 1) / bitsPerPage;
}

/**********************************************************************/
void logDeltaIndexEntry(DeltaIndexEntry *deltaEntry)
{
  logInfo("List 0x%X Key 0x%X Offset 0x%X%s%s ListSize 0x%X%s",
          deltaEntry->listNumber, deltaEntry->key, deltaEntry->offset,
          deltaEntry->atEnd ? " end" : "",
          deltaEntry->isCollision ? " collision" : "",
          getDeltaListSize(deltaEntry->deltaList),
          deltaEntry->listOverflow ? " overflow" : "");
  deltaEntry->listOverflow = false;
}
