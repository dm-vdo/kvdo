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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoComponent.c#11 $
 */

#include "vdoComponent.h"

#include "buffer.h"
#include "logger.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "slabDepotFormat.h"
#include "statusCodes.h"
#include "types.h"

/**
 * The current version for the data encoded in the super block. This must
 * be changed any time there is a change to encoding of the component data
 * of any VDO component.
 **/
static const struct version_number VDO_COMPONENT_DATA_41_0 = {
	.major_version = 41,
	.minor_version = 0,
};

/**********************************************************************/
size_t get_vdo_component_encoded_size(void)
{
	return (sizeof(struct version_number)
		+ sizeof(struct vdo_component_41_0));
}

/**
 * Encode a vdo_config structure into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
encode_vdo_config(const struct vdo_config *config, struct buffer *buffer)
{
	int result = put_uint64_le_into_buffer(buffer, config->logical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->physical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->slab_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
					   config->recovery_journal_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer, config->slab_journal_blocks);
}

/**********************************************************************/
int encode_vdo_component(struct vdo_component_41_0 state,
			 struct buffer *buffer)
{
	size_t initial_length, encoded_size;

	int result = encode_vdo_version_number(VDO_COMPONENT_DATA_41_0, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = put_uint32_le_into_buffer(buffer, state.state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.complete_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.read_only_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_config(&state.config, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.nonce);
	if (result != VDO_SUCCESS) {
		return result;
	}

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(encoded_size == sizeof(struct vdo_component_41_0),
		      "encoded VDO component size must match structure size");
}

/**
 * Decode a vdo_config structure from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
decode_vdo_config(struct buffer *buffer, struct vdo_config *config)
{
	block_count_t logical_blocks, physical_blocks, slab_size;
	block_count_t recovery_journal_size, slab_journal_blocks;

	int result = get_uint64_le_from_buffer(buffer, &logical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &physical_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &slab_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &recovery_journal_size);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &slab_journal_blocks);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*config = (struct vdo_config) {
		.logical_blocks = logical_blocks,
		.physical_blocks = physical_blocks,
		.slab_size = slab_size,
		.recovery_journal_size = recovery_journal_size,
		.slab_journal_blocks = slab_journal_blocks,
	};
	return VDO_SUCCESS;
}

/**
 * Decode the version 41.0 component state for the vdo itself from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
decode_vdo_component_41_0(struct buffer *buffer,
			  struct vdo_component_41_0 *state)
{
	size_t decoded_size, initial_length = content_length(buffer);

	uint64_t complete_recoveries;
	uint64_t read_only_recoveries;
	struct vdo_config config;
	nonce_t nonce;
	enum vdo_state vdo_state;

	int result = get_uint32_le_from_buffer(buffer, &vdo_state);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &complete_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &read_only_recoveries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_config(buffer, &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &nonce);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*state = (struct vdo_component_41_0) {
		.state = vdo_state,
		.complete_recoveries = complete_recoveries,
		.read_only_recoveries = read_only_recoveries,
		.config = config,
		.nonce = nonce,
	};

	decoded_size = initial_length - content_length(buffer);
	return ASSERT(decoded_size == sizeof(struct vdo_component_41_0),
		      "decoded VDO component size must match structure size");
}

/**********************************************************************/
int decode_vdo_component(struct buffer *buffer,
			 struct vdo_component_41_0 *component_ptr)
{
	struct vdo_component_41_0 component;
	struct version_number version;
	int result = decode_vdo_version_number(buffer, &version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_vdo_version(version, VDO_COMPONENT_DATA_41_0,
				      "VDO component data");
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_component_41_0(buffer, &component);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*component_ptr = component;
	return VDO_SUCCESS;
}

/**********************************************************************/
int validate_vdo_config(const struct vdo_config *config,
			block_count_t block_count,
			bool require_logical)
{
	struct slab_config slab_config;
	int result = ASSERT(config->slab_size > 0, "slab size unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_2(config->slab_size),
			"slab size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_size <= (1 << MAX_VDO_SLAB_BITS),
			"slab size must be less than or equal to 2^%d",
			MAX_VDO_SLAB_BITS);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_journal_blocks >= MINIMUM_VDO_SLAB_JOURNAL_BLOCKS,
		       "slab journal size meets minimum size");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_journal_blocks <= config->slab_size,
			"slab journal size is within expected bound");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = configure_vdo_slab(config->slab_size,
				    config->slab_journal_blocks,
				    &slab_config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT((slab_config.data_blocks >= 1),
			"slab must be able to hold at least one block");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physical_blocks > 0,
			"physical blocks unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physical_blocks <= MAXIMUM_VDO_PHYSICAL_BLOCKS,
			"physical block count %llu exceeds maximum %llu",
			config->physical_blocks,
			MAXIMUM_VDO_PHYSICAL_BLOCKS);
	if (result != UDS_SUCCESS) {
		return VDO_OUT_OF_RANGE;
	}

	// This can't check equality because FileLayer et al can only known
	// about the storage size, which may not match the super block size.
	if (block_count < config->physical_blocks) {
		uds_log_error("A physical size of %llu blocks was specified, but that is smaller than the %llu blocks configured in the vdo super block",
			      block_count,
			      config->physical_blocks);
		return VDO_PARAMETER_MISMATCH;
	}

	result = ASSERT(!require_logical || (config->logical_blocks > 0),
			"logical blocks unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->logical_blocks <= MAXIMUM_VDO_LOGICAL_BLOCKS,
			"logical blocks too large");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->recovery_journal_size > 0,
			"recovery journal size unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_2(config->recovery_journal_size),
			"recovery journal size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	return result;
}
