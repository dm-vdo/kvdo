// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "recovery-journal-format.h"

#include "buffer.h"
#include "permassert.h"

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
 * vdo_get_recovery_journal_encoded_size() - Get the size of the encoded state
 *                                           of a recovery journal.
 *
 * Return: the encoded size of the journal's state.
 */
size_t vdo_get_recovery_journal_encoded_size(void)
{
	return VDO_ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0);
}

/**
 * vdo_encode_recovery_journal_state_7_0() - Encode the state of a recovery
 *                                           journal.
 * @state: The recovery journal state.
 * @buffer: The buffer to encode into.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
					  struct buffer *buffer)
{
	size_t initial_length, encoded_size;

	int result = vdo_encode_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, buffer);

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
 * vdo_decode_recovery_journal_state_7_0() - Decode the state of a recovery
 *                                           journal saved in a buffer.
 * @buffer: The buffer containing the saved state.
 * @state: A pointer to a recovery journal state to hold the result of a
 *         succesful decode.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int
vdo_decode_recovery_journal_state_7_0(struct buffer *buffer,
				      struct recovery_journal_state_7_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	sequence_number_t journal_start;
	block_count_t logical_blocks_used, block_map_data_blocks;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_validate_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, &header,
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
 * vdo_get_journal_operation_name() - Get the name of a journal operation.
 * @operation: The operation to name.
 *
 * Return: The name of the operation.
 */
const char *vdo_get_journal_operation_name(enum journal_operation operation)
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
