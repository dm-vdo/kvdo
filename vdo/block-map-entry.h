/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BLOCK_MAP_ENTRY_H
#define BLOCK_MAP_ENTRY_H

#include "block-mapping-state.h"
#include "constants.h"
#include "numeric.h"
#include "types.h"

/**
 * DOC: Block map entries
 *
 * The entry for each logical block in the block map is encoded into five
 * bytes, which saves space in both the on-disk and in-memory layouts. It
 * consists of the 36 low-order bits of a physical_block_number_t
 * (addressing 256 terabytes with a 4KB block size) and a 4-bit encoding of a
 * block_mapping_state.
 *
 * Of the 8 high bits of the 5-byte structure:
 *
 * Bits 7..4: The four highest bits of the 36-bit physical block
 * number
 * Bits 3..0: The 4-bit block_mapping_state
 *
 * The following 4 bytes are the low order bytes of the physical block number,
 * in little-endian order.
 *
 * Conversion functions to and from a data location are provided.
 */
struct block_map_entry {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned mapping_state : 4;
	unsigned pbn_high_nibble : 4;
#else
	unsigned pbn_high_nibble : 4;
	unsigned mapping_state : 4;
#endif

	__le32 pbn_low_word;
} __packed;

static inline struct data_location
vdo_unpack_block_map_entry(const struct block_map_entry *entry)
{
	physical_block_number_t low32 = __le32_to_cpu(entry->pbn_low_word);
	physical_block_number_t high4 = entry->pbn_high_nibble;

	return (struct data_location) {
		.pbn = ((high4 << 32) | low32),
		.state = entry->mapping_state,
	};
}

static inline bool vdo_is_mapped_location(const struct data_location *location)
{
	return (location->state != VDO_MAPPING_STATE_UNMAPPED);
}

static inline bool vdo_is_valid_location(const struct data_location *location)
{
	if (location->pbn == VDO_ZERO_BLOCK) {
		return !vdo_is_state_compressed(location->state);
	} else {
		return vdo_is_mapped_location(location);
	}
}

/* FIXME: maybe this should be vdo_pack_block_map_entry() */
static inline struct block_map_entry
vdo_pack_pbn(physical_block_number_t pbn, enum block_mapping_state mapping_state)
{
	return (struct block_map_entry) {
		.mapping_state = (mapping_state & 0x0F),
		.pbn_high_nibble = ((pbn >> 32) & 0x0F),
		.pbn_low_word = __cpu_to_le32(pbn & UINT_MAX),
	};
}

#endif /* BLOCK_MAP_ENTRY_H */
