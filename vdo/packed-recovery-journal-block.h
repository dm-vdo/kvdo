/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PACKED_RECOVERY_JOURNAL_BLOCK_H
#define PACKED_RECOVERY_JOURNAL_BLOCK_H

#include "numeric.h"

#include "constants.h"
#include "recovery-journal-entry.h"
#include "types.h"

struct recovery_block_header {
	sequence_number_t block_map_head; /* Block map head sequence number */
	sequence_number_t slab_journal_head; /* Slab journal head seq. number */
	sequence_number_t sequence_number; /* Sequence number for this block */
	nonce_t nonce; /* A given VDO instance's nonce */
	block_count_t logical_blocks_used; /* Logical blocks in use */
	block_count_t block_map_data_blocks; /* Allocated block map pages */
	journal_entry_count_t entry_count; /* Number of entries written */
	uint8_t check_byte; /* The protection check byte */
	uint8_t recovery_count; /* Number of recoveries completed */
	enum vdo_metadata_type metadata_type; /* Metadata type */
};

/*
 * The packed, on-disk representation of a recovery journal block header.
 * All fields are kept in little-endian byte order.
 */
struct packed_journal_header {
	/* Block map head 64-bit sequence number */
	__le64 block_map_head;

	/* Slab journal head 64-bit sequence number */
	__le64 slab_journal_head;

	/* The 64-bit sequence number for this block */
	__le64 sequence_number;

	/* A given VDO instance's 64-bit nonce */
	__le64 nonce;

	/*
	 * 8-bit metadata type (should always be one for the recovery
	 * journal)
	 */
	uint8_t metadata_type;

	/* 16-bit count of the entries encoded in the block */
	__le16 entry_count;

	/*
	 * 64-bit count of the logical blocks used when this block was
	 * opened
	 */
	__le64 logical_blocks_used;

	/*
	 * 64-bit count of the block map blocks used when this block
	 * was opened
	 */
	__le64 block_map_data_blocks;

	/* The protection check byte */
	uint8_t check_byte;

	/* The number of recoveries completed */
	uint8_t recovery_count;
} __packed;

struct packed_journal_sector {
	/* The protection check byte */
	uint8_t check_byte;

	/* The number of recoveries completed */
	uint8_t recovery_count;

	/* The number of entries in this sector */
	uint8_t entry_count;

	/* Journal entries for this sector */
	struct packed_recovery_journal_entry entries[];
} __packed;

enum {
	/*
	 * Allowing more than 311 entries in each block changes the math
	 * concerning the amortization of metadata writes and recovery speed.
	 */
	RECOVERY_JOURNAL_ENTRIES_PER_BLOCK = 311,
	/*
	 * The number of entries in each sector (except the last) when filled
	 */
	RECOVERY_JOURNAL_ENTRIES_PER_SECTOR =
		((VDO_SECTOR_SIZE - sizeof(struct packed_journal_sector)) /
		 sizeof(struct packed_recovery_journal_entry)),
	/* The number of entries in the last sector when a block is full */
	RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR =
		(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK %
		 RECOVERY_JOURNAL_ENTRIES_PER_SECTOR),
};

/**
 * vdo_get_journal_block_sector() - Find the recovery journal sector from the
 *                                  block header and sector number.
 * @header: The header of the recovery journal block.
 * @sector_number: The index of the sector (1-based).
 *
 * Return: A packed recovery journal sector.
 */
static inline struct packed_journal_sector * __must_check
vdo_get_journal_block_sector(struct packed_journal_header *header,
			     int sector_number)
{
	char *sector_data =
		((char *) header) + (VDO_SECTOR_SIZE * sector_number);
	return (struct packed_journal_sector *) sector_data;
}

/**
 * vdo_pack_recovery_block_header() - Generate the packed representation of a
 *                                    recovery block header.
 * @header: The header containing the values to encode.
 * @packed: The header into which to pack the values.
 */
static inline void
vdo_pack_recovery_block_header(const struct recovery_block_header *header,
			       struct packed_journal_header *packed)
{
	*packed = (struct packed_journal_header) {
		.block_map_head = __cpu_to_le64(header->block_map_head),
		.slab_journal_head = __cpu_to_le64(header->slab_journal_head),
		.sequence_number = __cpu_to_le64(header->sequence_number),
		.nonce = __cpu_to_le64(header->nonce),
		.logical_blocks_used =
			__cpu_to_le64(header->logical_blocks_used),
		.block_map_data_blocks =
			__cpu_to_le64(header->block_map_data_blocks),
		.entry_count = __cpu_to_le16(header->entry_count),
		.check_byte = header->check_byte,
		.recovery_count = header->recovery_count,
		.metadata_type = header->metadata_type,
	};
}

/**
 * vdo_unpack_recovery_block_header() - Decode the packed representation of a
 *                                      recovery block header.
 * @packed: The packed header to decode.
 * @header: The header into which to unpack the values.
 */
static inline void
vdo_unpack_recovery_block_header(const struct packed_journal_header *packed,
				 struct recovery_block_header *header)
{
	*header = (struct recovery_block_header) {
		.block_map_head = __le64_to_cpu(packed->block_map_head),
		.slab_journal_head = __le64_to_cpu(packed->slab_journal_head),
		.sequence_number = __le64_to_cpu(packed->sequence_number),
		.nonce = __le64_to_cpu(packed->nonce),
		.logical_blocks_used =
			__le64_to_cpu(packed->logical_blocks_used),
		.block_map_data_blocks =
			__le64_to_cpu(packed->block_map_data_blocks),
		.entry_count = __le16_to_cpu(packed->entry_count),
		.check_byte = packed->check_byte,
		.recovery_count = packed->recovery_count,
		.metadata_type = packed->metadata_type,
	};
}

#endif /* PACKED_RECOVERY_JOURNAL_BLOCK_H */
