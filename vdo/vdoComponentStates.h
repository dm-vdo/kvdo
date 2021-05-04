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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoComponentStates.h#1 $
 */

#ifndef VDO_COMPONENT_STATES_H
#define VDO_COMPONENT_STATES_H

#include "blockMapFormat.h"
#include "fixedLayout.h"
#include "recoveryJournalFormat.h"
#include "slabDepotFormat.h"
#include "types.h"
#include "vdoComponent.h"
#include "vdoState.h"

/**
 * The version of the on-disk format of a VDO volume. This should be
 * incremented any time the on-disk representation of any VDO structure
 * changes. Changes which require only online upgrade steps should increment
 * the minor version. Changes which require an offline upgrade or which can not
 * be upgraded to at all should increment the major version and set the minor
 * version to 0.
 **/
extern const struct version_number VDO_VOLUME_VERSION_67_0;

/**
 * The entirety of the component data encoded in the VDO super block.
 **/
struct vdo_component_states {
	/* The release version */
	release_version_number_t release_version;

	/* The VDO volume version */
	struct version_number volume_version;

	/* Components */
	struct vdo_component_41_0 vdo;
	struct block_map_state_2_0 block_map;
	struct recovery_journal_state_7_0 recovery_journal;
	struct slab_depot_state_2_0 slab_depot;

	/* Our partitioning of the underlying storage */
	struct fixed_layout *layout;
};

/**
 * Clean up any allocations in a vdo_component_states.
 *
 * @param states  The component states to destroy
 **/
void destroy_component_states(struct vdo_component_states *states);

/**
 * Decode the payload of a super block.
 *
 * @param buffer                    The buffer containing the encoded super
 *                                  block contents
 * @param expected_release_version  The required release version
 * @param states                    A pointer to hold the decoded states
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
decode_component_states(struct buffer *buffer,
			release_version_number_t expected_release_version,
			struct vdo_component_states *states);

/**
 * Validate the decoded super block configuration.
 *
 * @param states          The state decoded from the super block
 * @param geometry_nonce  The nonce from the geometry block
 * @param size            The size of underlying storage
 *
 * @return VDO_SUCESS or an error if the configuration is invalid
 **/
int __must_check
validate_component_states(struct vdo_component_states *states,
			  nonce_t geometry_nonce,
			  block_count_t size);

/**
 * Encode a VDO super block into a buffer for writing in the super block.
 *
 * @param buffer  The buffer to encode into
 * @param states  The states of the vdo to be encoded
 **/
int __must_check
encode_vdo(struct buffer *buffer, struct vdo_component_states *states);

/**
 * Encode the state of all vdo components for writing in the super block.
 *
 * @param buffer  The buffer to encode into
 * @param states  The states to encode
 **/
int encode_component_states(struct buffer *buffer,
			    const struct vdo_component_states *states);

#endif /* VDO_COMPONENT_STATES_H */
