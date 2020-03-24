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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryUtils.h#8 $
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
__attribute__((warn_unused_result))
static inline union packed_journal_header *
get_journal_block_header(struct recovery_journal *journal, char *journal_data,
			 SequenceNumber sequence)
{
	off_t block_offset = (get_recovery_journal_block_number(journal,
								sequence)
			      * VDO_BLOCK_SIZE);
	return (union packed_journal_header *) &journal_data[block_offset];
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
__attribute__((warn_unused_result)) static inline bool
is_valid_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header)
{
	return ((header->metadataType == VDO_METADATA_RECOVERY_JOURNAL)
		&& (header->nonce == journal->nonce)
		&& (header->recoveryCount == journal->recovery_count));
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
__attribute__((warn_unused_result)) static inline bool
is_exact_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header,
				SequenceNumber sequence)
{
	return ((header->sequenceNumber == sequence)
		&& is_valid_recovery_journal_block(journal, header));
}

/**
 * Determine whether the header of the given sector could describe a
 * valid sector for the given journal block header.
 *
 * @param header  The unpacked block header to compare against
 * @param sector  The packed sector to check
 *
 * @return <code>True</code> if the sector matches the block header
 **/
__attribute__((warn_unused_result)) static inline bool
is_valid_recovery_journal_sector(const struct recovery_block_header *header,
				 const struct packed_journal_sector *sector)
{
	return ((header->checkByte == sector->checkByte)
		&& (header->recoveryCount == sector->recoveryCount));
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
void load_journal_async(struct recovery_journal *journal,
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
bool find_head_and_tail(struct recovery_journal *journal, char *journal_data,
			SequenceNumber *tail_ptr,
			SequenceNumber *block_map_head_ptr,
			SequenceNumber *slab_journal_head_ptr);

/**
 * Validate a recovery journal entry.
 *
 * @param vdo    The vdo
 * @param entry  The entry to validate
 *
 * @return VDO_SUCCESS or an error
 **/
int validate_recovery_journal_entry(const struct vdo *vdo,
				    const struct recovery_journal_entry *entry)
	__attribute__((warn_unused_result));

#endif // RECOVERY_UTILS_H
