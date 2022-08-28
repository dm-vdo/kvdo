// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-layout.h"

#include <linux/dm-kcopyd.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "status-codes.h"
#include "types.h"

#include "vdo.h"

const block_count_t VDO_ALL_FREE_BLOCKS = (uint64_t) -1;

struct fixed_layout {
	physical_block_number_t first_free;
	physical_block_number_t last_free;
	size_t num_partitions;
	struct partition *head;
};

struct partition {
	enum partition_id id; /* The id of this partition */
	struct fixed_layout *layout; /* The layout to which this partition */
				     /* belongs */
	physical_block_number_t offset; /* The offset into the layout of this */
				    /* partition */
	physical_block_number_t base; /* The untranslated number of the first block */
	block_count_t count; /* The number of blocks in the partition */
	struct partition *next; /* A pointer to the next partition in the layout */
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
	.id = VDO_FIXED_LAYOUT,
	.version = {
		.major_version = 3,
		.minor_version = 0,
	},
	.size = sizeof(struct layout_3_0), /* Minimum size */
					   /* (contains no partitions) */
};

/**
 * vdo_make_fixed_layout() - Make an unpartitioned fixed layout.
 * @total_blocks: The total size of the layout, in blocks.
 * @start_offset: The block offset in the underlying layer at which the fixed
 *                layout begins.
 * @layout_ptr: The pointer to hold the resulting layout.
 *
 * Return: A success or error code.
 */
int vdo_make_fixed_layout(block_count_t total_blocks,
			  physical_block_number_t start_offset,
			  struct fixed_layout **layout_ptr)
{
	struct fixed_layout *layout;
	int result = UDS_ALLOCATE(1, struct fixed_layout, "fixed layout", &layout);

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

/**
 * vdo_free_fixed_layout() - Free a fixed layout.
 * @layout: The layout to free.
 *
 * All partitions created by this layout become invalid pointers.
 */
void vdo_free_fixed_layout(struct fixed_layout *layout)
{
	if (layout == NULL) {
		return;
	}

	while (layout->head != NULL) {
		struct partition *part = layout->head;

		layout->head = part->next;
		UDS_FREE(part);
	}

	UDS_FREE(layout);
}

/**
 * vdo_get_total_fixed_layout_size() - Get the total size of the layout in
 *                                     blocks.
 * @layout: The layout.
 *
 * Return: The size of the layout.
 */
block_count_t vdo_get_total_fixed_layout_size(const struct fixed_layout *layout)
{
	block_count_t size = vdo_get_fixed_layout_blocks_available(layout);
	struct partition *partition;

	for (partition = layout->head; partition != NULL;
	     partition = partition->next) {
		size += partition->count;
	}

	return size;
}

/**
 * vdo_get_fixed_layout_partition() - Get a partition by id.
 * @layout: The layout from which to get a partition.
 * @id: The id of the partition.
 * @partition_ptr: A pointer to hold the partition.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_get_fixed_layout_partition(struct fixed_layout *layout,
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

/**
 * vdo_translate_to_pbn() - Translate a block number from the partition's view
 *                          to the layer's
 * @partition: The partition to use for translation.
 * @partition_block_number: The block number relative to the partition.
 * @layer_block_number: The block number relative to the layer.
 *
 * Return: VDO_SUCCESS or an error code.
 */
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

/**
 * vdo_translate_from_pbn() - Translate a block number from the layer's view
 *                            to the partition's.
 * @partition: The partition to use for translation.
 * @layer_block_number: The block number relative to the layer.
 * @partition_block_number: The block number relative to the partition.
 *
 * This is the inverse of vdo_translate_to_pbn().
 *
 * Return: VDO_SUCCESS or an error code.
 */
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

/**
 * vdo_get_fixed_layout_blocks_available() - Return the number of unallocated
 *                                           blocks available.
 * @layout: The fixed layout.
 *
 * Return: The number of blocks yet unallocated to partitions.
 */
block_count_t
vdo_get_fixed_layout_blocks_available(const struct fixed_layout *layout)
{
	return layout->last_free - layout->first_free;
}

/**
 * allocate_partition() - Allocate a partition.
 * @layout: The layout containing the partition.
 * @id: The id of the partition.
 * @offset: The offset into the layout at which the partition begins.
 * @base: The number of the first block for users of the partition.
 * @block_count: The number of blocks in the partition.
 *
 * The partition will be attached to the partition list in the layout.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int allocate_partition(struct fixed_layout *layout,
			      byte id,
			      physical_block_number_t offset,
			      physical_block_number_t base,
			      block_count_t block_count)
{
	struct partition *partition;
	int result = UDS_ALLOCATE(1, struct partition,
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

/**
 * vdo_make_fixed_layout_partition() - Create a new partition from the
 *                                     beginning or end of the unused space
 *                                     within a fixed layout.
 * @layout: The fixed layout.
 * @id: The id of the partition to make.
 * @block_count: The number of blocks to carve out, if set to
 *               VDO_ALL_FREE_BLOCKS, all remaining blocks will be used.
 * @direction: Whether to carve out from beginning or end.
 * @base: The number of the first block in the partition from the point of
 *        view of its users.
 *
 * Return: A success or error code, particularly VDO_NO_SPACE if there are
 *         less than block_count blocks remaining.
 */
int vdo_make_fixed_layout_partition(struct fixed_layout *layout,
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

	result = vdo_get_fixed_layout_partition(layout, id, NULL);
	if (result != VDO_UNKNOWN_PARTITION) {
		return VDO_PARTITION_EXISTS;
	}

	offset = ((direction == VDO_PARTITION_FROM_END) ?
		  (layout->last_free - block_count) : layout->first_free);
	result = allocate_partition(layout, id, offset, base, block_count);
	if (result != VDO_SUCCESS) {
		return result;
	}

	layout->num_partitions++;
	if (direction == VDO_PARTITION_FROM_END) {
		layout->last_free = layout->last_free - block_count;
	} else {
		layout->first_free += block_count;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_get_fixed_layout_partition_size() - Return the size in blocks of a
 *                                         partition.
 * @partition: A partition of the fixed_layout.
 *
 * Return: The size of the partition in blocks.
 */
block_count_t
vdo_get_fixed_layout_partition_size(const struct partition *partition)
{
	return partition->count;
}

/**
 * vdo_get_fixed_layout_partition_offset() - Get the first block of the
 *                                           partition in the layout.
 * @partition: A partition of the fixed_layout.
 *
 * Return: The partition's offset in blocks.
 */
physical_block_number_t
vdo_get_fixed_layout_partition_offset(const struct partition *partition)
{
	return partition->offset;
}

/**
 * vdo_get_fixed_layout_partition_base() - Get the number of the first block
 *                                         in the partition from the partition
 *                                         user's point of view.
 * @partition: A partition of the fixed_layout.
 *
 * Return: The number of the first block in the partition.
 */
physical_block_number_t
vdo_get_fixed_layout_partition_base(const struct partition *partition)
{
	return partition->base;
}

/**
 * get_encoded_size() - Get the size of an encoded layout
 * @layout: The layout.
 *
 * Return: The encoded size of the layout.
 */
static inline size_t get_encoded_size(const struct fixed_layout *layout)
{
	return sizeof(struct layout_3_0) +
	       (sizeof(struct partition_3_0) * layout->num_partitions);
}

size_t vdo_get_fixed_layout_encoded_size(const struct fixed_layout *layout)
{
	return VDO_ENCODED_HEADER_SIZE + get_encoded_size(layout);
}

/**
 * encode_partitions_3_0() - Encode a null-terminated list of fixed layout
 *                           partitions into a buffer using partition format
 *                           3.0.
 * @layout: The layout containing the list of partitions to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 *
 * Return: UDS_SUCCESS or an error code.
 */
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
 * encode_layout_3_0() - Encode the header fields of a fixed layout into a
 *                       buffer using layout format 3.0.
 * @layout: The layout to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 *
 * Return: UDS_SUCCESS or an error code.
 */
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

/**
 * vdo_encode_fixed_layout() - Encode a layout into a buffer.
 * @layout: The layout to encode.
 * @buffer: The buffer to encode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_encode_fixed_layout(const struct fixed_layout *layout,
			    struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	struct header header = LAYOUT_HEADER_3_0;

	if (!ensure_available_space(buffer,
				    vdo_get_fixed_layout_encoded_size(layout))) {
		return UDS_BUFFER_ERROR;
	}

	header.size = get_encoded_size(layout);
	result = vdo_encode_header(&header, buffer);
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
 * decode_partitions_3_0() - Decode a sequence of fixed layout partitions from
 *                           a buffer using partition format 3.0.
 * @buffer: A buffer positioned at the start of the encoding.
 * @layout: The layout in which to allocate the decoded partitions.
 *
 * Return: UDS_SUCCESS or an error code.
 */
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
 * decode_layout_3_0() - Decode the header fields of a fixed layout from a
 *                       buffer using layout format 3.0.
 * @buffer: A buffer positioned at the start of the encoding.
 * @layout: The structure to receive the decoded fields.
 *
 * Return: UDS_SUCCESS or an error code.
 */
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

/**
 * vdo_decode_fixed_layout() - Decode a fixed layout from a buffer.
 * @buffer: The buffer from which to decode.
 * @layout_ptr: A pointer to hold the layout.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_fixed_layout(struct buffer *buffer,
			    struct fixed_layout **layout_ptr)
{
	struct header header;
	struct layout_3_0 layout_header;
	struct fixed_layout *layout;

	int result = vdo_decode_header(buffer, &header);

	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Layout is variable size, so only do a minimum size check here. */
	result = vdo_validate_header(&LAYOUT_HEADER_3_0, &header, false, __func__);
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

	result = UDS_ALLOCATE(1, struct fixed_layout, "fixed layout", &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	layout->first_free = layout_header.first_free;
	layout->last_free = layout_header.last_free;
	layout->num_partitions = layout_header.partition_count;

	result = decode_partitions_3_0(buffer, layout);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(layout);
		return result;
	}

	*layout_ptr = layout;
	return VDO_SUCCESS;
}

/**
 * vdo_make_partitioned_fixed_layout() - Make a partitioned fixed layout for a
 *                                       VDO.
 * @physical_blocks: The number of physical blocks in the VDO.
 * @starting_offset: The starting offset of the layout.
 * @block_map_blocks: The size of the block map partition.
 * @journal_blocks: The size of the journal partition.
 * @summary_blocks: The size of the slab summary partition.
 * @layout_ptr: A pointer to hold the new fixed_layout.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_partitioned_fixed_layout(block_count_t physical_blocks,
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

	result = vdo_make_fixed_layout(physical_blocks - starting_offset,
				       starting_offset,
				       &layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_fixed_layout_partition(layout,
						 VDO_BLOCK_MAP_PARTITION,
						 block_map_blocks,
						 VDO_PARTITION_FROM_BEGINNING,
						 0);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(layout);
		return result;
	}

	result = vdo_make_fixed_layout_partition(layout,
						 VDO_SLAB_SUMMARY_PARTITION,
						 summary_blocks,
						 VDO_PARTITION_FROM_END, 0);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(layout);
		return result;
	}

	result = vdo_make_fixed_layout_partition(layout,
						 VDO_RECOVERY_JOURNAL_PARTITION,
						 journal_blocks,
						 VDO_PARTITION_FROM_END, 0);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(layout);
		return result;
	}

	/*
	 * The block allocator no longer traffics in relative PBNs so the offset
	 * doesn't matter. We need to keep this partition around both for
	 * upgraded systems, and because we decided that all of the usable space
	 * in the volume, other than the super block, should be part of some
	 * partition.
	 */
	result = vdo_make_fixed_layout_partition(layout,
						 VDO_BLOCK_ALLOCATOR_PARTITION,
						 VDO_ALL_FREE_BLOCKS,
						 VDO_PARTITION_FROM_BEGINNING,
						 block_map_blocks);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(layout);
		return result;
	}

	*layout_ptr = layout;
	return VDO_SUCCESS;
}

/*-----------------------------------------------------------------*/
static const enum partition_id REQUIRED_PARTITIONS[] = {
	VDO_BLOCK_MAP_PARTITION,
	VDO_BLOCK_ALLOCATOR_PARTITION,
	VDO_RECOVERY_JOURNAL_PARTITION,
	VDO_SLAB_SUMMARY_PARTITION,
};

static const uint8_t REQUIRED_PARTITION_COUNT = 4;

/**
 * get_partition_offset() - Get the offset of a given partition.
 * @layout: The layout containing the partition.
 * @id: The ID of the partition whose offset is desired.
 *
 * Return: The offset of the partition (in blocks).
 */
static block_count_t __must_check
get_partition_offset(struct vdo_layout *layout, enum partition_id id)
{
	return vdo_get_fixed_layout_partition_offset(vdo_get_partition(layout,
								       id));
}

/**
 * vdo_decode_layout() - Make a vdo_layout from the fixed_layout decoded from
 *                       the super block.
 * @layout: The fixed_layout from the super block.
 * @vdo_layout_ptr: A pointer to hold the vdo_layout.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_layout(struct fixed_layout *layout,
		      struct vdo_layout **vdo_layout_ptr)
{
	/* Check that all the expected partitions exist */
	struct vdo_layout *vdo_layout;
	struct partition *partition;
	uint8_t i;
	int result;

	for (i = 0; i < REQUIRED_PARTITION_COUNT; i++) {
		result = vdo_get_fixed_layout_partition(layout,
							REQUIRED_PARTITIONS[i],
							&partition);
		if (result != VDO_SUCCESS) {
			return uds_log_error_strerror(result,
						      "VDO layout is missing required partition %u",
						      REQUIRED_PARTITIONS[i]);
		}
	}

	result = UDS_ALLOCATE(1, struct vdo_layout, __func__, &vdo_layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo_layout->layout = layout;

	/* XXX Assert this is the same as where we loaded the super block. */
	vdo_layout->starting_offset =
		get_partition_offset(vdo_layout, VDO_BLOCK_MAP_PARTITION);

	*vdo_layout_ptr = vdo_layout;
	return VDO_SUCCESS;
}

/**
 * vdo_free_layout() - Free a vdo_layout.
 * @vdo_layout: The vdo_layout to free.
 */
void vdo_free_layout(struct vdo_layout *vdo_layout)
{
	if (vdo_layout == NULL) {
		return;
	}

	if (vdo_layout->copier) {
		dm_kcopyd_client_destroy(UDS_FORGET(vdo_layout->copier));
	}
	vdo_free_fixed_layout(UDS_FORGET(vdo_layout->next_layout));
	vdo_free_fixed_layout(UDS_FORGET(vdo_layout->layout));
	vdo_free_fixed_layout(UDS_FORGET(vdo_layout->previous_layout));
	UDS_FREE(vdo_layout);
}

/**
 * retrieve_partition() - Get a partition from a fixed_layout in conditions
 *                        where we expect that it can not fail.
 * @layout: The fixed_layout from which to get the partition.
 * @id: The ID of the partition to retrieve.
 *
 * Return: The desired partition.
 */
static struct partition * __must_check
retrieve_partition(struct fixed_layout *layout, enum partition_id id)
{
	struct partition *partition;
	int result = vdo_get_fixed_layout_partition(layout, id, &partition);

	ASSERT_LOG_ONLY(result == VDO_SUCCESS,
			"vdo_layout has expected partition");
	return partition;
}

/**
 * vdo_get_partition() - Get a partition from a vdo_layout.
 * @vdo_layout: The vdo_layout from which to get the partition.
 * @id: The ID of the desired partition.
 *
 * Because the layout's fixed_layout has already been validated, this can not
 * fail.
 *
 * Return: The requested partition.
 */
struct partition *vdo_get_partition(struct vdo_layout *vdo_layout,
				    enum partition_id id)
{
	return retrieve_partition(vdo_layout->layout, id);
}

/**
 * get_partition_from_next_layout() - Get a partition from a vdo_layout's next
 *                                    fixed_layout.
 * @vdo_layout: The vdo_layout from which to get the partition.
 * @id: The ID of the desired partition.
 *
 * This method should only be called when the vdo_layout is prepared to grow.
 *
 * Return: The requested partition.
 */
static struct partition * __must_check
get_partition_from_next_layout(struct vdo_layout *vdo_layout,
			       enum partition_id id)
{
	ASSERT_LOG_ONLY(vdo_layout->next_layout != NULL,
			"vdo_layout is prepared to grow");
	return retrieve_partition(vdo_layout->next_layout, id);
}

/**
 * get_partition_size() - Get the size of a given partition.
 * @layout: The layout containing the partition.
 * @id: The partition ID whose size to find.
 *
 * Return: The size of the partition (in blocks).
 */
static block_count_t __must_check
get_partition_size(struct vdo_layout *layout, enum partition_id id)
{
	struct partition *partition = vdo_get_partition(layout, id);

	return vdo_get_fixed_layout_partition_size(partition);
}

/**
 * prepare_to_vdo_grow_layout() - Prepare the layout to be grown.
 * @vdo_layout: The layout to grow.
 * @old_physical_blocks: The current size of the VDO.
 * @new_physical_blocks: The size to which the VDO will be grown.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int prepare_to_vdo_grow_layout(struct vdo_layout *vdo_layout,
			       block_count_t old_physical_blocks,
			       block_count_t new_physical_blocks)
{
	int result;
	struct partition *slab_summary_partition, *recovery_journal_partition;
	block_count_t min_new_size;

	if (vdo_get_next_layout_size(vdo_layout) == new_physical_blocks) {
		/*
		 * We are already prepared to grow to the new size, so we're
		 * done.
		 */
		return VDO_SUCCESS;
	}

	/* Make a copy completion if there isn't one */
	if (vdo_layout->copier == NULL) {
		vdo_layout->copier = dm_kcopyd_client_create(NULL);
		if (vdo_layout->copier == NULL) {
			return -ENOMEM;
		}
	}

	/* Free any unused preparation. */
	vdo_free_fixed_layout(UDS_FORGET(vdo_layout->next_layout));

	/*
	 * Make a new layout with the existing partition sizes for everything
	 * but the block allocator partition.
	 */
	result = vdo_make_partitioned_fixed_layout(new_physical_blocks,
						   vdo_layout->starting_offset,
						   get_partition_size(vdo_layout,
								      VDO_BLOCK_MAP_PARTITION),
						   get_partition_size(vdo_layout,
								      VDO_RECOVERY_JOURNAL_PARTITION),
						   get_partition_size(vdo_layout,
								      VDO_SLAB_SUMMARY_PARTITION),
						   &vdo_layout->next_layout);
	if (result != VDO_SUCCESS) {
		dm_kcopyd_client_destroy(UDS_FORGET(vdo_layout->copier));
		return result;
	}

	/*
	 * Ensure the new journal and summary are entirely within the added
	 * blocks.
	 */
	slab_summary_partition =
		get_partition_from_next_layout(vdo_layout,
					       VDO_SLAB_SUMMARY_PARTITION);
	recovery_journal_partition =
		get_partition_from_next_layout(vdo_layout,
					       VDO_RECOVERY_JOURNAL_PARTITION);
	min_new_size =
		(old_physical_blocks +
		 vdo_get_fixed_layout_partition_size(slab_summary_partition) +
		 vdo_get_fixed_layout_partition_size(recovery_journal_partition));
	if (min_new_size > new_physical_blocks) {
		/*
		 * Copying the journal and summary would destroy some old
		 * metadata.
		 */
		vdo_free_fixed_layout(UDS_FORGET(vdo_layout->next_layout));
		dm_kcopyd_client_destroy(UDS_FORGET(vdo_layout->copier));
		return VDO_INCREMENT_TOO_SMALL;
	}

	return VDO_SUCCESS;
}

/**
 * get_vdo_size() - Get the size of a VDO from the specified fixed_layout and
 *                  the starting offset thereof.
 * @layout: The fixed layout whose size to use.
 * @starting_offset: The starting offset of the layout.
 *
 * Return: The total size of a VDO (in blocks) with the given layout.
 */
static block_count_t __must_check
get_vdo_size(struct fixed_layout *layout, block_count_t starting_offset)
{
	/*
	 * The fixed_layout does not include the super block or any earlier
	 * metadata; all that is captured in the vdo_layout's starting offset
	 */
	return vdo_get_total_fixed_layout_size(layout) + starting_offset;
}

/**
 * vdo_get_next_layout_size() - Get the size of the next layout.
 * @vdo_layout: The layout to check.
 *
 * Return: The size which was specified when the layout was prepared for
 *         growth or 0 if the layout is not prepared to grow.
 */
block_count_t vdo_get_next_layout_size(struct vdo_layout *vdo_layout)
{
	return ((vdo_layout->next_layout == NULL) ?
			0 :
			get_vdo_size(vdo_layout->next_layout,
				     vdo_layout->starting_offset));
}

/**
 * vdo_get_next_block_allocator_partition_size() - Get the size of the next
 *                                                 block allocator partition.
 * @vdo_layout: The vdo_layout which has been prepared to grow.
 *
 * Return: The size of the block allocator partition in the next layout or 0
 *         if the layout is not prepared to grow.
 */
block_count_t
vdo_get_next_block_allocator_partition_size(struct vdo_layout *vdo_layout)
{
	struct partition *partition;

	if (vdo_layout->next_layout == NULL) {
		return 0;
	}

	partition = get_partition_from_next_layout(vdo_layout,
						   VDO_BLOCK_ALLOCATOR_PARTITION);
	return vdo_get_fixed_layout_partition_size(partition);
}

/**
 * vdo_grow_layout() - Grow the layout by swapping in the prepared layout.
 * @vdo_layout: The layout to grow.
 *
 * Return: The new size of the VDO.
 */
block_count_t vdo_grow_layout(struct vdo_layout *vdo_layout)
{
	ASSERT_LOG_ONLY(vdo_layout->next_layout != NULL,
			"VDO prepared to grow physical");
	vdo_layout->previous_layout = vdo_layout->layout;
	vdo_layout->layout = vdo_layout->next_layout;
	vdo_layout->next_layout = NULL;

	return get_vdo_size(vdo_layout->layout, vdo_layout->starting_offset);
}

/**
 * vdo_finish_layout_growth() - Clean up any unused resources once an attempt
 *                              to grow has completed.
 * @vdo_layout: The layout.
 */
void vdo_finish_layout_growth(struct vdo_layout *vdo_layout)
{
	if (vdo_layout->layout != vdo_layout->previous_layout) {
		vdo_free_fixed_layout(UDS_FORGET(vdo_layout->previous_layout));
	}

	if (vdo_layout->layout != vdo_layout->next_layout) {
		vdo_free_fixed_layout(UDS_FORGET(vdo_layout->next_layout));
	}
}

static void copy_callback(int read_err, unsigned long write_err, void *context)
{
	struct vdo_completion *completion = context;
	int result = (((read_err == 0) && (write_err == 0))
		      ? VDO_SUCCESS : -EIO );
	vdo_finish_completion(completion, result);
}

static int partition_to_region(struct partition *partition,
			       struct vdo *vdo,
			       struct dm_io_region *region)
{
	block_count_t blocks
		= vdo_get_fixed_layout_partition_size(partition);
	physical_block_number_t pbn;

	int result = vdo_translate_to_pbn(partition, 0, &pbn);

	if (result != VDO_SUCCESS) {
		return result;
	}

	pbn -= vdo->geometry.bio_offset;

	*region = (struct dm_io_region) {
		.bdev = vdo_get_backing_device(vdo),
		.sector = pbn * VDO_SECTORS_PER_BLOCK,
		.count = blocks * VDO_SECTORS_PER_BLOCK,
	};
	return VDO_SUCCESS;
}

/**
 * vdo_copy_layout_partition() - Copy a partition from the location specified
 *                               in the current layout to that in the next
 *                               layout.
 * @layout: The vdo_layout which is prepared to grow.
 * @id: The ID of the partition to copy.
 * @parent: The completion to notify when the copy is complete.
 */
void vdo_copy_layout_partition(struct vdo_layout *layout,
			       enum partition_id id,
			       struct vdo_completion *parent)
{
	struct vdo *vdo = parent->vdo;
	struct dm_io_region read_region, write_regions[1];
	int result = VDO_SUCCESS;

	struct partition *from = vdo_get_partition(layout, id);
	struct partition *to = get_partition_from_next_layout(layout, id);

	result = partition_to_region(from, vdo, &read_region);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	result = partition_to_region(to, vdo, &write_regions[0]);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	dm_kcopyd_copy(layout->copier, &read_region, 1, write_regions, 0,
		       copy_callback, parent); }

/**
 * vdo_get_fixed_layout() - Get the current fixed layout of the vdo.
 * @vdo_layout: The layout.
 *
 * Return: The layout's current fixed layout.
 */
struct fixed_layout *vdo_get_fixed_layout(const struct vdo_layout *vdo_layout)
{
	return vdo_layout->layout;
}
