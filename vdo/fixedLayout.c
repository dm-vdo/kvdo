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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/fixedLayout.c#25 $
 */

#include "fixedLayout.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "header.h"
#include "statusCodes.h"

const block_count_t VDO_ALL_FREE_BLOCKS = (uint64_t) -1;

struct fixed_layout {
	physical_block_number_t first_free;
	physical_block_number_t last_free;
	size_t num_partitions;
	struct partition *head;
};

struct partition {
	enum partition_id id; // The id of this partition
	struct fixed_layout *layout; // The layout to which this partition
				     // belongs
	physical_block_number_t offset; // The offset into the layout of this
				    // partition
	physical_block_number_t base; // The untranslated number of the first block
	block_count_t count; // The number of blocks in the partition
	struct partition *next; // A pointer to the next partition in the layout
};

struct layout_3_0 {
	physical_block_number_t first_free;
	physical_block_number_t last_free;
	byte partition_count;
} __packed;

struct partition_3_0 {
	enum partition_id id;
	physical_block_number_t offset;
	physical_block_number_t base;
	block_count_t count;
} __packed;

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
int make_vdo_fixed_layout(block_count_t total_blocks,
			  physical_block_number_t start_offset,
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
void free_vdo_fixed_layout(struct fixed_layout **layout_ptr)
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
block_count_t get_total_vdo_fixed_layout_size(const struct fixed_layout *layout)
{
	block_count_t size = get_vdo_fixed_layout_blocks_available(layout);
	struct partition *partition;
	for (partition = layout->head; partition != NULL;
	     partition = partition->next) {
		size += partition->count;
	}

	return size;
}

/**********************************************************************/
int vdo_get_partition(struct fixed_layout *layout,
		      enum partition_id id,
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
int vdo_translate_to_pbn(const struct partition *partition,
			 physical_block_number_t partition_block_number,
			 physical_block_number_t *layer_block_number)
{
	physical_block_number_t offset_from_base;
	if (partition == NULL) {
		*layer_block_number = partition_block_number;
		return VDO_SUCCESS;
	}

	if (partition_block_number < partition->base) {
		return VDO_OUT_OF_RANGE;
	}

	offset_from_base = partition_block_number - partition->base;
	if (offset_from_base >= partition->count) {
		return VDO_OUT_OF_RANGE;
	}

	*layer_block_number = partition->offset + offset_from_base;
	return VDO_SUCCESS;
}

/**********************************************************************/
int vdo_translate_from_pbn(const struct partition *partition,
			   physical_block_number_t layer_block_number,
			   physical_block_number_t *partition_block_number_ptr)
{
	physical_block_number_t partition_block_number;

	if (partition == NULL) {
		*partition_block_number_ptr = layer_block_number;
		return VDO_SUCCESS;
	}

	if (layer_block_number < partition->offset) {
		return VDO_OUT_OF_RANGE;
	}

	partition_block_number = layer_block_number - partition->offset;
	if (partition_block_number >= partition->count) {
		return VDO_OUT_OF_RANGE;
	}

	*partition_block_number_ptr = partition_block_number + partition->base;
	return VDO_SUCCESS;
}

/**********************************************************************/
block_count_t
get_vdo_fixed_layout_blocks_available(const struct fixed_layout *layout)
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
static int allocate_partition(struct fixed_layout *layout,
			      byte id,
			      physical_block_number_t offset,
			      physical_block_number_t base,
			      block_count_t block_count)
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
int make_vdo_fixed_layout_partition(struct fixed_layout *layout,
				    enum partition_id id,
				    block_count_t block_count,
				    enum partition_direction direction,
				    physical_block_number_t base)
{
	int result;
	physical_block_number_t offset;

	block_count_t free_blocks = layout->last_free - layout->first_free;
	if (block_count == VDO_ALL_FREE_BLOCKS) {
		if (free_blocks == 0) {
			return VDO_NO_SPACE;
		} else {
			block_count = free_blocks;
		}
	} else if (block_count > free_blocks) {
		return VDO_NO_SPACE;
	}

	result = vdo_get_partition(layout, id, NULL);
	if (result != VDO_UNKNOWN_PARTITION) {
		return VDO_PARTITION_EXISTS;
	}

	offset = ((direction == FROM_END) ? (layout->last_free - block_count) :
					   layout->first_free);
	result = allocate_partition(layout, id, offset, base, block_count);
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
block_count_t
get_vdo_fixed_layout_partition_size(const struct partition *partition)
{
	return partition->count;
}

/**********************************************************************/
physical_block_number_t
get_vdo_fixed_layout_partition_offset(const struct partition *partition)
{
	return partition->offset;
}

/**********************************************************************/
physical_block_number_t
get_vdo_fixed_layout_partition_base(const struct partition *partition)
{
	return partition->base;
}

/**********************************************************************/
static inline size_t get_encoded_size(const struct fixed_layout *layout)
{
	return sizeof(struct layout_3_0) +
	       (sizeof(struct partition_3_0) * layout->num_partitions);
}

/**********************************************************************/
size_t get_vdo_fixed_layout_encoded_size(const struct fixed_layout *layout)
{
	return ENCODED_HEADER_SIZE + get_encoded_size(layout);
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
static int encode_partitions_3_0(const struct fixed_layout *layout,
				 struct buffer *buffer)
{
	const struct partition *partition;
	for (partition = layout->head;
	     partition != NULL;
	     partition = partition->next) {
		int result;
		STATIC_ASSERT_SIZEOF(enum partition_id, sizeof(byte));
		result = put_byte(buffer, partition->id);
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
static int encode_layout_3_0(const struct fixed_layout *layout,
			     struct buffer *buffer)
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
int encode_vdo_fixed_layout(const struct fixed_layout *layout,
			    struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	struct header header = LAYOUT_HEADER_3_0;

	if (!ensure_available_space(buffer,
				    get_vdo_fixed_layout_encoded_size(layout))) {
		return UDS_BUFFER_ERROR;
	}

	header.size = get_encoded_size(layout);
	result = encode_vdo_header(&header, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = encode_layout_3_0(layout, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	encoded_size = content_length(buffer) - initial_length;
	result = ASSERT(encoded_size == sizeof(struct layout_3_0),
			"encoded size of fixed layout header must match structure");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_partitions_3_0(layout, buffer);
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
static int decode_partitions_3_0(struct buffer *buffer,
				 struct fixed_layout *layout)
{
	size_t i;
	for (i = 0; i < layout->num_partitions; i++) {
		byte id;
		uint64_t offset, base, count;
		int result = get_byte(buffer, &id);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = get_uint64_le_from_buffer(buffer, &offset);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = get_uint64_le_from_buffer(buffer, &base);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = get_uint64_le_from_buffer(buffer, &count);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = allocate_partition(layout, id, offset, base, count);
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
static int decode_layout_3_0(struct buffer *buffer, struct layout_3_0 *layout)
{
	size_t decoded_size, initial_length = content_length(buffer);
	physical_block_number_t first_free, last_free;
	byte partition_count;

	int result = get_uint64_le_from_buffer(buffer, &first_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &last_free);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_byte(buffer, &partition_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*layout = (struct layout_3_0) {
		.first_free = first_free,
		.last_free = last_free,
		.partition_count = partition_count,
	};

	decoded_size = initial_length - content_length(buffer);
	return ASSERT(decoded_size == sizeof(struct layout_3_0),
		      "decoded size of fixed layout header must match structure");
}

/**********************************************************************/
int decode_vdo_fixed_layout(struct buffer *buffer,
			    struct fixed_layout **layout_ptr)
{
	struct header header;
	struct layout_3_0 layout_header;
	struct fixed_layout *layout;

	int result = decode_vdo_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Layout is variable size, so only do a minimum size check here.
	result = validate_vdo_header(&LAYOUT_HEADER_3_0, &header, false, __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_layout_3_0(buffer, &layout_header);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (content_length(buffer) <
	    (sizeof(struct partition_3_0) * layout_header.partition_count)) {
		return VDO_UNSUPPORTED_VERSION;
	}

	result = ALLOCATE(1, struct fixed_layout, "fixed layout", &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->first_free = layout_header.first_free;
	layout->last_free = layout_header.last_free;
	layout->num_partitions = layout_header.partition_count;

	result = decode_partitions_3_0(buffer, layout);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&layout);
		return result;
	}

	*layout_ptr = layout;
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_partitioned_vdo_fixed_layout(block_count_t physical_blocks,
				      physical_block_number_t starting_offset,
				      block_count_t block_map_blocks,
				      block_count_t journal_blocks,
				      block_count_t summary_blocks,
				      struct fixed_layout **layout_ptr)
{
	struct fixed_layout *layout;
	int result;

	block_count_t necessary_size = (starting_offset + block_map_blocks +
					journal_blocks + summary_blocks);
	if (necessary_size > physical_blocks) {
		return uds_log_error_strerror(VDO_NO_SPACE,
					      "Not enough space to make a VDO");
	}

	result = make_vdo_fixed_layout(physical_blocks - starting_offset,
				       starting_offset,
				       &layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_vdo_fixed_layout_partition(layout,
						 BLOCK_MAP_PARTITION,
						 block_map_blocks,
						 FROM_BEGINNING,
						 0);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&layout);
		return result;
	}

	result = make_vdo_fixed_layout_partition(layout, SLAB_SUMMARY_PARTITION,
						 summary_blocks, FROM_END, 0);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&layout);
		return result;
	}

	result = make_vdo_fixed_layout_partition(layout,
						 RECOVERY_JOURNAL_PARTITION,
						 journal_blocks, FROM_END, 0);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&layout);
		return result;
	}

	/*
	 * The block allocator no longer traffics in relative PBNs so the offset
	 * doesn't matter. We need to keep this partition around both for
	 * upgraded systems, and because we decided that all of the usable space
	 * in the volume, other than the super block, should be part of some
	 * partition.
	 */
	result = make_vdo_fixed_layout_partition(layout,
						 BLOCK_ALLOCATOR_PARTITION,
						 VDO_ALL_FREE_BLOCKS,
						 FROM_BEGINNING,
						 block_map_blocks);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&layout);
		return result;
	}

	*layout_ptr = layout;
	return VDO_SUCCESS;
}
