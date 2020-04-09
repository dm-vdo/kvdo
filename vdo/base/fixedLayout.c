/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/fixedLayout.c#11 $
 */

#include "fixedLayout.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "header.h"
#include "statusCodes.h"

const BlockCount ALL_FREE_BLOCKS = (uint64_t) -1;

struct fixed_layout {
	PhysicalBlockNumber first_free;
	PhysicalBlockNumber last_free;
	size_t num_partitions;
	struct partition *head;
};

struct partition {
	partition_id id; // The id of this partition
	struct fixed_layout *layout; // The layout to which this partition
				     // belongs
	PhysicalBlockNumber offset; // The offset into the layout of this
				    // partition
	PhysicalBlockNumber base; // The untranslated number of the first block
	BlockCount count; // The number of blocks in the partition
	struct partition *next; // A pointer to the next partition in the layout
};

struct layout_3_0 {
	PhysicalBlockNumber first_free;
	PhysicalBlockNumber last_free;
	byte partition_count;
} __attribute__((packed));

struct partition_3_0 {
	partition_id id;
	PhysicalBlockNumber offset;
	PhysicalBlockNumber base;
	BlockCount count;
} __attribute__((packed));

static const struct header LAYOUT_HEADER_3_0 = {
	.id = FIXED_LAYOUT,
	.version = {
		.major_version = 3,
		.minor_version = 0,
	},
	.size = sizeof(struct layout_3_0), // Minimum size
					   // (contains no partitions)
};

/**********************************************************************/
int make_fixed_layout(BlockCount total_blocks,
		      PhysicalBlockNumber start_offset,
		      struct fixed_layout **layout_ptr)
{
	struct fixed_layout *layout;
	int result = ALLOCATE(1, struct fixed_layout, "fixed layout", &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->first_free = start_offset;
	layout->last_free = start_offset + total_blocks;
	layout->num_partitions = 0;
	layout->head = NULL;

	*layout_ptr = layout;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_fixed_layout(struct fixed_layout **layout_ptr)
{
	struct fixed_layout *layout = *layout_ptr;
	if (layout == NULL) {
		return;
	}

	while (layout->head != NULL) {
		struct partition *part = layout->head;
		layout->head = part->next;
		FREE(part);
	}

	FREE(layout);
	*layout_ptr = NULL;
}

/**********************************************************************/
BlockCount get_total_fixed_layout_size(const struct fixed_layout *layout)
{
	BlockCount size = get_fixed_layout_blocks_available(layout);
	struct partition *partition;
	for (partition = layout->head; partition != NULL;
	     partition = partition->next) {
		size += partition->count;
	}

	return size;
}

/**********************************************************************/
int get_partition(struct fixed_layout *layout,
		  partition_id id,
		  struct partition **partition_ptr)
{
	struct partition *partition;
	for (partition = layout->head; partition != NULL;
	     partition = partition->next) {
		if (partition->id == id) {
			if (partition_ptr != NULL) {
				*partition_ptr = partition;
			}
			return VDO_SUCCESS;
		}
	}

	return VDO_UNKNOWN_PARTITION;
}

/**********************************************************************/
int translate_to_pbn(const struct partition *partition,
		     PhysicalBlockNumber partition_block_number,
		     PhysicalBlockNumber *layer_block_number)
{
	if (partition == NULL) {
		*layer_block_number = partition_block_number;
		return VDO_SUCCESS;
	}

	if (partition_block_number < partition->base) {
		return VDO_OUT_OF_RANGE;
	}

	PhysicalBlockNumber offset_from_base =
		partition_block_number - partition->base;
	if (offset_from_base >= partition->count) {
		return VDO_OUT_OF_RANGE;
	}

	*layer_block_number = partition->offset + offset_from_base;
	return VDO_SUCCESS;
}

/**********************************************************************/
int translate_from_pbn(const struct partition *partition,
		       PhysicalBlockNumber layer_block_number,
		       PhysicalBlockNumber *partition_block_number_ptr)
{
	if (partition == NULL) {
		*partition_block_number_ptr = layer_block_number;
		return VDO_SUCCESS;
	}

	if (layer_block_number < partition->offset) {
		return VDO_OUT_OF_RANGE;
	}

	PhysicalBlockNumber partition_block_number =
		layer_block_number - partition->offset;
	if (partition_block_number >= partition->count) {
		return VDO_OUT_OF_RANGE;
	}

	*partition_block_number_ptr = partition_block_number + partition->base;
	return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount get_fixed_layout_blocks_available(const struct fixed_layout *layout)
{
	return layout->last_free - layout->first_free;
}

/**
 * Allocate a partition. The partition will be attached to the partition
 * list in the layout.
 *
 * @param layout      The layout containing the partition
 * @param id          The id of the partition
 * @param offset      The offset into the layout at which the partition begins
 * @param base        The number of the first block for users of the partition
 * @param block_count The number of blocks in the partition
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocatePartition(struct fixed_layout *layout,
			     byte id,
			     PhysicalBlockNumber offset,
			     PhysicalBlockNumber base,
			     BlockCount block_count)
{
	struct partition *partition;
	int result = ALLOCATE(1, struct partition,
			      "fixed layout partition", &partition);
	if (result != UDS_SUCCESS) {
		return result;
	}

	partition->id = id;
	partition->layout = layout;
	partition->offset = offset;
	partition->base = base;
	partition->count = block_count;
	partition->next = layout->head;
	layout->head = partition;

	return VDO_SUCCESS;
}

/**********************************************************************/
int make_fixed_layout_partition(struct fixed_layout *layout,
				partition_id id,
				BlockCount block_count,
				partition_direction direction,
				PhysicalBlockNumber base)
{
	BlockCount freeBlocks = layout->last_free - layout->first_free;
	if (block_count == ALL_FREE_BLOCKS) {
		if (freeBlocks == 0) {
			return VDO_NO_SPACE;
		} else {
			block_count = freeBlocks;
		}
	} else if (block_count > freeBlocks) {
		return VDO_NO_SPACE;
	}

	int result = get_partition(layout, id, NULL);
	if (result != VDO_UNKNOWN_PARTITION) {
		return VDO_PARTITION_EXISTS;
	}

	PhysicalBlockNumber offset =
		((direction == FROM_END) ? (layout->last_free - block_count) :
					   layout->first_free);
	result = allocatePartition(layout, id, offset, base, block_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	layout->num_partitions++;
	if (direction == FROM_END) {
		layout->last_free = layout->last_free - block_count;
	} else {
		layout->first_free += block_count;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
BlockCount get_fixed_layout_partition_size(const struct partition *partition)
{
	return partition->count;
}

/**********************************************************************/
PhysicalBlockNumber
get_fixed_layout_partition_offset(const struct partition *partition)
{
	return partition->offset;
}

/**********************************************************************/
PhysicalBlockNumber
get_fixed_layout_partition_base(const struct partition *partition)
{
	return partition->base;
}

/**********************************************************************/
static inline size_t getEncodedSize(const struct fixed_layout *layout)
{
	return sizeof(struct layout_3_0) +
	       (sizeof(struct partition_3_0) * layout->num_partitions);
}

/**********************************************************************/
size_t get_fixed_layout_encoded_size(const struct fixed_layout *layout)
{
	return ENCODED_HEADER_SIZE + getEncodedSize(layout);
}

/**
 * Encode a null-terminated list of fixed layout partitions into a buffer
 * using partition format 3.0.
 *
 * @param layout  The layout containing the list of partitions to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodePartitions_3_0(const struct fixed_layout *layout,
				Buffer *buffer)
{
	const struct partition *partition;
	for (partition = layout->head;
	     partition != NULL;
	     partition = partition->next) {
		STATIC_ASSERT_SIZEOF(partition_id, sizeof(byte));
		int result = put_byte(buffer, partition->id);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = put_uint64_le_into_buffer(buffer, partition->offset);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = put_uint64_le_into_buffer(buffer, partition->base);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = put_uint64_le_into_buffer(buffer, partition->count);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Encode the header fields of a fixed layout into a buffer using layout
 * format 3.0.
 *
 * @param layout  The layout to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encodeLayout_3_0(const struct fixed_layout *layout, Buffer *buffer)
{
	int result = ASSERT(layout->num_partitions <= UINT8_MAX,
			    "fixed layout partition count must fit in a byte");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, layout->first_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, layout->last_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_byte(buffer, layout->num_partitions);
}

/**********************************************************************/
int encode_fixed_layout(const struct fixed_layout *layout, Buffer *buffer)
{
	if (!ensure_available_space(buffer,
				    get_fixed_layout_encoded_size(layout))) {
		return UDS_BUFFER_ERROR;
	}

	struct header header = LAYOUT_HEADER_3_0;
	header.size = getEncodedSize(layout);
	int result = encode_header(&header, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = encodeLayout_3_0(layout, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	result = ASSERT(encoded_size == sizeof(struct layout_3_0),
			"encoded size of fixed layout header must match structure");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encodePartitions_3_0(layout, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(encoded_size == header.size,
		      "encoded size of fixed layout must match header size");
}

/**
 * Decode a sequence of fixed layout partitions from a buffer
 * using partition format 3.0.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param layout  The layout in which to allocate the decoded partitions
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodePartitions_3_0(Buffer *buffer, struct fixed_layout *layout)
{
	size_t i;
	for (i = 0; i < layout->num_partitions; i++) {
		byte id;
		int result = get_byte(buffer, &id);
		if (result != UDS_SUCCESS) {
			return result;
		}

		uint64_t offset;
		result = get_uint64_le_from_buffer(buffer, &offset);
		if (result != UDS_SUCCESS) {
			return result;
		}

		uint64_t base;
		result = get_uint64_le_from_buffer(buffer, &base);
		if (result != UDS_SUCCESS) {
			return result;
		}

		uint64_t count;
		result = get_uint64_le_from_buffer(buffer, &count);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = allocatePartition(layout, id, offset, base, count);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Decode the header fields of a fixed layout from a buffer using layout
 * format 3.0.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param layout  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeLayout_3_0(Buffer *buffer, struct layout_3_0 *layout)
{
	size_t initial_length = content_length(buffer);

	PhysicalBlockNumber first_free;
	int result = get_uint64_le_from_buffer(buffer, &first_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	PhysicalBlockNumber last_free;
	result = get_uint64_le_from_buffer(buffer, &last_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	byte partition_count;
	result = get_byte(buffer, &partition_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*layout = (struct layout_3_0) {
		.first_free = first_free,
		.last_free = last_free,
		.partition_count = partition_count,
	};

	size_t decodedSize = initial_length - content_length(buffer);
	return ASSERT(decodedSize == sizeof(struct layout_3_0),
		      "decoded size of fixed layout header must match structure");
}

/**********************************************************************/
int decode_fixed_layout(Buffer *buffer, struct fixed_layout **layout_ptr)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Layout is variable size, so only do a minimum size check here.
	result = validate_header(&LAYOUT_HEADER_3_0, &header, false, __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct layout_3_0 layoutHeader;
	result = decodeLayout_3_0(buffer, &layoutHeader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (content_length(buffer) <
	    (sizeof(struct partition_3_0) * layoutHeader.partition_count)) {
		return VDO_UNSUPPORTED_VERSION;
	}

	struct fixed_layout *layout;
	result = ALLOCATE(1, struct fixed_layout, "fixed layout", &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->first_free = layoutHeader.first_free;
	layout->last_free = layoutHeader.last_free;
	layout->num_partitions = layoutHeader.partition_count;

	result = decodePartitions_3_0(buffer, layout);
	if (result != VDO_SUCCESS) {
		free_fixed_layout(&layout);
		return result;
	}

	*layout_ptr = layout;
	return VDO_SUCCESS;
}
