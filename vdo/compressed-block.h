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

/**
 * The compressed block overlay.
 **/
struct compressed_block {
	struct compressed_block_header header;
	char data[];
} __packed;

void reset_vdo_compressed_block_header(struct compressed_block_header *header);

int get_vdo_compressed_block_fragment(enum block_mapping_state mapping_state,
				      char *buffer,
				      block_size_t block_size,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size);

void put_vdo_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment, uint16_t offset,
				       const char *data, uint16_t size);

#endif /* COMPRESSED_BLOCK_H */
