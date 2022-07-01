/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RECOVERY_UTILS_H
#define RECOVERY_UTILS_H

#include "constants.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal-entry.h"
#include "recovery-journal.h"
#include "types.h"

/**
 * Get the block header for a block at a position in the journal data.
 *
 * @param journal       The recovery journal
 * @param journal_data  The recovery journal data
 * @param sequence      The sequence number
 *
 * @return A pointer to a packed recovery journal blokck header.
 **/
static inline struct packed_journal_header * __must_check
vdo_get_recovery_journal_block_header(struct recovery_journal *journal,
				      char *journal_data,
				      sequence_number_t sequence)
{
	off_t block_offset =
		(vdo_get_recovery_journal_block_number(journal, sequence)
		* VDO_BLOCK_SIZE);
	return (struct packed_journal_header *) &journal_data[block_offset];
}

/**
 * Determine whether the given header describes a valid block for the
 * given journal. A block is not valid if it is unformatted, or if it
 * is older than the last successful recovery or reformat.
 *
 * @param journal  The journal to use
 * @param header   The unpacked block header to check
 *
 * @return <code>True</code> if the header is valid
 **/
static inline bool __must_check
vdo_is_valid_recovery_journal_block(const struct recovery_journal *journal,
				    const struct recovery_block_header *header)
{
	return ((header->metadata_type == VDO_METADATA_RECOVERY_JOURNAL)
		&& (header->nonce == journal->nonce)
		&& (header->recovery_count == journal->recovery_count));
}

/**
 * Determine whether the given header describes the exact block indicated.
 *
 * @param journal   The journal to use
 * @param header    The unpacked block header to check
 * @param sequence  The expected sequence number
 *
 * @return <code>True</code> if the block matches
 **/
static inline bool __must_check
vdo_is_exact_recovery_journal_block(const struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    sequence_number_t sequence)
{
	return ((header->sequence_number == sequence)
		&& vdo_is_valid_recovery_journal_block(journal, header));
}

void vdo_load_recovery_journal(struct recovery_journal *journal,
			       struct vdo_completion *parent,
			       char **journal_data_ptr);

bool
vdo_find_recovery_journal_head_and_tail(struct recovery_journal *journal,
					char *journal_data,
					sequence_number_t *tail_ptr,
					sequence_number_t *block_map_head_ptr,
					sequence_number_t *slab_journal_head_ptr);

int __must_check
vdo_validate_recovery_journal_entry(const struct vdo *vdo,
				    const struct recovery_journal_entry *entry);

#endif /* RECOVERY_UTILS_H */
