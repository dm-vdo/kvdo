/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMPRESSED_BLOCK_H
#define COMPRESSED_BLOCK_H

#include "compiler.h"

#include "block-mapping-state.h"
#include "constants.h"
#include "header.h"
#include "types.h"

/*
 * The header of a compressed block.
 */
struct compressed_block_header {
	/*
	 * Unsigned 32-bit major and minor versions,
	 * in little-endian byte order
	 */
	struct packed_version_number version;

	/*
	 * List of unsigned 16-bit compressed block sizes,
	 * in little-endian order
	 */
	__le16 sizes[VDO_MAX_COMPRESSION_SLOTS];
} __packed;

enum {
	VDO_COMPRESSED_BLOCK_DATA_SIZE =
		VDO_BLOCK_SIZE - sizeof(struct compressed_block_header),

	/*
	 * A compressed block is only written if we can pack at least two
	 * fragments into it, so a fragment which fills the entire data portion
	 * of a compressed block is too big.
	 */
	VDO_MAX_COMPRESSED_FRAGMENT_SIZE = VDO_COMPRESSED_BLOCK_DATA_SIZE - 1,
};

/*
 * The compressed block overlay.
 */
struct compressed_block {
	struct compressed_block_header header;
	char data[VDO_COMPRESSED_BLOCK_DATA_SIZE];
} __packed;

int vdo_get_compressed_block_fragment(enum block_mapping_state mapping_state,
				      struct compressed_block *block,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size);

void vdo_initialize_compressed_block(struct compressed_block *block,
				     uint16_t size);

static inline void
vdo_clear_unused_compression_slots(struct compressed_block *block,
				   slot_number_t first_unused)
{
	if (first_unused < VDO_MAX_COMPRESSION_SLOTS) {
		memset(&block->header.sizes[first_unused],
		       0,
		       ((VDO_MAX_COMPRESSION_SLOTS - first_unused) *
			sizeof(__le16)));
	}
}

void vdo_put_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment,
				       uint16_t offset,
				       const char *data,
				       uint16_t size);

#endif /* COMPRESSED_BLOCK_H */
