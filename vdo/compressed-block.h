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

/**
 * Initializes/resets a compressed block header.
 *
 * @param header        the header
 *
 * When done, the version number is set to the current version, and all
 * fragments are empty.
 **/
void reset_vdo_compressed_block_header(struct compressed_block_header *header);

/**
 * Get a reference to a compressed fragment from a compression block.
 *
 * @param [in]  mapping_state    the mapping state for the look up
 * @param [in]  buffer           buffer that contains compressed data
 * @param [in]  block_size       size of a data block
 * @param [out] fragment_offset  the offset of the fragment within a
 *                               compressed block
 * @param [out] fragment_size    the size of the fragment
 *
 * @return If a valid compressed fragment is found, VDO_SUCCESS;
 *         otherwise, VDO_INVALID_FRAGMENT if the fragment is invalid.
 **/
int get_vdo_compressed_block_fragment(enum block_mapping_state mapping_state,
				      char *buffer,
				      block_size_t block_size,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size);

/**
 * Copy a fragment into the compressed block.
 *
 * @param block      the compressed block
 * @param fragment   the number of the fragment
 * @param offset     the byte offset of the fragment in the data area
 * @param data       a pointer to the compressed data
 * @param size       the size of the data
 *
 * @note no bounds checking -- the data better fit without smashing other stuff
 **/
void put_vdo_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment, uint16_t offset,
				       const char *data, uint16_t size);

#endif // COMPRESSED_BLOCK_H
