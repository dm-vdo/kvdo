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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMappingState.h#1 $
 */

#ifndef BLOCK_MAPPING_STATE_H
#define BLOCK_MAPPING_STATE_H

#include "common.h"

/**
 * Four bits of each five-byte block map entry contain a mapping state value
 * used to distinguish unmapped or trimmed logical blocks (which are treated
 * as mapped to the zero block) from entries that have been mapped to a
 * physical block, including the zero block.
 **/
typedef enum {
  MAPPING_STATE_UNMAPPED        = 0,  // Must be zero to be the default value
  MAPPING_STATE_UNCOMPRESSED    = 1,  // A normal (uncompressed) block
  MAPPING_STATE_COMPRESSED_BASE = 2,  // Compressed in slot 0
  MAPPING_STATE_COMPRESSED_MAX  = 15, // Compressed in slot 13
} BlockMappingState;

/**
 * The total number of compressed blocks that can live in a physical block.
 **/
enum {
  MAX_COMPRESSION_SLOTS =
    MAPPING_STATE_COMPRESSED_MAX - MAPPING_STATE_COMPRESSED_BASE + 1,
};

/**********************************************************************/
static inline BlockMappingState getStateForSlot(byte slotNumber)
{
  return (slotNumber + MAPPING_STATE_COMPRESSED_BASE);
}

/**********************************************************************/
static inline byte getSlotFromState(BlockMappingState mappingState)
{
  return (mappingState - MAPPING_STATE_COMPRESSED_BASE);
}

/**********************************************************************/
static inline bool isCompressed(const BlockMappingState mappingState)
{
  return (mappingState > MAPPING_STATE_UNCOMPRESSED);
}

#endif // BLOCK_MAPPING_STATE_H
