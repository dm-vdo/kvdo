/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RECOVERY_JOURNAL_FORMAT_H
#define RECOVERY_JOURNAL_FORMAT_H

#include "buffer.h"

#include "header.h"
#include "packed-recovery-journal-block.h"
#include "types.h"

/*
 * The state of the recovery journal as encoded in the VDO super block.
 */
struct recovery_journal_state_7_0 {
	/** Sequence number to start the journal */
	sequence_number_t journal_start;
	/** Number of logical blocks used by VDO */
	block_count_t logical_blocks_used;
	/** Number of block map pages allocated */
	block_count_t block_map_data_blocks;
} __packed;

extern const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0;

size_t __must_check vdo_get_recovery_journal_encoded_size(void);

int __must_check
vdo_encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
				      struct buffer *buffer);

int __must_check
vdo_decode_recovery_journal_state_7_0(struct buffer *buffer,
				      struct recovery_journal_state_7_0 *state);

const char * __must_check
vdo_get_journal_operation_name(enum journal_operation operation);

/**
 * vdo_is_valid_recovery_journal_sector() - Determine whether the header of
 *                                          the given sector could describe a
 *                                          valid sector for the given journal
 *                                          block header.
 * @header: The unpacked block header to compare against.
 * @sector: The packed sector to check.
 *
 * Return: true if the sector matches the block header.
 */
static inline bool __must_check
vdo_is_valid_recovery_journal_sector(const struct recovery_block_header *header,
				     const struct packed_journal_sector *sector)
{
	return ((header->check_byte == sector->check_byte)
		&& (header->recovery_count == sector->recovery_count));
}

/**
 * vdo_compute_recovery_journal_block_number() - Compute the physical block
 *                                               number of the recovery
 *                                               journal block which would
 *                                               have a given sequence number.
 * @journal_size: The size of the journal.
 * @sequence_number: The sequence number.
 *
 * Return: The pbn of the journal block which would the specified sequence
 *         number.
 */
static inline physical_block_number_t __must_check
vdo_compute_recovery_journal_block_number(block_count_t journal_size,
					  sequence_number_t sequence_number)
{
	/*
	 * Since journal size is a power of two, the block number modulus can
	 * just be extracted from the low-order bits of the sequence.
	 */
	return (sequence_number & (journal_size - 1));
}

#endif /* RECOVERY_JOURNAL_FORMAT_H */
