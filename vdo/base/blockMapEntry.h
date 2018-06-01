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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapEntry.h#3 $
 */

#ifndef BLOCK_MAP_ENTRY_H
#define BLOCK_MAP_ENTRY_H

#include "blockMappingState.h"
#include "constants.h"
#include "numeric.h"
#include "types.h"

/**
 * The entry for each logical block in the block map is encoded into five
 * bytes, which saves space in both the on-disk and in-memory layouts. It
 * consists of the 36 low-order bits of a PhysicalBlockNumber (addressing 256
 * terabytes with a 4KB block size) and a 4-bit encoding of a
 * BlockMappingState.
 **/
typedef union __attribute__((packed)) blockMapEntry {
  struct __attribute__((packed)) {
    /**
     * Bits 7..4: The four highest bits of the 36-bit physical block number
     * Bits 3..0: The 4-bit BlockMappingState
     **/
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned mappingState  : 4;
    unsigned pbnHighNibble : 4;
#else
    unsigned pbnHighNibble : 4;
    unsigned mappingState  : 4;
#endif

    /** 32 low-order bits of the 36-bit PBN, in little-endian byte order */
    byte pbnLowWord[4];
  } fields;

  // A raw view of the packed encoding.
  uint8_t raw[5];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining packed entries in GDB.
  struct __attribute__((packed)) {
    unsigned mappingState  : 4;
    unsigned pbnHighNibble : 4;
    uint32_t pbnLowWord;
  } littleEndian;
#endif
} BlockMapEntry;

/**
 * Unpack a packed PhysicalBlockNumber from a BlockMapEntry.
 *
 * @param entry          A pointer to the entry
 *
 * @return the unpacked representation of the absolute physical block number
 **/
static inline PhysicalBlockNumber unpackPBN(const BlockMapEntry *entry)
{
  PhysicalBlockNumber low32 = getUInt32LE(entry->fields.pbnLowWord);
  PhysicalBlockNumber high4 = entry->fields.pbnHighNibble;
  return ((high4 << 32) | low32);
}

/**
 * Unpack the BlockMappingState encoded in a BlockMapEntry.
 *
 * @param entry          A pointer to the entry
 *
 * @return the unpacked representation of the mapping state
 **/
static inline BlockMappingState unpackMappingState(const BlockMapEntry *entry)
{
  return entry->fields.mappingState;
}

/**********************************************************************/
static inline bool isUnmapped(const BlockMapEntry *entry)
{
  return (unpackMappingState(entry) == MAPPING_STATE_UNMAPPED);
}

/**********************************************************************/
static inline bool isInvalid(const BlockMapEntry *entry)
{
  PhysicalBlockNumber pbn = unpackPBN(entry);
  return (((pbn == ZERO_BLOCK) && isCompressed(unpackMappingState(entry)))
          || ((pbn != ZERO_BLOCK) && isUnmapped(entry)));
}

/**
 * Pack a PhysicalBlockNumber into a BlockMapEntry.
 *
 * @param pbn            The physical block number to convert to its
 *                       packed five-byte representation
 * @param mappingState   The mapping state of the block
 *
 * @return the packed representation of the block number and mapping state
 *
 * @note unrepresentable high bits of the unpacked PBN are silently truncated
 **/
static inline BlockMapEntry packPBN(PhysicalBlockNumber pbn,
                                    BlockMappingState   mappingState)
{
  BlockMapEntry entry;
  entry.fields.mappingState  = (mappingState & 0x0F);
  entry.fields.pbnHighNibble = ((pbn >> 32) & 0x0F),
  storeUInt32LE(entry.fields.pbnLowWord, pbn & UINT_MAX);
  return entry;
}

#endif // BLOCK_MAP_ENTRY_H
