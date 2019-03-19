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
 * $Id: //eng/uds-releases/gloria/src/uds/hashUtils.h#1 $
 */

#ifndef HASH_UTILS_H
#define HASH_UTILS_H 1

#include "compiler.h"
#include "common.h"
#include "geometry.h"
#include "numeric.h"
#include "uds.h"

// How various portions of a hash are apportioned.  Size dependent.
#if UDS_CHUNK_NAME_SIZE == 16
enum {
  MASTER_INDEX_BYTES_OFFSET  = 0,  // size 8
  CHAPTER_INDEX_BYTES_OFFSET = 8,  // size 6
  SAMPLE_BYTES_OFFSET        = 14, // size 2
  MASTER_INDEX_BYTES_COUNT   = 8,
  CHAPTER_INDEX_BYTES_COUNT  = 6,
  SAMPLE_BYTES_COUNT         = 2,
};
#else
enum {
  SAMPLE_BYTES_OFFSET        = 0,  // size 4
  CHAPTER_INDEX_BYTES_OFFSET = 12, // size 8
  GRID_BYTES_OFFSET          = 20, // size 4
  MASTER_INDEX_BYTES_OFFSET  = 24, // size 8
  SAMPLE_BYTES_COUNT         = 4,
  CHAPTER_INDEX_BYTES_COUNT  = 8,
  GRID_BYTES_COUNT           = 4,
  MASTER_INDEX_BYTES_COUNT   = 8
};
#endif

/**
 * Extract the portion of a block name used by the chapter index.
 *
 * @param name The block name
 *
 * @return The chapter index bytes
 **/
static INLINE uint64_t extractChapterIndexBytes(const UdsChunkName *name)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    // Get the high order 16 bits, then the low order 32 bits
    uint64_t bytes
      = (uint64_t) getUInt16BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET]) << 32;
    bytes |= getUInt32BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET + 2]);
    return bytes;
  } else {
    return getUInt64BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET]);
  }
}

/**
 * Extract the portion of a block name used by the grid router.
 *
 * @param name The block name
 *
 * @return The grid router portion of the block name
 **/
static INLINE uint32_t extractGridRouterBytes(const UdsChunkName *name
                                              __attribute__((unused)))
{
#if UDS_CHUNK_NAME_SIZE == 16
  return 0;
#else
  return getUInt32BE(&name->name[GRID_BYTES_OFFSET]);
#endif
}

/**
 * Extract the portion of a block name used by the master index.
 *
 * @param name The block name
 *
 * @return The master index portion of the block name
 **/
static INLINE uint64_t extractMasterIndexBytes(const UdsChunkName *name)
{
  return getUInt64BE(&name->name[MASTER_INDEX_BYTES_OFFSET]);
}

/**
 * Extract the portion of a block name used for sparse sampling.
 *
 * @param name The block name
 *
 * @return The sparse sample portion of the block name
 **/
static INLINE uint32_t extractSamplingBytes(const UdsChunkName *name)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    return getUInt16BE(&name->name[SAMPLE_BYTES_OFFSET]);
  } else {
    return getUInt32BE(&name->name[SAMPLE_BYTES_OFFSET]);
  }
}

/**
 * For a given block, find the chapter delta list to use
 *
 * @param name     The block name to hash
 * @param geometry The geometry to use
 *
 * @return The chapter delta list where we expect to find the given blockname
 **/
static INLINE unsigned int hashToChapterDeltaList(const UdsChunkName *name,
                                                  const Geometry     *geometry)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    return (unsigned int) ((extractChapterIndexBytes(name)
                            >> geometry->chapterAddressBits)
                           & ((1 << geometry->chapterDeltaListBits) - 1));
  } else {
    return (unsigned int) ((extractChapterIndexBytes(name))
                           & ((1 << geometry->chapterDeltaListBits) - 1));
  }
}

/**
 * For a given block, find the chapter delta address to use
 *
 * @param name     The block name to hash
 * @param geometry The geometry to use
 *
 * @return The chapter delta address to use
 **/
static INLINE unsigned int hashToChapterDeltaAddress(const UdsChunkName *name,
                                                     const Geometry *geometry)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    return (unsigned int) (extractChapterIndexBytes(name)
                           & ((1 << geometry->chapterAddressBits) - 1));
  } else {
    // the actual number of bits should come from the geometry.
    return (unsigned int) ((extractChapterIndexBytes(name) >> 32)
                           & ((1 << geometry->chapterAddressBits) - 1));
  }
}

/**
 * For a given block name, find the slot in the open chapter hash table
 * where it is expected to reside.
 *
 * @param name      The block name to hash
 * @param slotCount The size of the hash table
 *
 * @return the record number in the index page where we expect to find
 #         the given blockname
 **/
static INLINE unsigned int nameToHashSlot(const UdsChunkName *name,
                                          unsigned int slotCount)
{
  return (unsigned int) (extractChapterIndexBytes(name) % slotCount);
}

/**
 * Convert a chunk name to hex to make it more readable.
 *
 * @param chunkName  The chunk name
 * @param hexData    The resulting hexdata from the given chunk name
 * @param hexDataLen The capacity of hexData
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hexDataLen
 *                      is too short.
 **/
int chunkNameToHex(const UdsChunkName *chunkName,
                   char               *hexData,
                   size_t              hexDataLen)
  __attribute__((warn_unused_result));

/**
 * Convert chunk data to hex to make it more readable.
 *
 * @param chunkData  The chunk data
 * @param hexData    The resulting hexdata from the given chunk data
 * @param hexDataLen The capacity of hexData
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hexDataLen
 *                      is too short.
 **/
int chunkDataToHex(const UdsChunkData *chunkData,
                   char               *hexData,
                   size_t              hexDataLen)
  __attribute__((warn_unused_result));

/**
 * Compute the number of bits required to store a field with the given
 * maximum value.
 *
 * @param maxValue   The maximum value of the field
 *
 * @return           the number of bits required
 **/
unsigned int computeBits(unsigned int maxValue)
  __attribute__((warn_unused_result));

/**
 * FOR TESTING. Set the portion of a block name used by the chapter index.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static INLINE void setChapterIndexBytes(UdsChunkName *name, uint64_t value)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    // Store the high order bytes, then the low-order bytes
    storeUInt16BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET],
                  (uint16_t)(value >> 32));
    storeUInt32BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET + 2],
                  (uint32_t)value);
  } else {
    storeUInt64BE(&name->name[CHAPTER_INDEX_BYTES_OFFSET], value);
  }
}

/**
 * FOR TESTING. Set the bits used to find a chapter delta list
 *
 * @param name     The block name
 * @param geometry The geometry to use
 * @param value    The value to store
 **/
static INLINE void setChapterDeltaListBits(UdsChunkName   *name,
                                           const Geometry *geometry,
                                           uint64_t        value)
{
  uint64_t deltaAddress = hashToChapterDeltaAddress(name, geometry);
  if (UDS_CHUNK_NAME_SIZE == 16) {
    deltaAddress |= value << geometry->chapterAddressBits;
    setChapterIndexBytes(name, deltaAddress);
  } else {
    // the actual number of bits should come from the geometry.
    setChapterIndexBytes(name, (deltaAddress << 32) | value);
  }
}

/**
 * FOR TESTING. Set the portion of a block name used by the master index.
 *
 * @param name  The block name
 * @param val   The value to store
 **/
static INLINE void setMasterIndexBytes(UdsChunkName *name, uint64_t val)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    storeUInt64BE(&name->name[MASTER_INDEX_BYTES_OFFSET], val);
  } else {
    storeUInt64BE(&name->name[MASTER_INDEX_BYTES_OFFSET], val);
  }
}

/**
 * Set the portion of a block name used for sparse sampling.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static INLINE void setSamplingBytes(UdsChunkName *name, uint32_t value)
{
  if (UDS_CHUNK_NAME_SIZE == 16) {
    storeUInt16BE(&name->name[SAMPLE_BYTES_OFFSET], (uint16_t)value);
  } else {
    storeUInt32BE(&name->name[SAMPLE_BYTES_OFFSET], value);
  }
}

/**
 * Special function wrapper required for compile-time assertions. This
 * function will fail to compile if UDS_CHUNK_NAME_SIZE is not an integer
 * multiple of 8.
 **/
void hashUtilsCompileTimeAssertions(void);

#endif /* HASH_UTILS_H */
