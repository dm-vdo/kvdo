// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-component-states.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map-format.h"
#include "constants.h"
#include "num-utils.h"
#include "recovery-journal-format.h"
#include "slab-depot-format.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-component.h"
#include "vdo-layout.h"

const struct version_number VDO_VOLUME_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

/**
 * vdo_destroy_component_states() - Clean up any allocations in a
 *                                  vdo_component_states.
 * @states: The component states to destroy.
 */
void vdo_destroy_component_states(struct vdo_component_states *states)
{
	if (states == NULL) {
		return;
	}

	vdo_free_fixed_layout(UDS_FORGET(states->layout));
}

/**
 * decode_components() - Decode the components now that we know the component
 *                       data is a version we understand.
 * @buffer: The buffer being decoded.
 * @states: An object to hold the successfully decoded state.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
decode_components(struct buffer *buffer, struct vdo_component_states *states)
{
	int result = vdo_decode_component(buffer, &states->vdo);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_fixed_layout(buffer, &states->layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_recovery_journal_state_7_0(buffer,
						       &states->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_slab_depot_state_2_0(buffer, &states->slab_depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_decode_block_map_state_2_0(buffer, &states->block_map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((content_length(buffer) == 0),
			"All decoded component data was used");
	return VDO_SUCCESS;
}

/**
 * vdo_decode_component_states() - Decode the payload of a super block.
 * @buffer: The buffer containing the encoded super block contents.
 * @expected_release_version: The required release version.
 * @states: A pointer to hold the decoded states.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_component_states(struct buffer *buffer,
				release_version_number_t expected_release_version,
				struct vdo_component_states *states)
{
	/* Check the release version against the one from the geometry. */
	int result = get_uint32_le_from_buffer(buffer,
					       &states->release_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (states->release_version != expected_release_version) {
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "Geometry release version %u does not match super block release version %u",
					      expected_release_version,
					      states->release_version);
	}

	/* Check the VDO volume version */
	result = vdo_decode_version_number(buffer, &states->volume_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_validate_version(VDO_VOLUME_VERSION_67_0,
				      states->volume_version,
				      "volume");
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_components(buffer, states);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(UDS_FORGET(states->layout));
		return result;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_validate_component_states() - Validate the decoded super block
 *                                   configuration.
 * @states: The state decoded from the super block.
 * @geometry_nonce: The nonce from the geometry block.
 * @physical_size: The minimum block count of the underlying storage.
 * @logical_size: The expected logical size of the VDO, or 0 if the
 *                logical size may be unspecified.
 *
 * Return: VDO_SUCCESS or an error if the configuration is invalid.
 */
int vdo_validate_component_states(struct vdo_component_states *states,
				  nonce_t geometry_nonce,
				  block_count_t physical_size,
				  block_count_t logical_size)
{
	if (geometry_nonce != states->vdo.nonce) {
		return uds_log_error_strerror(VDO_BAD_NONCE,
					      "Geometry nonce %llu does not match superblock nonce %llu",
					      (unsigned long long) geometry_nonce,
					      (unsigned long long) states->vdo.nonce);
	}

	return vdo_validate_config(&states->vdo.config,
				   physical_size,
				   logical_size);
}

/**
 * get_component_data_size() - Get the component data size of a vdo.
 * @layout: The layout of the vdo.
 *
 * Return: The component data size of the vdo.
 */
static size_t __must_check get_component_data_size(struct fixed_layout *layout)
{
	return (sizeof(release_version_number_t) +
		sizeof(struct packed_version_number) +
		vdo_get_component_encoded_size() +
		vdo_get_fixed_layout_encoded_size(layout) +
		vdo_get_recovery_journal_encoded_size() +
		vdo_get_slab_depot_encoded_size() +
		vdo_get_block_map_encoded_size());
}

/**
 * vdo_encode_component_states() - Encode the state of all vdo components for
 *                                 writing in the super block.
 * @buffer: The buffer to encode into.
 * @states: The states to encode.
 */
int vdo_encode_component_states(struct buffer *buffer,
				const struct vdo_component_states *states)
{
	size_t expected_size;
	int result = reset_buffer_end(buffer, 0);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, states->release_version);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = vdo_encode_version_number(states->volume_version, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_encode_component(states->vdo, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_encode_fixed_layout(states->layout, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_encode_recovery_journal_state_7_0(states->recovery_journal,
						       buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_encode_slab_depot_state_2_0(states->slab_depot, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_encode_block_map_state_2_0(states->block_map, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	expected_size = get_component_data_size(states->layout);
	ASSERT_LOG_ONLY((content_length(buffer) == expected_size),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}
