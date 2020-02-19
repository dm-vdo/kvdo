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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalEntry.h#5 $
 */

#ifndef RECOVERY_JOURNAL_ENTRY_H
#define RECOVERY_JOURNAL_ENTRY_H

#include "numeric.h"

#include "blockMapEntry.h"
#include "journalPoint.h"
#include "types.h"

/**
 * A recovery journal entry stores two physical locations: a data location
 * that is the value of a single mapping in the block map tree, and the
 * location of the block map page and and slot that is either acquiring or
 * releasing a reference to the data location. The journal entry also stores
 * an operation code that says whether the reference is being acquired (an
 * increment) or released (a decrement), and whether the mapping is for a
 * logical block or for the block map tree itself.
 **/
struct recovery_journal_entry {
	struct block_map_slot slot;
	struct data_location mapping;
	JournalOperation operation;
};

/** The packed, on-disk representation of a recovery journal entry. */
typedef union __attribute__((packed)) {
	struct __attribute__((packed)) {
		/**
		 * In little-endian bit order:
		 * Bits 15..12:  The four highest bits of the 36-bit physical
		 * block number of the block map tree page Bits 11..2:   The
		 * 10-bit block map page slot number Bits 1..0:    The 2-bit
		 * JournalOperation of the entry
		 **/
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		unsigned operation : 2;
		unsigned slot_low : 6;
		unsigned slot_high : 4;
		unsigned pbn_high_nibble : 4;
#else
		unsigned slot_low : 6;
		unsigned operation : 2;
		unsigned pbn_high_nibble : 4;
		unsigned slot_high : 4;
#endif

		/**
		 * Bits 47..16:  The 32 low-order bits of the block map page
		 * PBN, in little-endian byte order
		 **/
		byte pbn_low_word[4];

		/**
		 * Bits 87..48:  The five-byte block map entry encoding the
		 * location that was or will be stored in the block map page
		 * slot
		 **/
		BlockMapEntry block_map_entry;
	} fields;

	// A raw view of the packed encoding.
	uint8_t raw[11];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// This view is only valid on little-endian machines and is only present
	// for ease of directly examining packed entries in GDB.
	struct __attribute__((packed)) {
		unsigned operation : 2;
		unsigned slot : 10;
		unsigned pbn_high_nibble : 4;
		uint32_t pbn_low_word;
		BlockMapEntry block_map_entry;
	} little_endian;
#endif
} packed_recovery_journal_entry;

/**
 * Return the packed, on-disk representation of a recovery journal entry.
 *
 * @param entry   The journal entry to pack
 *
 * @return  The packed representation of the journal entry
 **/
static inline packed_recovery_journal_entry
pack_recovery_journal_entry(const struct recovery_journal_entry *entry)
{
	packed_recovery_journal_entry packed = {
		.fields = {
			.operation = entry->operation,
			.slot_low = entry->slot.slot & 0x3F,
			.slot_high = (entry->slot.slot >> 6) & 0x0F,
			.pbn_high_nibble = (entry->slot.pbn >> 32) & 0x0F,
			.block_map_entry = packPBN(entry->mapping.pbn,
						   entry->mapping.state),
		}};
	storeUInt32LE(packed.fields.pbn_low_word, entry->slot.pbn & UINT_MAX);
	return packed;
}

/**
 * Unpack the on-disk representation of a recovery journal entry.
 *
 * @param entry  The recovery journal entry to unpack
 *
 * @return  The unpacked entry
 **/
static inline struct recovery_journal_entry
unpack_recovery_journal_entry(const packed_recovery_journal_entry *entry)
{
	PhysicalBlockNumber low32 = getUInt32LE(entry->fields.pbn_low_word);
	PhysicalBlockNumber high4 = entry->fields.pbn_high_nibble;
	return (struct recovery_journal_entry) {
		.operation = entry->fields.operation,
		.slot =
			{
				.pbn = ((high4 << 32) | low32),
				.slot = (entry->fields.slot_low
					 | (entry->fields.slot_high << 6)),
			},
		.mapping = unpackBlockMapEntry(&entry->fields.block_map_entry),
	};
}

#endif // RECOVERY_JOURNAL_ENTRY_H
