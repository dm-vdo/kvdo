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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournalInternals.h#20 $
 */

#ifndef SLAB_JOURNAL_INTERNALS_H
#define SLAB_JOURNAL_INTERNALS_H

#include "slabJournal.h"

#include "numeric.h"

#include "blockAllocatorInternals.h"
#include "blockMapEntry.h"
#include "journalPoint.h"
#include "slab.h"
#include "slabSummary.h"
#include "statistics.h"
#include "waitQueue.h"

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
	struct __attribute__((packed)) {
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
	struct __attribute__((packed)) {
		unsigned offset : 23;
		unsigned increment : 1;
	} little_endian;
#endif
} __attribute__((packed)) packed_slab_journal_entry;

/** The unpacked representation of the header of a slab journal block */
struct slab_journal_block_header {
	/** Sequence number for head of journal */
	SequenceNumber head;
	/** Sequence number for this block */
	SequenceNumber sequence_number;
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
typedef union __attribute__((packed)) {
	struct __attribute__((packed)) {
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
	struct __attribute__((packed)) {
		SequenceNumber head;
		SequenceNumber sequence_number;
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
} __attribute__((packed));

typedef union {
	/* Entries which include block map increments */
	struct full_slab_journal_entries full_entries;
	/* Entries which are only data updates */
	packed_slab_journal_entry entries[SLAB_JOURNAL_ENTRIES_PER_BLOCK];
	/* Ensure the payload fills to the end of the block */
	byte space[SLAB_JOURNAL_PAYLOAD_SIZE];
} __attribute__((packed)) slab_journal_payload;

struct packed_slab_journal_block {
	packed_slab_journal_block_header header;
	slab_journal_payload payload;
} __attribute__((packed));

struct journal_lock {
	uint16_t count;
	SequenceNumber recovery_start;
};

struct slab_journal {
	/** A waiter object for getting a VIO pool entry */
	struct waiter resource_waiter;
	/** A waiter object for updating the slab summary */
	struct waiter slab_summary_waiter;
	/** A waiter object for getting an extent with which to flush */
	struct waiter flush_waiter;
	/** The queue of VIOs waiting to make an entry */
	struct wait_queue entry_waiters;
	/** The parent slab reference of this journal */
	struct vdo_slab *slab;

	/** Whether a tail block commit is pending */
	bool waiting_to_commit;
	/** Whether the journal is updating the slab summary */
	bool updating_slab_summary;
	/** Whether the journal is adding entries from the entryWaiters queue */
	bool adding_entries;
	/** Whether a partial write is in progress */
	bool partial_write_in_progress;

	/** The oldest block in the journal on disk */
	SequenceNumber head;
	/** The oldest block in the journal which may not be reaped */
	SequenceNumber unreapable;
	/** The end of the half-open interval of the active journal */
	SequenceNumber tail;
	/** The next journal block to be committed */
	SequenceNumber next_commit;
	/** The tail sequence number that is written in the slab summary */
	SequenceNumber summarized;
	/** The tail sequence number that was last summarized in slab summary */
	SequenceNumber last_summarized;

	/** The sequence number of the recovery journal lock */
	SequenceNumber recovery_lock;

	/**
	 * The number of entries which fit in a single block. Can't use the
	 * constant because unit tests change this number.
	 **/
	JournalEntryCount entries_per_block;
	/**
	 * The number of full entries which fit in a single block. Can't use the
	 * constant because unit tests change this number.
	 **/
	JournalEntryCount full_entries_per_block;

	/** The recovery journal of the VDO (slab journal holds locks on it) */
	struct recovery_journal *recoveryJournal;

	/** The slab summary to update tail block location */
	struct slab_summary_zone *summary;
	/** The statistics shared by all slab journals in our physical zone */
	struct atomic_slab_journal_statistics *events;
	/** A ring of the VIO pool entries for outstanding journal block writes
	 */
	RingNode uncommitted_blocks;

	/**
	 * The current tail block header state. This will be packed into
	 * the block just before it is written.
	 **/
	struct slab_journal_block_header tail_header;
	/** A pointer to a block-sized buffer holding the packed block data */
	struct packed_slab_journal_block *block;

	/** The number of blocks in the on-disk journal */
	block_count_t size;
	/** The number of blocks at which to start pushing reference blocks */
	block_count_t flushing_threshold;
	/** The number of blocks at which all reference blocks should be writing
	 */
	block_count_t flushing_deadline;
	/** The number of blocks at which to wait for reference blocks to write
	 */
	block_count_t blockingThreshold;
	/**
	 * The number of blocks at which to scrub the slab before coming online
	 */
	block_count_t scrubbing_threshold;

	/** This node is for BlockAllocator to keep a queue of dirty journals */
	RingNode dirty_node;

	/** The lock for the oldest unreaped block of the journal */
	struct journal_lock *reap_lock;
	/** The locks for each on disk block */
	struct journal_lock locks[];
};

/**
 * Get the slab journal block offset of the given sequence number.
 *
 * @param journal   The slab journal
 * @param sequence  The sequence number
 *
 * @return the offset corresponding to the sequence number
 **/
__attribute__((warn_unused_result)) static inline TailBlockOffset
get_slab_journal_block_offset(struct slab_journal *journal, SequenceNumber sequence)
{
	return (sequence % journal->size);
}

/**
 * Encode a slab journal entry (exposed for unit tests).
 *
 * @param tail_header  The unpacked header for the block
 * @param payload      The journal block payload to hold the entry
 * @param sbn          The slab block number of the entry to encode
 * @param operation    The type of the entry
 **/
void encode_slab_journal_entry(struct slab_journal_block_header *tail_header,
			       slab_journal_payload *payload,
			       slab_block_number sbn,
			       journal_operation operation);

/**
 * Decode a slab journal entry.
 *
 * @param block         The journal block holding the entry
 * @param entry_count   The number of the entry
 *
 * @return The decoded entry
 **/
struct slab_journal_entry
decode_slab_journal_entry(struct packed_slab_journal_block *block,
			  JournalEntryCount entry_count)
	__attribute__((warn_unused_result));

/**
 * Generate the packed encoding of a slab journal entry.
 *
 * @param packed        The entry into which to pack the values
 * @param sbn           The slab block number of the entry to encode
 * @param is_increment  The increment flag
 **/
static inline void pack_slab_journal_entry(packed_slab_journal_entry *packed,
					   slab_block_number sbn,
					   bool is_increment)
{
	packed->fields.offset_low8  = (sbn & 0x0000FF);
	packed->fields.offset_mid8  = (sbn & 0x00FF00) >> 8;
	packed->fields.offset_high7 = (sbn & 0x7F0000) >> 16;
	packed->fields.increment   = is_increment ? 1 : 0;
}

/**
 * Decode the packed representation of a slab journal entry.
 *
 * @param packed  The packed entry to decode
 *
 * @return The decoded slab journal entry
 **/
__attribute__((warn_unused_result)) static inline struct slab_journal_entry
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
 * Generate the packed representation of a slab block header.
 *
 * @param header  The header containing the values to encode
 * @param packed  The header into which to pack the values
 **/
static inline void
pack_slab_journal_block_header(const struct slab_journal_block_header *header,
			       packed_slab_journal_block_header *packed)
{
	storeUInt64LE(packed->fields.head, header->head);
	storeUInt64LE(packed->fields.sequence_number, header->sequence_number);
	storeUInt64LE(packed->fields.nonce, header->nonce);
	storeUInt16LE(packed->fields.entry_count, header->entry_count);

	packed->fields.metadata_type = header->metadata_type;
	packed->fields.has_block_map_increments = header->has_block_map_increments;

	pack_journal_point(&header->recovery_point,
			   &packed->fields.recovery_point);
}

/**
 * Decode the packed representation of a slab block header.
 *
 * @param packed  The packed header to decode
 * @param header  The header into which to unpack the values
 **/
static inline void
unpack_slab_journal_block_header(const packed_slab_journal_block_header *packed,
				 struct slab_journal_block_header *header)
{
	*header = (struct slab_journal_block_header) {
		.head = getUInt64LE(packed->fields.head),
		.sequence_number = getUInt64LE(packed->fields.sequence_number),
		.nonce = getUInt64LE(packed->fields.nonce),
		.entry_count = getUInt16LE(packed->fields.entry_count),
		.metadata_type = packed->fields.metadata_type,
		.has_block_map_increments =
			packed->fields.has_block_map_increments,
	};
	unpack_journal_point(&packed->fields.recovery_point,
			     &header->recovery_point);
}

#endif // SLAB_JOURNAL_INTERNALS_H
