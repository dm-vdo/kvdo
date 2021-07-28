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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/compressedBlock.c#3 $
 */

#include "compressedBlock.h"

#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "statusCodes.h"

static const struct version_number COMPRESSED_BLOCK_1_0 = {
	.major_version = 1,
	.minor_version = 0,
};

enum {
	COMPRESSED_BLOCK_1_0_SIZE = 4 + 4 + (2 * VDO_MAX_COMPRESSION_SLOTS),
};

/**********************************************************************/
void reset_vdo_compressed_block_header(struct compressed_block_header *header)
{
	// Make sure the block layout isn't accidentally changed by changing
	// the length of the block header.
	STATIC_ASSERT_SIZEOF(struct compressed_block_header,
			     COMPRESSED_BLOCK_1_0_SIZE);

	header->version = pack_vdo_version_number(COMPRESSED_BLOCK_1_0);
	memset(header->sizes, 0, sizeof(header->sizes));
}

/**********************************************************************/
static uint16_t
get_compressed_fragment_size(const struct compressed_block_header *header,
			     byte slot)
{
	return __le16_to_cpu(header->sizes[slot]);
}

/**********************************************************************/
int get_vdo_compressed_block_fragment(enum block_mapping_state mapping_state,
				      char *buffer,
				      block_size_t block_size,
				      uint16_t *fragment_offset,
				      uint16_t *fragment_size)
{
	uint16_t compressed_size, offset;
	unsigned int i;
	byte slot;
	struct version_number version;
	struct compressed_block_header *header =
		(struct compressed_block_header *) buffer;

	if (!vdo_is_state_compressed(mapping_state)) {
		return VDO_INVALID_FRAGMENT;
	}

	version = unpack_vdo_version_number(header->version);
	if (!are_same_vdo_version(version, COMPRESSED_BLOCK_1_0)) {
		return VDO_INVALID_FRAGMENT;
	}

	slot = vdo_get_slot_from_state(mapping_state);
	if (slot >= VDO_MAX_COMPRESSION_SLOTS) {
		return VDO_INVALID_FRAGMENT;
	}

	compressed_size = get_compressed_fragment_size(header, slot);
	offset = sizeof(struct compressed_block_header);
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
void put_vdo_compressed_block_fragment(struct compressed_block *block,
				       unsigned int fragment,
				       uint16_t offset,
				       const char *data,
				       uint16_t size)
{
	block->header.sizes[fragment] = __cpu_to_le16(size);
	memcpy(&block->data[offset], data, size);
}
