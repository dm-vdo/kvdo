/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_SUMMARY_FORMAT_H
#define SLAB_SUMMARY_FORMAT_H

#include "constants.h"
#include "types.h"

/**
 * The offset of a slab journal tail block.
 **/
typedef uint8_t tail_block_offset_t;

enum {
	VDO_SLAB_SUMMARY_FULLNESS_HINT_BITS = 6,
};

struct slab_summary_entry {
	/** Bits 7..0: The offset of the tail block within the slab journal */
	tail_block_offset_t tail_block_offset;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	/** Bits 13..8: A hint about the fullness of the slab */
	unsigned int fullness_hint : 6;
	/** Bit 14: Whether the ref_counts must be loaded from the layer */
	unsigned int load_ref_counts : 1;
	/** Bit 15: The believed cleanliness of this slab */
	unsigned int is_dirty : 1;
#else
	/** Bit 15: The believed cleanliness of this slab */
	unsigned int is_dirty : 1;
	/** Bit 14: Whether the ref_counts must be loaded from the layer */
	unsigned int load_ref_counts : 1;
	/** Bits 13..8: A hint about the fullness of the slab */
	unsigned int fullness_hint : 6;
#endif
} __packed;

/* XXX: These methods shouldn't take a block_size parameter. */

/**
 * Returns the size on disk of a single zone of the slab_summary.
 *
 * @param block_size  The block size of the physical layer
 *
 * @return the number of blocks required to store a single zone of the
 *         slab_summary on disk
 **/
static inline block_count_t __must_check
vdo_get_slab_summary_zone_size(block_size_t block_size)
{
	slab_count_t entries_per_block =
		block_size / sizeof(struct slab_summary_entry);
	block_count_t blocks_needed = MAX_VDO_SLABS / entries_per_block;
	return blocks_needed;
}

/**
 * Returns the size on disk of the slab_summary structure.
 *
 * @param block_size  The block size of the physical layer
 *
 * @return the blocks required to store the slab_summary on disk
 **/
static inline block_count_t __must_check
vdo_get_slab_summary_size(block_size_t block_size)
{
	return vdo_get_slab_summary_zone_size(block_size) * MAX_VDO_PHYSICAL_ZONES;
}

/**
 * Computes the shift for slab summary hints.
 *
 * @param slab_size_shift  Exponent for the number of blocks per slab
 *
 * @return The hint shift
 **/
static inline uint8_t __must_check
vdo_get_slab_summary_hint_shift(unsigned int slab_size_shift)
{
	return ((slab_size_shift > VDO_SLAB_SUMMARY_FULLNESS_HINT_BITS) ?
		(slab_size_shift - VDO_SLAB_SUMMARY_FULLNESS_HINT_BITS) : 0);
}

#endif /* SLAB_SUMMARY_FORMAT_H */
