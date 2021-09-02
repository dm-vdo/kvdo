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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/volumeGeometry.c#57 $
 */

#include "volumeGeometry.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"

#include "bio.h"
#include "checksum.h"
#include "constants.h"
#include "header.h"
#include "releaseVersions.h"
#include "statusCodes.h"
#include "types.h"

enum {
	MAGIC_NUMBER_SIZE = 8,
	DEFAULT_GEOMETRY_BLOCK_VERSION = 5,
};

struct geometry_block {
	char magic_number[MAGIC_NUMBER_SIZE];
	struct header header;
	crc32_checksum_t checksum;
} __packed;

static const struct header GEOMETRY_BLOCK_HEADER_5_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 5,
		.minor_version = 0,
	},
	// Note: this size isn't just the payload size following the header,
	// like it is everywhere else in VDO.
	.size = sizeof(struct geometry_block) + sizeof(struct volume_geometry),
};

static const struct header GEOMETRY_BLOCK_HEADER_4_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 4,
		.minor_version = 0,
	},
	// Note: this size isn't just the payload size following the header,
	// like it is everywhere else in VDO.
	.size = sizeof(struct geometry_block) +
		sizeof(struct volume_geometry_4_0),
};

static const byte MAGIC_NUMBER[MAGIC_NUMBER_SIZE + 1] = "dmvdo001";

static const release_version_number_t COMPATIBLE_RELEASE_VERSIONS[] = {
	VDO_MAGNESIUM_RELEASE_VERSION_NUMBER,
	VDO_ALUMINUM_RELEASE_VERSION_NUMBER,
};

/**
 * Determine whether the supplied release version can be understood by
 * the VDO code.
 *
 * @param version  The release version number to check
 *
 * @return <code>True</code> if the given version can be loaded.
 **/
static inline bool is_loadable_release_version(release_version_number_t version)
{
	unsigned int i;

	if (version == VDO_CURRENT_RELEASE_VERSION_NUMBER) {
		return true;
	}

	for (i = 0; i < ARRAY_SIZE(COMPATIBLE_RELEASE_VERSIONS); i++) {
		if (version == COMPATIBLE_RELEASE_VERSIONS[i]) {
			return true;
		}
	}

	return false;
}

/**
 * Decode the on-disk representation of an index configuration from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decode_index_config(struct buffer *buffer,
			       struct index_config *config)
{
	uint32_t mem;
	bool sparse;
	int result = get_uint32_le_from_buffer(buffer, &mem);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_boolean(buffer, &sparse);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*config = (struct index_config) {
		.mem = mem,
		.sparse = sparse,
	};
	return VDO_SUCCESS;
}


/**
 * Decode the on-disk representation of a volume region from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param region  The structure to receive the decoded fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decode_volume_region(struct buffer *buffer,
				struct volume_region *region)
{
	physical_block_number_t start_block;
	enum volume_region_id id;
	int result = get_uint32_le_from_buffer(buffer, &id);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &start_block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*region = (struct volume_region) {
		.id = id,
		.start_block = start_block,
	};
	return VDO_SUCCESS;
}


/**
 * Decode the on-disk representation of a volume geometry from a buffer.
 *
 * @param buffer    A buffer positioned at the start of the encoding
 * @param geometry  The structure to receive the decoded fields
 * @param version   The geometry block version to decode
 *
 * @return UDS_SUCCESS or an error
 **/
static int decode_volume_geometry(struct buffer *buffer,
				  struct volume_geometry *geometry,
				  uint32_t version)
{
	release_version_number_t release_version;
	enum volume_region_id id;
	nonce_t nonce;
	block_count_t bio_offset;
	int result = get_uint32_le_from_buffer(buffer, &release_version);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &nonce);
	if (result != VDO_SUCCESS) {
		return result;
	}

	geometry->release_version = release_version;
	geometry->nonce = nonce;

	result = get_bytes_from_buffer(buffer, sizeof(uuid_t),
				       (unsigned char *) &geometry->uuid);
	if (result != VDO_SUCCESS) {
		return result;
	}

	bio_offset = 0;
	if (version > 4) {
		result = get_uint64_le_from_buffer(buffer, &bio_offset);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}
	geometry->bio_offset = bio_offset;

	for (id = 0; id < VDO_VOLUME_REGION_COUNT; id++) {
		result = decode_volume_region(buffer, &geometry->regions[id]);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return decode_index_config(buffer, &geometry->index_config);
}


/**
 * Decode the on-disk representation of a geometry block, up to but not
 * including the checksum, from a buffer.
 *
 * @param buffer    A buffer positioned at the start of the block
 * @param geometry  The structure to receive the decoded volume geometry fields
 *
 * @return UDS_SUCCESS or an error
 **/
static int decode_geometry_block(struct buffer *buffer,
				 struct volume_geometry *geometry)
{
	int result;
	struct header header;

	if (!has_same_bytes(buffer, MAGIC_NUMBER, MAGIC_NUMBER_SIZE)) {
		return VDO_BAD_MAGIC;
	}

	result = skip_forward(buffer, MAGIC_NUMBER_SIZE);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (header.version.major_version <= 4) {
		result = validate_vdo_header(&GEOMETRY_BLOCK_HEADER_4_0,
					     &header, true, __func__);
	} else {
		result = validate_vdo_header(&GEOMETRY_BLOCK_HEADER_5_0,
					     &header, true, __func__);
	}
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_volume_geometry(buffer, geometry,
					header.version.major_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Leave the CRC for the caller to decode and verify.
	return ASSERT(header.size == (uncompacted_amount(buffer) +
				      sizeof(crc32_checksum_t)),
		      "should have decoded up to the geometry checksum");
}

/**
 * Decode and validate an encoded geometry block.
 *
 * @param block     The encoded geometry block
 * @param geometry  The structure to receive the decoded fields
 **/
static int __must_check
vdo_parse_geometry_block(byte *block, struct volume_geometry *geometry)
{
	crc32_checksum_t checksum, saved_checksum;
	struct buffer *buffer;
	int result;

	result = wrap_buffer(block, VDO_BLOCK_SIZE, VDO_BLOCK_SIZE, &buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_geometry_block(buffer, geometry);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	// Checksum everything decoded so far.
	checksum = vdo_update_crc32(VDO_INITIAL_CHECKSUM, block,
				    uncompacted_amount(buffer));
	result = get_uint32_le_from_buffer(buffer, &saved_checksum);
	if (result != VDO_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	// Finished all decoding. Everything that follows is validation code.
	free_buffer(UDS_FORGET(buffer));

	if (!is_loadable_release_version(geometry->release_version)) {
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "release version %d cannot be loaded",
					      geometry->release_version);
	}

	return ((checksum == saved_checksum) ? VDO_SUCCESS :
					      VDO_CHECKSUM_MISMATCH);
}

/**********************************************************************/
int vdo_read_geometry_block(struct block_device *bdev,
			    struct volume_geometry *geometry)
{
	struct bio *bio;
	byte *block;
	int result = UDS_ALLOCATE(VDO_BLOCK_SIZE, byte, __func__, &block);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(block);
		return result;
	}

	result = vdo_reset_bio_with_buffer(bio,
					   block,
					   NULL,
					   NULL,
					   REQ_OP_READ,
					   VDO_GEOMETRY_BLOCK_LOCATION);
	if (result != VDO_SUCCESS) {
		vdo_free_bio(bio);
		UDS_FREE(block);
		return result;
	}

	bio_set_dev(bio, bdev);
	submit_bio_wait(bio);
	result = blk_status_to_errno(bio->bi_status);
	vdo_free_bio(bio);
	if (result != 0) {
		uds_log_error_strerror(result, "synchronous read failed");
		UDS_FREE(block);
		return -EIO;
	}


	result = vdo_parse_geometry_block(block, geometry);
	UDS_FREE(block);
	return result;
}


/************************************************************************/
int
vdo_index_config_to_uds_configuration(const struct index_config *index_config,
				      struct uds_configuration **uds_config_ptr)
{
	struct uds_configuration *uds_configuration;
	int result = uds_initialize_configuration(&uds_configuration,
						  index_config->mem);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "error initializing configuration");
	}

	uds_configuration_set_sparse(uds_configuration, index_config->sparse);
	*uds_config_ptr = uds_configuration;
	return VDO_SUCCESS;
}
