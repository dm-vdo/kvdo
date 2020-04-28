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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDecode.c#1 $
 */

#include "vdoDecode.h"

#include "logger.h"

#include "blockMap.h"
#include "constants.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "types.h"
#include "vdoInternal.h"

/**********************************************************************/
int start_vdo_decode(struct vdo *vdo, bool validate_config)
{
	int result = validate_vdo_version(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_vdo_component(vdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (!validate_config) {
		return VDO_SUCCESS;
	}

	if (vdo->load_config.nonce != vdo->nonce) {
		return logErrorWithStringError(VDO_BAD_NONCE,
					       "Geometry nonce %llu does not match superblock nonce %llu",
					       vdo->load_config.nonce,
					       vdo->nonce);
	}

	block_count_t block_count = vdo->layer->getBlockCount(vdo->layer);
	return validate_vdo_config(&vdo->config, block_count, true);
}

/**********************************************************************/
int __must_check finish_vdo_decode(struct vdo *vdo)
{
	struct buffer *buffer = get_component_buffer(vdo->super_block);
	const struct thread_config *thread_config = get_thread_config(vdo);
	int result =
		make_recovery_journal(vdo->nonce,
				      vdo->layer,
				      get_vdo_partition(vdo->layout,
							RECOVERY_JOURNAL_PARTITION),
				      vdo->complete_recoveries,
				      vdo->config.recovery_journal_size,
				      RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
				      vdo->read_only_notifier,
				      thread_config,
				      &vdo->recovery_journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_recovery_journal(vdo->recovery_journal, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = decode_slab_depot(buffer,
				   thread_config,
				   vdo->nonce,
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

	result = decode_block_map(buffer,
				  vdo->config.logical_blocks,
				  thread_config,
				  &vdo->block_map);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((content_length(buffer) == 0),
			"All decoded component data was used");
	return VDO_SUCCESS;
}
