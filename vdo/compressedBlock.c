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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/compressedBlock.c#11 $
 */

#include "compressedBlock.h"

#include "memoryAlloc.h"
#include "numeric.h"

static const struct version_number COMPRESSED_BLOCK_1_0 = {
	.major_version = 1,
	.minor_version = 0,
};

/**********************************************************************/
void reset_compressed_block_header(compressed_block_header *header)
{
	STATIC_ASSERT(sizeof(header->fields) == sizeof(header->raw));

	header->fields.version = pack_version_number(COMPRESSED_BLOCK_1_0);
	memset(header->fields.sizes, 0, sizeof(header->fields.sizes));
}

/**********************************************************************/
static uint16_t
get_compressed_fragment_size(const compressed_block_header *header,
			     byte slot)
{
	return get_unaligned_le16(header->fields.sizes[slot]);
}

/**********************************************************************/
int get_compressed_block_fragment(BlockMappingState mapping_state,
				  char *buffer,
				  block_size_t block_size,
				  uint16_t *fragment_offset,
				  uint16_t *fragment_size)
{
	if (!is_compressed(mapping_state)) {
		return VDO_INVALID_FRAGMENT;
	}

	compressed_block_header *header = (compressed_block_header *) buffer;
	struct version_number version =
		unpack_version_number(header->fields.version);
	if (!are_same_version(version, COMPRESSED_BLOCK_1_0)) {
		return VDO_INVALID_FRAGMENT;
	}

	byte slot = get_slot_from_state(mapping_state);
	if (slot >= MAX_COMPRESSION_SLOTS) {
		return VDO_INVALID_FRAGMENT;
	}

	uint16_t compressed_size = get_compressed_fragment_size(header, slot);
	uint16_t offset = sizeof(compressed_block_header);
	unsigned int i;
	for (i = 0; i < slot; i++) {
		offset += get_compressed_fragment_size(header, i);
		if (offset >= block_size) {
			return VDO_INVALID_FRAGMENT;
		}
	}

	if ((offset + compressed_size) > block_size) {
		return VDO_INVALID_FRAGMENT;
	}

	*fragment_offset = offset;
	*fragment_size = compressed_size;
	return VDO_SUCCESS;
}

/**********************************************************************/
void put_compressed_block_fragment(struct compressed_block *block,
				   unsigned int fragment,
				   uint16_t offset,
				   const char *data,
				   uint16_t size)
{
	put_unaligned_le16(size, block->header.fields.sizes[fragment]);
	memcpy(&block->data[offset], data, size);
}
