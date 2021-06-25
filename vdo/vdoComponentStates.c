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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoComponentStates.c#15 $
 */

#include "vdoComponentStates.h"

#include "logger.h"
#include "permassert.h"

#include "blockMapFormat.h"
#include "constants.h"
#include "fixedLayout.h"
#include "numUtils.h"
#include "recoveryJournalFormat.h"
#include "slabDepotFormat.h"
#include "statusCodes.h"
#include "types.h"
#include "vdoComponent.h"

const struct version_number VDO_VOLUME_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

/**********************************************************************/
void destroy_vdo_component_states(struct vdo_component_states *states)
{
	if (states == NULL) {
		return;
	}

	free_vdo_fixed_layout(&states->layout);
}

/**
 * Decode the components now that we know the component data is a version we
 * understand.
 *
 * @param buffer  The buffer being decoded
 * @param states  An object to hold the successfully decoded state
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check
decode_components(struct buffer *buffer, struct vdo_component_states *states)
{
	int result = decode_vdo_component(buffer, &states->vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_fixed_layout(buffer, &states->layout);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_recovery_journal_state_7_0(buffer,
						       &states->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_slab_depot_state_2_0(buffer, &states->slab_depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_block_map_state_2_0(buffer, &states->block_map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((content_length(buffer) == 0),
			"All decoded component data was used");
	return VDO_SUCCESS;
}

/**********************************************************************/
int decode_vdo_component_states(struct buffer *buffer,
				release_version_number_t expected_release_version,
				struct vdo_component_states *states)
{
	// Check the release version against the one from the geometry.
	int result = get_uint32_le_from_buffer(buffer,
					       &states->release_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (states->release_version != expected_release_version) {
		return log_error_strerror(VDO_UNSUPPORTED_VERSION,
					  "Geometry release version %u does not match super block release version %u",
					  expected_release_version,
					  states->release_version);
	}

	// Check the VDO volume version
	result = decode_vdo_version_number(buffer, &states->volume_version);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_vdo_version(VDO_VOLUME_VERSION_67_0,
				      states->volume_version,
				      "volume");
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_components(buffer, states);
	if (result != VDO_SUCCESS) {
		free_vdo_fixed_layout(&states->layout);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int validate_vdo_component_states(struct vdo_component_states *states,
				  nonce_t geometry_nonce,
				  block_count_t size)
{
	if (geometry_nonce != states->vdo.nonce) {
		return log_error_strerror(VDO_BAD_NONCE,
					  "Geometry nonce %llu does not match superblock nonce %llu",
					  (unsigned long long) geometry_nonce,
					  (unsigned long long) states->vdo.nonce);
	}

	return validate_vdo_config(&states->vdo.config, size, true);
}

/**
 * Get the component data size of a vdo.
 *
 * @param layout  The layout of the vdo
 *
 * @return the component data size of the vdo
 **/
static size_t __must_check get_component_data_size(struct fixed_layout *layout)
{
	return (sizeof(release_version_number_t) +
		sizeof(struct version_number) +
		get_vdo_component_encoded_size() +
		get_vdo_fixed_layout_encoded_size(layout) +
		get_vdo_recovery_journal_encoded_size() +
		get_vdo_slab_depot_encoded_size() +
		get_vdo_block_map_encoded_size());
}

/**********************************************************************/
int encode_vdo_component_states(struct buffer *buffer,
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

	result = encode_vdo_version_number(states->volume_version, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_component(states->vdo, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_fixed_layout(states->layout, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_recovery_journal_state_7_0(states->recovery_journal,
						       buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_slab_depot_state_2_0(states->slab_depot, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = encode_vdo_block_map_state_2_0(states->block_map, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	expected_size = get_component_data_size(states->layout);
	ASSERT_LOG_ONLY((content_length(buffer) == expected_size),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}
