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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/packedRecoveryJournalBlock.h#15 $
 */

#ifndef PACKED_RECOVERY_JOURNAL_BLOCK_H
#define PACKED_RECOVERY_JOURNAL_BLOCK_H

#include "numeric.h"

#include "constants.h"
#include "recoveryJournalEntry.h"
#include "types.h"

struct recovery_block_header {
	sequence_number_t block_map_head; // Block map head sequence number
	sequence_number_t slab_journal_head; // Slab journal head seq. number
	sequence_number_t sequence_number; // Sequence number for this block
	nonce_t nonce; // A given VDO instance's nonce
	block_count_t logical_blocks_used; // Logical blocks in use
	block_count_t block_map_data_blocks; // Allocated block map pages
	JournalEntryCount entry_count; // Number of entries written
	uint8_t check_byte; // The protection check byte
	uint8_t recovery_count; // Number of recoveries completed
	vdo_metadata_type metadata_type; // Metadata type
};

/**
 * The packed, on-disk representation of a recovery journal block header.
 * All fields are kept in little-endian byte order.
 **/
union packed_journal_header {
	struct __packed {
		/** Block map head 64-bit sequence number */
		byte block_map_head[8];

		/** Slab journal head 64-bit sequence number */
		byte slab_journal_head[8];

		/** The 64-bit sequence number for this block */
		byte sequence_number[8];

		/** A given VDO instance's 64-bit nonce */
		byte nonce[8];

		/**
		 * 8-bit metadata type (should always be one for the recovery
		 * journal)
		 */
		uint8_t metadata_type;

		/** 16-bit count of the entries encoded in the block */
		byte entry_count[2];

		/**
		 * 64-bit count of the logical blocks used when this block was
		 * opened
		 */
		byte logical_blocks_used[8];

		/**
		 * 64-bit count of the block map blocks used when this block
		 * was opened
		 */
		byte block_map_data_blocks[8];

		/** The protection check byte */
		uint8_t check_byte;

		/** The number of recoveries completed */
		uint8_t recovery_count;
	} fields;

	// A raw view of the packed encoding.
	uint8_t raw[8 + 8 + 8 + 8 + 1 + 2 + 8 + 8 + 1 + 1];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// This view is only valid on little-endian machines and is only
	// present for ease of directly examining packed entries in GDB.
	struct __packed {
		sequence_number_t block_map_head;
		sequence_number_t slab_journal_head;
		sequence_number_t sequence_number;
		nonce_t nonce;
		vdo_metadata_type metadata_type;
		JournalEntryCount entry_count;
		block_count_t logical_blocks_used;
		block_count_t block_map_data_blocks;
		uint8_t check_byte;
		uint8_t recovery_count;
	} little_endian;
#endif
} __packed;

struct packed_journal_sector {
	/** The protection check byte */
	uint8_t check_byte;

	/** The number of recoveries completed */
	uint8_t recovery_count;

	/** The number of entries in this sector */
	uint8_t entry_count;

	/** Journal entries for this sector */
	packed_recovery_journal_entry entries[];
} __packed;

enum {
	// Allowing more than 311 entries in each block changes the math
	// concerning the amortization of metadata writes and recovery speed.
	RECOVERY_JOURNAL_ENTRIES_PER_BLOCK = 311,
	/**
	 * The number of entries in each sector (except the last) when filled
	 */
	RECOVERY_JOURNAL_ENTRIES_PER_SECTOR =
		((VDO_SECTOR_SIZE - sizeof(struct packed_journal_sector)) /
		 sizeof(packed_recovery_journal_entry)),
	/** The number of entries in the last sector when a block is full */
	RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR =
		(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK %
		 RECOVERY_JOURNAL_ENTRIES_PER_SECTOR),
};

/**
 * Find the recovery journal sector from the block header and sector number.
 *
 * @param header         The header of the recovery journal block
 * @param sector_number  The index of the sector (1-based)
 *
 * @return A packed recovery journal sector
 **/
static inline struct packed_journal_sector * __must_check
get_journal_block_sector(union packed_journal_header *header,
			 int sector_number)
{
	char *sector_data =
		((char *) header) + (VDO_SECTOR_SIZE * sector_number);
	return (struct packed_journal_sector *) sector_data;
}

/**
 * Generate the packed representation of a recovery block header.
 *
 * @param header  The header containing the values to encode
 * @param packed  The header into which to pack the values
 **/
static inline void
pack_recovery_block_header(const struct recovery_block_header *header,
			   union packed_journal_header *packed)
{
	put_unaligned_le64(header->block_map_head,
			   packed->fields.block_map_head);
	put_unaligned_le64(header->slab_journal_head,
			   packed->fields.slab_journal_head);
	put_unaligned_le64(header->sequence_number,
			   packed->fields.sequence_number);
	put_unaligned_le64(header->nonce, packed->fields.nonce);
	put_unaligned_le64(header->logical_blocks_used,
			   packed->fields.logical_blocks_used);
	put_unaligned_le64(header->block_map_data_blocks,
			   packed->fields.block_map_data_blocks);
	put_unaligned_le16(header->entry_count, packed->fields.entry_count);

	packed->fields.check_byte = header->check_byte;
	packed->fields.recovery_count = header->recovery_count;
	packed->fields.metadata_type = header->metadata_type;
}

/**
 * Decode the packed representation of a recovery block header.
 *
 * @param packed  The packed header to decode
 * @param header  The header into which to unpack the values
 **/
static inline void
unpack_recovery_block_header(const union packed_journal_header *packed,
			     struct recovery_block_header *header)
{
	*header = (struct recovery_block_header) {
		.block_map_head =
			get_unaligned_le64(packed->fields.block_map_head),
		.slab_journal_head =
			get_unaligned_le64(packed->fields.slab_journal_head),
		.sequence_number =
			get_unaligned_le64(packed->fields.sequence_number),
		.nonce = get_unaligned_le64(packed->fields.nonce),
		.logical_blocks_used =
			get_unaligned_le64(packed->fields.logical_blocks_used),
		.block_map_data_blocks =
			get_unaligned_le64(packed->fields.block_map_data_blocks),
		.entry_count = get_unaligned_le16(packed->fields.entry_count),
		.check_byte = packed->fields.check_byte,
		.recovery_count = packed->fields.recovery_count,
		.metadata_type = packed->fields.metadata_type,
	};
}

#endif // PACKED_RECOVERY_JOURNAL_BLOCK_H
