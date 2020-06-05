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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalFormat.h#1 $
 */

#ifndef RECOVERY_JOURNAL_FORMAT_H
#define RECOVERY_JOURNAL_FORMAT_H

#include "buffer.h"

#include "header.h"
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
} __attribute__((packed));

extern const struct header RECOVERY_JOURNAL_HEADER_7_0;

/**
 * Get the size of the encoded state of a recovery journal.
 *
 * @return the encoded size of the journal's state
 **/
size_t __must_check get_recovery_journal_encoded_size(void);

/**
 * Encode the state of a recovery journal.
 *
 * @param state   the recovery journal state
 * @param buffer  the buffer to encode into
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
				  struct buffer *buffer);

/**
 * Decode the state of a recovery journal saved in a buffer.
 *
 * @param buffer  the buffer containing the saved state
 * @param state   a pointer to a recovery journal state to hold the result of a
 *                succesful decode
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
decode_recovery_journal_state_7_0(struct buffer *buffer,
				  struct recovery_journal_state_7_0 *state);


#endif // RECOVERY_JOURNAL_FORMAT_H
