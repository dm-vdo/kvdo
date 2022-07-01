/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_JOURNAL_FORMAT_H
#define SLAB_JOURNAL_FORMAT_H

#include "numeric.h"

#include "constants.h"
#include "journal-point.h"
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
	enum journal_operation operation;
};

/** A single slab journal entry in its on-disk form */
typedef struct {
	uint8_t offset_low8;
	uint8_t offset_mid8;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned offset_high7 : 7;
	unsigned increment : 1;
#else
	unsigned increment : 1;
	unsigned offset_high7 : 7;
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
	enum vdo_metadata_type metadata_type;
	/** Whether this block contains block map increments */
	bool has_block_map_increments;
	/** The number of entries in the block */
	journal_entry_count_t entry_count;
};

/**
 * The packed, on-disk representation of a slab journal block header.
 * All fields are kept in little-endian byte order.
 **/
struct packed_slab_journal_block_header {
	/** 64-bit sequence number for head of journal */
	__le64 head;
	/** 64-bit sequence number for this block */
	__le64 sequence_number;
	/** Recovery journal point for the last entry, packed into 64 bits */
	struct packed_journal_point recovery_point;
	/** The 64-bit nonce for a given VDO instance */
	__le64 nonce;
	/** 8-bit metadata type (should always be two, for the slab journal) */
	uint8_t metadata_type;
	/** Whether this block contains block map increments */
	bool has_block_map_increments;
	/** 16-bit count of the entries encoded in the block */
	__le16 entry_count;
} __packed;

enum {
	VDO_SLAB_JOURNAL_PAYLOAD_SIZE =
		VDO_BLOCK_SIZE - sizeof(struct packed_slab_journal_block_header),
	VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK =
		(VDO_SLAB_JOURNAL_PAYLOAD_SIZE * 8) / 25,
	VDO_SLAB_JOURNAL_ENTRY_TYPES_SIZE =
		((VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK - 1) / 8) + 1,
	VDO_SLAB_JOURNAL_ENTRIES_PER_BLOCK =
		(VDO_SLAB_JOURNAL_PAYLOAD_SIZE / sizeof(packed_slab_journal_entry)),
};

/** The payload of a slab journal block which has block map increments */
struct full_slab_journal_entries {
	/* The entries themselves */
	packed_slab_journal_entry entries[VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK];
	/* The bit map indicating which entries are block map increments */
	byte entry_types[VDO_SLAB_JOURNAL_ENTRY_TYPES_SIZE];
} __packed;

typedef union {
	/* Entries which include block map increments */
	struct full_slab_journal_entries full_entries;
	/* Entries which are only data updates */
	packed_slab_journal_entry entries[VDO_SLAB_JOURNAL_ENTRIES_PER_BLOCK];
	/* Ensure the payload fills to the end of the block */
	byte space[VDO_SLAB_JOURNAL_PAYLOAD_SIZE];
} __packed slab_journal_payload;

struct packed_slab_journal_block {
	struct packed_slab_journal_block_header header;
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
vdo_get_slab_journal_start_block(const struct slab_config *slab_config,
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
vdo_pack_slab_journal_block_header(const struct slab_journal_block_header *header,
				   struct packed_slab_journal_block_header *packed)
{
	packed->head = __cpu_to_le64(header->head);
	packed->sequence_number = __cpu_to_le64(header->sequence_number);
	packed->nonce = __cpu_to_le64(header->nonce);
	packed->entry_count = __cpu_to_le16(header->entry_count);
	packed->metadata_type = header->metadata_type;
	packed->has_block_map_increments = header->has_block_map_increments;

	vdo_pack_journal_point(&header->recovery_point,
			       &packed->recovery_point);
}

/**
 * Decode the packed representation of a slab journal entry.
 *
 * @param packed  The packed entry to decode
 *
 * @return The decoded slab journal entry
 **/
static inline struct slab_journal_entry __must_check
vdo_unpack_slab_journal_entry(const packed_slab_journal_entry *packed)
{
	struct slab_journal_entry entry;

	entry.sbn = packed->offset_high7;
	entry.sbn <<= 8;
	entry.sbn |= packed->offset_mid8;
	entry.sbn <<= 8;
	entry.sbn |= packed->offset_low8;
	entry.operation = (packed->increment ? VDO_JOURNAL_DATA_INCREMENT
					     : VDO_JOURNAL_DATA_DECREMENT);
	return entry;
}

struct slab_journal_entry __must_check
vdo_decode_slab_journal_entry(struct packed_slab_journal_block *block,
			      journal_entry_count_t entry_count);


#endif /* SLAB_JOURNAL_FORMAT_H */
