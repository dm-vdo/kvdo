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

#include "recovery-journal-format.h"

#include "buffer.h"
#include "permassert.h"

#include "fixed-layout.h"
#include "header.h"
#include "status-codes.h"
#include "types.h"

const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = VDO_RECOVERY_JOURNAL,
	.version = {
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

/**
 * Get the size of the encoded state of a recovery journal.
 *
 * @return the encoded size of the journal's state
 **/
size_t get_vdo_recovery_journal_encoded_size(void)
{
	return VDO_ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0);
}

/**
 * Encode the state of a recovery journal.
 *
 * @param state   the recovery journal state
 * @param buffer  the buffer to encode into
 *
 * @return VDO_SUCCESS or an error code
 **/
int encode_vdo_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
					  struct buffer *buffer)
{
	size_t initial_length, encoded_size;

	int result = encode_vdo_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, buffer);

	if (result != UDS_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

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

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == encoded_size,
		      "encoded recovery journal component size must match header size");
}

/**
 * Decode the state of a recovery journal saved in a buffer.
 *
 * @param buffer  the buffer containing the saved state
 * @param state   a pointer to a recovery journal state to hold the result of a
 *                succesful decode
 *
 * @return VDO_SUCCESS or an error code
 **/
int
decode_vdo_recovery_journal_state_7_0(struct buffer *buffer,
				      struct recovery_journal_state_7_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	sequence_number_t journal_start;
	block_count_t logical_blocks_used, block_map_data_blocks;

	result = decode_vdo_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_vdo_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, &header,
				     true, __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = get_uint64_le_from_buffer(buffer, &journal_start);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &logical_blocks_used);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &block_map_data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == decoded_size,
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

/**
 * Get the name of a journal operation.
 *
 * @param operation  The operation to name
 *
 * @return The name of the operation
 **/
const char *get_vdo_journal_operation_name(enum journal_operation operation)
{
	switch (operation) {
	case VDO_JOURNAL_DATA_DECREMENT:
		return "data decrement";

	case VDO_JOURNAL_DATA_INCREMENT:
		return "data increment";

	case VDO_JOURNAL_BLOCK_MAP_DECREMENT:
		return "block map decrement";

	case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
		return "block map increment";

	default:
		return "unknown journal operation";
	}
}
