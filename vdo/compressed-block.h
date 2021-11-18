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
 */

#ifndef COMPRESSED_BLOCK_H
#define COMPRESSED_BLOCK_H

#include "compiler.h"

#include "block-mapping-state.h"
#include "constants.h"
#include "header.h"
#include "types.h"

/**
 * The header of a compressed block.
 **/
struct compressed_block_header {
	/**
	 * Unsigned 32-bit major and minor versions,
	 * in little-endian byte order
	 */
	struct packed_version_number version;

	/**
	 * List of unsigned 16-bit compressed block sizes,
	 * in little-endian order
	 */
	__le16 sizes[VDO_MAX_COMPRESSION_SLOTS];
} __packed;

enum {
	/*
	 * A compressed block is only written if we can pack at least two
	 * fragments into it, so a fragment which fills the entire data portion
	 * of a compressed block is too big.
	 */
	VDO_MAX_COMPRESSED_FRAGMENT_SIZE =
		VDO_BLOCK_SIZE - sizeof(struct compressed_block_header) - 1,
};

/**
 * The compressed block overlay.
 **/
struct compressed_block {
	struct compressed_block_header header;
	char data[];
} __packed;

int vdo_get_compressed_block_fragment(enum block_mapping_state mapping_state,
				      char *buffer,
				      block_size_t block_size,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size);

void vdo_initialize_compressed_block(struct compressed_block *block,
				     uint16_t size);

void vdo_put_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment,
				       uint16_t offset,
				       const char *data,
				       uint16_t size);

#endif /* COMPRESSED_BLOCK_H */
