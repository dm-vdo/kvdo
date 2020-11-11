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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournalFormat.h#3 $
 */

#ifndef SLAB_JOURNAL_FORMAT_H
#define SLAB_JOURNAL_FORMAT_H

#include "constants.h"
#include "journalPoint.h"
#include "types.h"

/**
 * vdo_slab journal blocks may have one of two formats, depending upon whether
 * or not any of the entries in the block are block map increments. Since the
 * steady state for a VDO is that all of the necessary block map pages will be
 * allocated, most slab journal blocks will have only data entries. Such
 * blocks can hold more entries, hence the two formats.
 **/

/** A single slab journal entry */
struct slab_journal_entry {
	slab_block_number sbn;
	journal_operation operation;
};

/** A single slab journal entry in its on-disk form */
typedef union {
	struct __packed {
		uint8_t offset_low8;
		uint8_t offset_mid8;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		unsigned offset_high7 : 7;
		unsigned increment : 1;
#else
		unsigned increment : 1;
		unsigned offset_high7 : 7;
#endif
	} fields;

	// A raw view of the packed encoding.
	uint8_t raw[3];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// This view is only valid on little-endian machines and is only present
	// for ease of directly examining packed entries in GDB.
	struct __packed {
		unsigned offset : 23;
		unsigned increment : 1;
	} little_endian;
#endif
} __packed packed_slab_journal_entry;

/** The unpacked representation of the header of a slab journal block */
struct slab_journal_block_header {
	/** Sequence number for head of journal */
	sequence_number_t head;
	/** Sequence number for this block */
	sequence_number_t sequence_number;
	/** The nonce for a given VDO instance */
	nonce_t nonce;
	/** Recovery journal point for last entry */
	struct journal_point recovery_point;
	/** Metadata type */
	vdo_metadata_type metadata_type;
	/** Whether this block contains block map increments */
	bool has_block_map_increments;
	/** The number of entries in the block */
	JournalEntryCount entry_count;
};

/**
 * The packed, on-disk representation of a slab journal block header.
 * All fields are kept in little-endian byte order.
 **/
typedef union __packed {
	struct __packed {
		/** 64-bit sequence number for head of journal */
		byte head[8];
		/** 64-bit sequence number for this block */
		byte sequence_number[8];
		/**
		 * Recovery journal point for last entry, packed into 64 bits
		 */
		struct packed_journal_point recovery_point;
		/** The 64-bit nonce for a given VDO instance */
		byte nonce[8];
		/**
		 * 8-bit metadata type (should always be two, for the slab
		 * journal)
		 */
		uint8_t metadata_type;
		/** Whether this block contains block map increments */
		bool has_block_map_increments;
		/** 16-bit count of the entries encoded in the block */
		byte entry_count[2];
	} fields;

	// A raw view of the packed encoding.
	uint8_t raw[8 + 8 + 8 + 8 + 1 + 1 + 2];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// This view is only valid on little-endian machines and is only present
	// for ease of directly examining packed entries in GDB.
	struct __packed {
		sequence_number_t head;
		sequence_number_t sequence_number;
		struct packed_journal_point recovery_point;
		nonce_t nonce;
		vdo_metadata_type metadata_type;
		bool has_block_map_increments;
		JournalEntryCount entry_count;
	} little_endian;
#endif
} packed_slab_journal_block_header;

enum {
	SLAB_JOURNAL_PAYLOAD_SIZE =
		VDO_BLOCK_SIZE - sizeof(packed_slab_journal_block_header),
	SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK =
		(SLAB_JOURNAL_PAYLOAD_SIZE * 8) / 25,
	SLAB_JOURNAL_ENTRY_TYPES_SIZE =
		((SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK - 1) / 8) + 1,
	SLAB_JOURNAL_ENTRIES_PER_BLOCK =
		(SLAB_JOURNAL_PAYLOAD_SIZE / sizeof(packed_slab_journal_entry)),
};

/** The payload of a slab journal block which has block map increments */
struct full_slab_journal_entries {
	/* The entries themselves */
	packed_slab_journal_entry entries[SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK];
	/* The bit map indicating which entries are block map increments */
	byte entry_types[SLAB_JOURNAL_ENTRY_TYPES_SIZE];
} __packed;

typedef union {
	/* Entries which include block map increments */
	struct full_slab_journal_entries full_entries;
	/* Entries which are only data updates */
	packed_slab_journal_entry entries[SLAB_JOURNAL_ENTRIES_PER_BLOCK];
	/* Ensure the payload fills to the end of the block */
	byte space[SLAB_JOURNAL_PAYLOAD_SIZE];
} __packed slab_journal_payload;

struct packed_slab_journal_block {
	packed_slab_journal_block_header header;
	slab_journal_payload payload;
} __packed;

/**
 * Get the physical block number of the start of the slab journal
 * relative to the start block allocator partition.
 *
 * @param slab_config  The slab configuration of the VDO
 * @param origin       The first block of the slab
 **/
static inline physical_block_number_t __must_check
get_slab_journal_start_block(const struct slab_config *slab_config,
			     physical_block_number_t origin)
{
	return origin + slab_config->data_blocks
	       + slab_config->reference_count_blocks;
}

/**
 * Generate the packed representation of a slab block header.
 *
 * @param header  The header containing the values to encode
 * @param packed  The header into which to pack the values
 **/
static inline void
pack_slab_journal_block_header(const struct slab_journal_block_header *header,
			       packed_slab_journal_block_header *packed)
{
	put_unaligned_le64(header->head, packed->fields.head);
	put_unaligned_le64(header->sequence_number,
			   packed->fields.sequence_number);
	put_unaligned_le64(header->nonce, packed->fields.nonce);
	put_unaligned_le16(header->entry_count, packed->fields.entry_count);

	packed->fields.metadata_type = header->metadata_type;
	packed->fields.has_block_map_increments = header->has_block_map_increments;

	pack_journal_point(&header->recovery_point,
			   &packed->fields.recovery_point);
}

/**
 * Decode the packed representation of a slab journal entry.
 *
 * @param packed  The packed entry to decode
 *
 * @return The decoded slab journal entry
 **/
static inline struct slab_journal_entry __must_check
unpack_slab_journal_entry(const packed_slab_journal_entry *packed)
{
	struct slab_journal_entry entry;
	entry.sbn = packed->fields.offset_high7;
	entry.sbn <<= 8;
	entry.sbn |= packed->fields.offset_mid8;
	entry.sbn <<= 8;
	entry.sbn |= packed->fields.offset_low8;
	entry.operation =
		(packed->fields.increment ? DATA_INCREMENT : DATA_DECREMENT);
	return entry;
}

/**
 * Decode a slab journal entry.
 *
 * @param block         The journal block holding the entry
 * @param entry_count   The number of the entry
 *
 * @return The decoded entry
 **/
struct slab_journal_entry __must_check
decode_slab_journal_entry(struct packed_slab_journal_block *block,
			  JournalEntryCount entry_count);


#endif // SLAB_JOURNAL_FORMAT_H
