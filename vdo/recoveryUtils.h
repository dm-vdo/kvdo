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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryUtils.h#16 $
 */

#ifndef RECOVERY_UTILS_H
#define RECOVERY_UTILS_H

#include "constants.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
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
get_vdo_recovery_journal_block_header(struct recovery_journal *journal,
				      char *journal_data,
				      sequence_number_t sequence)
{
	off_t block_offset =
		(get_vdo_recovery_journal_block_number(journal, sequence)
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
is_valid_vdo_recovery_journal_block(const struct recovery_journal *journal,
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
is_exact_vdo_recovery_journal_block(const struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    sequence_number_t sequence)
{
	return ((header->sequence_number == sequence)
		&& is_valid_vdo_recovery_journal_block(journal, header));
}

/**
 * Load the journal data off the disk.
 *
 * @param [in]  journal           The recovery journal to load
 * @param [in]  parent            The completion to notify when the load is
 *                                complete
 * @param [out] journal_data_ptr  A pointer to the journal data buffer (it is
 *                                the caller's responsibility to free this
 *                                buffer)
 **/
void load_vdo_recovery_journal(struct recovery_journal *journal,
			       struct vdo_completion *parent,
			       char **journal_data_ptr);

/**
 * Find the tail and the head of the journal by searching for the highest
 * sequence number in a block with a valid nonce, and the highest head value
 * among the blocks with valid nonces.
 *
 * @param [in]  journal                The recovery journal
 * @param [in]  journal_data           The journal data read from disk
 * @param [out] tail_ptr               A pointer to return the tail found, or if
 *                                     no higher block is found, the value
 *                                     currently in the journal
 * @param [out] block_map_head_ptr     A pointer to return the block map head
 * @param [out] slab_journal_head_ptr  An optional pointer to return the slab
 *                                     journal head
 *
 * @return  <code>True</code> if there were valid journal blocks
 **/
bool
find_vdo_recovery_journal_head_and_tail(struct recovery_journal *journal,
					char *journal_data,
					sequence_number_t *tail_ptr,
					sequence_number_t *block_map_head_ptr,
					sequence_number_t *slab_journal_head_ptr);

/**
 * Validate a recovery journal entry.
 *
 * @param vdo    The vdo
 * @param entry  The entry to validate
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
validate_vdo_recovery_journal_entry(const struct vdo *vdo,
				    const struct recovery_journal_entry *entry);

#endif // RECOVERY_UTILS_H
