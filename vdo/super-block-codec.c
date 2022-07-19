// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "super-block-codec.h"

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "status-codes.h"
#include "vdo.h"

enum {
	SUPER_BLOCK_FIXED_SIZE = VDO_ENCODED_HEADER_SIZE + sizeof(uint32_t),
	MAX_COMPONENT_DATA_SIZE = VDO_SECTOR_SIZE - SUPER_BLOCK_FIXED_SIZE,
};

static const struct header SUPER_BLOCK_HEADER_12_0 = {
	.id = VDO_SUPER_BLOCK,
	.version = {
			.major_version = 12,
			.minor_version = 0,
		},

	/* This is the minimum size, if the super block contains no components. */
	.size = SUPER_BLOCK_FIXED_SIZE - VDO_ENCODED_HEADER_SIZE,
};

/**
 * vdo_initialize_super_block_codec() - Initialize a super block codec.
 * @codec: The codec to initialize.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_initialize_super_block_codec(struct super_block_codec *codec)
{
	int result = make_buffer(MAX_COMPONENT_DATA_SIZE,
				 &codec->component_buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(VDO_BLOCK_SIZE, char, "encoded super block",
			      (char **) &codec->encoded_super_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Even though the buffer is a full block, to avoid the potential
	 * corruption from a torn write, the entire encoding must fit in the
	 * first sector.
	 */
	return wrap_buffer(codec->encoded_super_block,
			   VDO_SECTOR_SIZE,
			   0,
			   &codec->block_buffer);
}

/**
 * vdo_destroy_super_block_codec() - Free resources in a super block codec.
 * @codec: The codec to clean up.
 */
void vdo_destroy_super_block_codec(struct super_block_codec *codec)
{
	free_buffer(UDS_FORGET(codec->block_buffer));
	free_buffer(UDS_FORGET(codec->component_buffer));
	UDS_FREE(codec->encoded_super_block);
}

/**
 * vdo_encode_super_block() - Encode a super block into its on-disk
 *                            representation.
 * @codec: The super block codec.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_encode_super_block(struct super_block_codec *codec)
{
	size_t component_data_size;
	uint32_t checksum;
	struct header header = SUPER_BLOCK_HEADER_12_0;
	struct buffer *buffer = codec->block_buffer;
	int result = reset_buffer_end(buffer, 0);

	if (result != VDO_SUCCESS) {
		return result;
	}

	component_data_size = content_length(codec->component_buffer);

	/* Encode the header. */
	header.size += component_data_size;
	result = vdo_encode_header(&header, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Copy the already-encoded component data. */
	result = put_bytes(buffer, component_data_size,
			  get_buffer_contents(codec->component_buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Compute and encode the checksum. */
	checksum = vdo_crc32(codec->encoded_super_block,
			     content_length(buffer));
	result = put_uint32_le_into_buffer(buffer, checksum);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**
 * vdo_decode_super_block() - Decode a super block from its on-disk
 *                            representation.
 * @codec: The super block to decode.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_super_block(struct super_block_codec *codec)
{
	struct header header;
	int result;
	size_t component_data_size;
	uint32_t checksum, saved_checksum;

	/* Reset the block buffer to start decoding the entire first sector. */
	struct buffer *buffer = codec->block_buffer;

	clear_buffer(buffer);

	/* Decode and validate the header. */
	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_validate_header(&SUPER_BLOCK_HEADER_12_0, &header, false,
				     __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (header.size > content_length(buffer)) {
		/*
		 * We can't check release version or checksum until we know the
		 * content size, so we have to assume a version mismatch on
		 * unexpected values.
		 */
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "super block contents too large: %zu",
					      header.size);
	}

	/* Restrict the buffer to the actual payload bytes that remain. */
	result =
		reset_buffer_end(buffer, uncompacted_amount(buffer) + header.size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/* The component data is all the rest, except for the checksum. */
	component_data_size = content_length(buffer) - sizeof(uint32_t);
	result = put_buffer(codec->component_buffer, buffer,
			    component_data_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * Checksum everything up to but not including the saved checksum
	 * itself.
	 */
	checksum = vdo_crc32(codec->encoded_super_block,
			     uncompacted_amount(buffer));

	/* Decode and verify the saved checksum. */
	result = get_uint32_le_from_buffer(buffer, &saved_checksum);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT(content_length(buffer) == 0,
			"must have decoded entire superblock payload");
	if (result != VDO_SUCCESS) {
		return result;
	}

	return ((checksum != saved_checksum) ? VDO_CHECKSUM_MISMATCH
		: VDO_SUCCESS);
}

