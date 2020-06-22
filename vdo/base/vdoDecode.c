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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDecode.c#6 $
 */

#include "vdoDecode.h"

#include "logger.h"

#include "blockMap.h"
#include "constants.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "types.h"
#include "vdoComponentStates.h"
#include "vdoInternal.h"

/**********************************************************************/
int start_vdo_decode(struct vdo *vdo, bool validate_config)
{
	// Decode the component data from the super block.
	struct buffer *buffer = get_component_buffer(vdo->super_block);
	int result = decode_component_states(buffer,
					     vdo->load_config.release_version,
					     &vdo->states);

	if (result != VDO_SUCCESS) {
		return result;
	}

	set_vdo_state(vdo, vdo->states.vdo.state);
	vdo->load_state = vdo->states.vdo.state;

	if (!validate_config) {
		return VDO_SUCCESS;
	}

	if (vdo->load_config.nonce != vdo->states.vdo.nonce) {
		return logErrorWithStringError(VDO_BAD_NONCE,
					       "Geometry nonce %llu does not match superblock nonce %llu",
					       vdo->load_config.nonce,
					       vdo->states.vdo.nonce);
	}

	block_count_t block_count = vdo->layer->getBlockCount(vdo->layer);
	result = validate_vdo_config(&vdo->states.vdo.config, block_count,
				     true);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int __must_check finish_vdo_decode(struct vdo *vdo)
{
	const struct thread_config *thread_config = get_thread_config(vdo);
	int result =
		decode_recovery_journal(vdo->states.recovery_journal,
					vdo->states.vdo.nonce,
					vdo->layer,
					get_vdo_partition(vdo->layout,
							  RECOVERY_JOURNAL_PARTITION),
					vdo->states.vdo.complete_recoveries,
					vdo->states.vdo.config.recovery_journal_size,
					RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
					vdo->read_only_notifier,
					thread_config,
					&vdo->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_slab_depot(vdo->states.slab_depot,
				   thread_config,
				   vdo->states.vdo.nonce,
				   vdo->layer,
				   get_vdo_partition(vdo->layout,
						     SLAB_SUMMARY_PARTITION),
				   vdo->read_only_notifier,
				   vdo->recovery_journal,
				   &vdo->state,
				   &vdo->depot);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return decode_block_map(vdo->states.block_map,
				vdo->states.vdo.config.logical_blocks,
				thread_config,
				&vdo->block_map);
}
