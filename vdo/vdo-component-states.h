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
 */

#ifndef VDO_COMPONENT_STATES_H
#define VDO_COMPONENT_STATES_H

#include "block-map-format.h"
#include "fixed-layout.h"
#include "recovery-journal-format.h"
#include "slab-depot-format.h"
#include "types.h"
#include "vdo-component.h"
#include "vdo-state.h"

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
	struct vdo_component vdo;
	struct block_map_state_2_0 block_map;
	struct recovery_journal_state_7_0 recovery_journal;
	struct slab_depot_state_2_0 slab_depot;

	/* Our partitioning of the underlying storage */
	struct fixed_layout *layout;
};

void destroy_vdo_component_states(struct vdo_component_states *states);

int __must_check
decode_vdo_component_states(struct buffer *buffer,
			    release_version_number_t expected_release_version,
			    struct vdo_component_states *states);

int __must_check
validate_vdo_component_states(struct vdo_component_states *states,
			      nonce_t geometry_nonce,
			      block_count_t physical_size,
			      block_count_t logical_size);

/**
 * Encode a VDO super block into a buffer for writing in the super block.
 *
 * @param buffer  The buffer to encode into
 * @param states  The states of the vdo to be encoded
 **/
int __must_check
encode_vdo(struct buffer *buffer, struct vdo_component_states *states);

int encode_vdo_component_states(struct buffer *buffer,
				const struct vdo_component_states *states);

#endif /* VDO_COMPONENT_STATES_H */
