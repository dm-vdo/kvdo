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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalFormat.c#2 $
 */

#include "recoveryJournalFormat.h"

#include "buffer.h"
#include "permassert.h"

#include "fixedLayout.h"
#include "header.h"
#include "types.h"

const struct header RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = RECOVERY_JOURNAL,
	.version =
		{
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

/**********************************************************************/
size_t get_recovery_journal_encoded_size(void)
{
	return ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0);
}

/**********************************************************************/
int encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
				      struct buffer *buffer)
{
	int result = encode_header(&RECOVERY_JOURNAL_HEADER_7_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	result = put_uint64_le_into_buffer(buffer, state.journal_start);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.logical_blocks_used);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
					   state.block_map_data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t encoded_size = content_length(buffer) - initial_length;
	return ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == encoded_size,
		      "encoded recovery journal component size must match header size");
}

/**********************************************************************/
int
decode_recovery_journal_state_7_0(struct buffer *buffer,
				  struct recovery_journal_state_7_0 *state)
{
	struct header header;
	int result = decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_header(&RECOVERY_JOURNAL_HEADER_7_0, &header, true,
				 __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	size_t initial_length = content_length(buffer);

	sequence_number_t journal_start;
	result = get_uint64_le_from_buffer(buffer, &journal_start);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t logical_blocks_used;
	result = get_uint64_le_from_buffer(buffer, &logical_blocks_used);
	if (result != UDS_SUCCESS) {
		return result;
	}

	block_count_t block_map_data_blocks;
	result = get_uint64_le_from_buffer(buffer, &block_map_data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	size_t decoded_size = initial_length - content_length(buffer);
	result = ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == decoded_size,
			"decoded recovery journal component size must match header size");
	if (result != UDS_SUCCESS) {
		return result;
	}

	*state = (struct recovery_journal_state_7_0) {
		.journal_start = journal_start,
		.logical_blocks_used = logical_blocks_used,
		.block_map_data_blocks = block_map_data_blocks,
	};

	return VDO_SUCCESS;
}
