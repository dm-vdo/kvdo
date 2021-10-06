/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/block-map-entry.h#1 $
 */

#ifndef BLOCK_MAP_ENTRY_H
#define BLOCK_MAP_ENTRY_H

#include "block-mapping-state.h"
#include "constants.h"
#include "numeric.h"
#include "types.h"

/**
 * The entry for each logical block in the block map is encoded into five
 * bytes, which saves space in both the on-disk and in-memory layouts. It
 * consists of the 36 low-order bits of a physical_block_number_t
 * (addressing 256 terabytes with a 4KB block size) and a 4-bit encoding of a
 * block_mapping_state.
 **/
struct block_map_entry {
	/**
	 * Bits 7..4: The four highest bits of the 36-bit physical block
	 * number
	 * Bits 3..0: The 4-bit block_mapping_state
	 **/
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned mapping_state : 4;
	unsigned pbn_high_nibble : 4;
#else
	unsigned pbn_high_nibble : 4;
	unsigned mapping_state : 4;
#endif

	/**
	 * 32 low-order bits of the 36-bit PBN, in little-endian byte
	 * order
	 */
	__le32 pbn_low_word;
} __packed;

/**
 * Unpack the fields of a block_map_entry, returning them as a data_location.
 *
 * @param entry   A pointer to the entry to unpack
 *
 * @return the location of the data mapped by the block map entry
 **/
static inline struct data_location
unpack_vdo_block_map_entry(const struct block_map_entry *entry)
{
	physical_block_number_t low32 = __le32_to_cpu(entry->pbn_low_word);
	physical_block_number_t high4 = entry->pbn_high_nibble;

	return (struct data_location) {
		.pbn = ((high4 << 32) | low32),
		.state = entry->mapping_state,
	};
}

/**********************************************************************/
static inline bool vdo_is_mapped_location(const struct data_location *location)
{
	return (location->state != VDO_MAPPING_STATE_UNMAPPED);
}

/**********************************************************************/
static inline bool vdo_is_valid_location(const struct data_location *location)
{
	if (location->pbn == VDO_ZERO_BLOCK) {
		return !vdo_is_state_compressed(location->state);
	} else {
		return vdo_is_mapped_location(location);
	}
}

/**
 * Pack a physical_block_number_t into a block_map_entry.
 *
 * @param pbn             The physical block number to convert to its
 *                        packed five-byte representation
 * @param mapping_state   The mapping state of the block
 *
 * @return the packed representation of the block number and mapping state
 *
 * @note unrepresentable high bits of the unpacked PBN are silently truncated
 **/
static inline struct block_map_entry
pack_vdo_pbn(physical_block_number_t pbn, enum block_mapping_state mapping_state)
{
	return (struct block_map_entry) {
		.mapping_state = (mapping_state & 0x0F),
		.pbn_high_nibble = ((pbn >> 32) & 0x0F),
		.pbn_low_word = __cpu_to_le32(pbn & UINT_MAX),
	};
}

#endif // BLOCK_MAP_ENTRY_H
