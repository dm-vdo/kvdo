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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/blockMapEntry.h#1 $
 */

#ifndef BLOCK_MAP_ENTRY_H
#define BLOCK_MAP_ENTRY_H

#include "blockMappingState.h"
#include "constants.h"
#include "types.h"

/**
 * The entry for each logical block in the block map is encoded into five
 * bytes, which saves space in both the on-disk and in-memory layouts. It
 * consists of the 36 low-order bits of a PhysicalBlockNumber (addressing 256
 * terabytes with a 4KB block size) and a 4-bit encoding of a
 * BlockMappingState.
 **/
typedef struct __attribute__((packed)) blockMapEntry {
  unsigned mappingState  : 4;   // The 4-bit BlockMappingState
  unsigned pbnHighNibble : 4;   // 4 highest bits of the physical block number
  uint32_t pbnLowWord;          // 32 low-order bits of the PBN
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
  return (((PhysicalBlockNumber) entry->pbnHighNibble << 32)
          | entry->pbnLowWord);
}

/**
 * Unpack a packed PhysicalBlockNumber from a BlockMapEntry with an offset
 *
 * @param entry  The entry to unpack
 * @param offset  The offset of the PBN from its absolute PBN
 *
 * @return the unpacked representation of the absolute physical block number
 **/
static inline PhysicalBlockNumber unpackOffsetPBN(const BlockMapEntry *entry,
                                                  uint8_t              offset)
{
  PhysicalBlockNumber pbn = unpackPBN(entry);
  return (pbn == ZERO_BLOCK) ? pbn : pbn + offset;
}

/**********************************************************************/
static inline bool isUnmapped(const BlockMapEntry *entry)
{
  return (entry->mappingState == MAPPING_STATE_UNMAPPED);
}

/**********************************************************************/
static inline bool isInvalid(const BlockMapEntry *entry)
{
  PhysicalBlockNumber pbn = unpackPBN(entry);
  return (((pbn == ZERO_BLOCK) && isCompressed(entry->mappingState))
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
  return (BlockMapEntry) {
    .mappingState  = mappingState,
    .pbnHighNibble = ((pbn >> 32) & 0x0F),
    .pbnLowWord    = (pbn & 0xFFFFFFFF),
  };
}

#endif // BLOCK_MAP_ENTRY_H
