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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/superBlock.c#17 $
 */

#include "superBlock.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "completion.h"
#include "constants.h"
#include "header.h"
#include "releaseVersions.h"
#include "statusCodes.h"
#include "types.h"
#include "vio.h"

struct vdo_super_block {
	/** The parent for asynchronous load and save operations */
	struct vdo_completion *parent;
	/** The vio for reading and writing the super block to disk */
	struct vio *vio;
	/** The buffer for encoding and decoding component data */
	struct buffer *component_buffer;
	/**
	 * A sector-sized buffer wrapping the first sector of
	 * encoded_super_block, for encoding and decoding the entire super
	 * block.
	 **/
	struct buffer *block_buffer;
	/** A 1-block buffer holding the encoded on-disk super block */
	byte *encoded_super_block;
	/** The release version number loaded from the volume */
	ReleaseVersionNumber loaded_release_version;
	/** Whether this super block may not be written */
	bool unwriteable;
};

enum {
	SUPER_BLOCK_FIXED_SIZE = ENCODED_HEADER_SIZE
		+ sizeof(ReleaseVersionNumber) + CHECKSUM_SIZE,
	MAX_COMPONENT_DATA_SIZE = VDO_SECTOR_SIZE - SUPER_BLOCK_FIXED_SIZE,
};

static const struct header SUPER_BLOCK_HEADER_12_0 = {
	.id = SUPER_BLOCK,
	.version =
		{
			.major_version = 12,
			.minor_version = 0,
		},

	// This is the minimum size, if the super block contains no components.
	.size = SUPER_BLOCK_FIXED_SIZE - ENCODED_HEADER_SIZE,
};

/**
 * Allocate a super block. Callers must free the allocated super block even
 * on error.
 *
 * @param layer            The physical layer which holds the super block on
 *                         disk
 * @param super_block_ptr  A pointer to hold the new super block
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
allocate_super_block(PhysicalLayer *layer,
		     struct vdo_super_block **super_block_ptr)
{
	int result =
		ALLOCATE(1, struct vdo_super_block, __func__, super_block_ptr);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct vdo_super_block *super_block = *super_block_ptr;
	result = make_buffer(MAX_COMPONENT_DATA_SIZE,
			     &super_block->component_buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE,
					 "encoded super block",
					 (char **) &super_block->encoded_super_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Even though the buffer is a full block, to avoid the potential
	// corruption from a torn write, the entire encoding must fit in the
	// first sector.
	result = wrap_buffer(super_block->encoded_super_block,
			     VDO_SECTOR_SIZE,
			     0,
			     &super_block->block_buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (layer->createMetadataVIO == NULL) {
		return VDO_SUCCESS;
	}

	return create_vio(layer, VIO_TYPE_SUPER_BLOCK, VIO_PRIORITY_METADATA,
			  super_block,
			  (char *) super_block->encoded_super_block,
			  &super_block->vio);
}

/**********************************************************************/
int make_super_block(PhysicalLayer *layer,
		     struct vdo_super_block **super_block_ptr)
{
	struct vdo_super_block *super_block;
	int result = allocate_super_block(layer, &super_block);
	if (result != VDO_SUCCESS) {
		free_super_block(&super_block);
		return result;
	}

	// For a new super block, use the current release.
	super_block->loaded_release_version = CURRENT_RELEASE_VERSION_NUMBER;
	*super_block_ptr = super_block;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_super_block(struct vdo_super_block **super_block_ptr)
{
	if (*super_block_ptr == NULL) {
		return;
	}

	struct vdo_super_block *super_block = *super_block_ptr;
	free_buffer(&super_block->block_buffer);
	free_buffer(&super_block->component_buffer);
	free_vio(&super_block->vio);
	FREE(super_block->encoded_super_block);
	FREE(super_block);
	*super_block_ptr = NULL;
}

/**
 * Encode a super block into its on-disk representation.
 *
 * @param super_block  The super block to encode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
encode_super_block(struct vdo_super_block *super_block)
{
	struct buffer *buffer = super_block->block_buffer;
	int result = reset_buffer_end(buffer, 0);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t component_data_size =
		content_length(super_block->component_buffer);

	// Encode the header.
	struct header header = SUPER_BLOCK_HEADER_12_0;
	header.size += component_data_size;
	result = encode_header(&header, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Encode the loaded release version.
	result = put_uint32_le_into_buffer(buffer,
				           super_block->loaded_release_version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Copy the already-encoded component data.
	result = put_bytes(buffer, component_data_size,
			  get_buffer_contents(super_block->component_buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Compute and encode the checksum.
	crc32_checksum_t checksum =
		update_crc32(INITIAL_CHECKSUM, super_block->encoded_super_block,
			     content_length(buffer));
	result = put_uint32_le_into_buffer(buffer, checksum);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int save_super_block(PhysicalLayer *layer, struct vdo_super_block *super_block,
		     PhysicalBlockNumber super_block_offset)
{
	int result = encode_super_block(super_block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return layer->writer(layer, super_block_offset, 1,
			     (char *) super_block->encoded_super_block, NULL);
}

/**
 * Finish the parent of a super block load or save operation. This
 * callback is registered in save_super_block_async() and
 * load_super_block_async.
 *
 * @param completion  The super block vio
 **/
static void finish_super_block_parent(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;
	super_block->parent = NULL;
	finish_completion(parent, completion->result);
}

/**
 * Log a super block save error. This error handler is registered in
 * save_super_block_async().
 *
 * @param completion  The super block vio
 **/
static void handle_save_error(struct vdo_completion *completion)
{
	logErrorWithStringError(completion->result, "super block save failed");
	/*
	 * Mark the super block as unwritable so that we won't attempt to write
	 * it again. This avoids the case where a growth attempt fails writing
	 * the super block with the new size, but the subsequent attempt to
	 * write out the read-only state succeeds. In this case, writes which
	 * happened just before the suspend would not be visible if the VDO is
	 * restarted without rebuilding, but, after a read-only rebuild, the
	 * effects of those writes would reappear.
	 */
	((struct vdo_super_block *) completion->parent)->unwriteable = true;
	completion->callback(completion);
}

/**********************************************************************/
void save_super_block_async(struct vdo_super_block *super_block,
			    PhysicalBlockNumber super_block_offset,
			    struct vdo_completion *parent)
{
	if (super_block->unwriteable) {
		finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	if (super_block->parent != NULL) {
		finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	int result = encode_super_block(super_block);
	if (result != VDO_SUCCESS) {
		finish_completion(parent, result);
		return;
	}

	super_block->parent = parent;
	super_block->vio->completion.callbackThreadID = parent->callbackThreadID;
	launch_write_metadata_vio_with_flush(super_block->vio,
					     super_block_offset,
					     finish_super_block_parent,
					     handle_save_error,
					     true, true);
}

/**
 * Decode a super block from its on-disk representation.
 *
 * @param super_block  The super block to decode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
decode_super_block(struct vdo_super_block *super_block)
{
	// Reset the block buffer to start decoding the entire first sector.
	struct buffer *buffer = super_block->block_buffer;
	clear_buffer(buffer);

	// Decode and validate the header.
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&SUPER_BLOCK_HEADER_12_0, &header, false,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (header.size > content_length(buffer)) {
		// We can't check release version or checksum until we know the
		// content size, so we have to assume a version mismatch on
		// unexpected values.
		return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
					       "super block contents too large: %zu",
					       header.size);
	}

	// Restrict the buffer to the actual payload bytes that remain.
	result =
		reset_buffer_end(buffer, uncompacted_amount(buffer) + header.size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Decode and store the release version number. It will be checked when
	// the VDO master version is decoded and validated.
	result = get_uint32_le_from_buffer(buffer,
				           &super_block->loaded_release_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// The component data is all the rest, except for the checksum.
	size_t component_data_size =
		content_length(buffer) - sizeof(crc32_checksum_t);
	result = put_buffer(super_block->component_buffer, buffer,
			    component_data_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Checksum everything up to but not including the saved checksum
	// itself.
	crc32_checksum_t checksum =
		update_crc32(INITIAL_CHECKSUM, super_block->encoded_super_block,
			     uncompacted_amount(buffer));

	// Decode and verify the saved checksum.
	crc32_checksum_t saved_checksum;
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

/**********************************************************************/
int load_super_block(PhysicalLayer *layer,
		     PhysicalBlockNumber super_block_offset,
		     struct vdo_super_block **super_block_ptr)
{
	struct vdo_super_block *super_block = NULL;
	int result = allocate_super_block(layer, &super_block);
	if (result != VDO_SUCCESS) {
		free_super_block(&super_block);
		return result;
	}

	result = layer->reader(layer, super_block_offset, 1,
			       (char *)super_block->encoded_super_block, NULL);
	if (result != VDO_SUCCESS) {
		free_super_block(&super_block);
		return result;
	}

	result = decode_super_block(super_block);
	if (result != VDO_SUCCESS) {
		free_super_block(&super_block);
		return result;
	}

	*super_block_ptr = super_block;
	return result;
}

/**
 * Continue after loading the super block. This callback is registered
 * in load_super_block_async().
 *
 * @param completion  The super block vio
 **/
static void finish_reading_super_block(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;
	super_block->parent = NULL;
	finish_completion(parent, decode_super_block(super_block));
}

/**********************************************************************/
void load_super_block_async(struct vdo_completion *parent,
			    PhysicalBlockNumber super_block_offset,
			    struct vdo_super_block **super_block_ptr)
{
	PhysicalLayer *layer = parent->layer;
	struct vdo_super_block *super_block = NULL;
	int result = allocate_super_block(layer, &super_block);
	if (result != VDO_SUCCESS) {
		free_super_block(&super_block);
		finish_completion(parent, result);
		return;
	}

	*super_block_ptr = super_block;

	super_block->parent = parent;
	super_block->vio->completion.callbackThreadID = parent->callbackThreadID;
	launch_read_metadata_vio(super_block->vio,
				 super_block_offset,
				 finish_reading_super_block,
				 finish_super_block_parent);
}

/**********************************************************************/
struct buffer *get_component_buffer(struct vdo_super_block *super_block)
{
	return super_block->component_buffer;
}

/**********************************************************************/
ReleaseVersionNumber
get_loaded_release_version(const struct vdo_super_block *super_block)
{
	return super_block->loaded_release_version;
}

/**********************************************************************/
size_t get_fixed_super_block_size(void)
{
	return SUPER_BLOCK_FIXED_SIZE;
}
