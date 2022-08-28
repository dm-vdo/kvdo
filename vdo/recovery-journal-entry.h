/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RECOVERY_JOURNAL_ENTRY_H
#define RECOVERY_JOURNAL_ENTRY_H

#include "numeric.h"

#include "block-map-entry.h"
#include "journal-point.h"
#include "types.h"

/*
 * A recovery journal entry stores two physical locations: a data location
 * that is the value of a single mapping in the block map tree, and the
 * location of the block map page and slot that is either acquiring or
 * releasing a reference to the data location. The journal entry also stores
 * an operation code that says whether the reference is being acquired (an
 * increment) or released (a decrement), and whether the mapping is for a
 * logical block or for the block map tree itself.
 */
struct recovery_journal_entry {
	struct block_map_slot slot;
	struct data_location mapping;
	enum journal_operation operation;
};

/* The packed, on-disk representation of a recovery journal entry. */
struct packed_recovery_journal_entry {
	/*
	 * In little-endian bit order:
	 * Bits 15..12:  The four highest bits of the 36-bit physical
	 * block number of the block map tree page Bits 11..2:   The
	 * 10-bit block map page slot number Bits 1..0:    The 2-bit
	 * journal_operation of the entry
	 */
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

	/*
	 * Bits 47..16:  The 32 low-order bits of the block map page
	 * PBN, in little-endian byte order
	 */
	__le32 pbn_low_word;

	/*
	 * Bits 87..48:  The five-byte block map entry encoding the
	 * location that was or will be stored in the block map page slot
	 */
	struct block_map_entry block_map_entry;
} __packed;

/**
 * vdo_pack_recovery_journal_entry() - Return the packed, on-disk
 *                                     representation of a recovery journal
 *                                     entry.
 * @entry: The journal entry to pack.
 *
 * Return: The packed representation of the journal entry.
 */
static inline struct packed_recovery_journal_entry
vdo_pack_recovery_journal_entry(const struct recovery_journal_entry *entry)
{
	return (struct packed_recovery_journal_entry) {
		.operation = entry->operation,
		.slot_low = entry->slot.slot & 0x3F,
		.slot_high = (entry->slot.slot >> 6) & 0x0F,
		.pbn_high_nibble = (entry->slot.pbn >> 32) & 0x0F,
		.pbn_low_word = __cpu_to_le32(entry->slot.pbn & UINT_MAX),
		.block_map_entry = vdo_pack_pbn(entry->mapping.pbn,
						entry->mapping.state),
	};
}

/**
 * vdo_unpack_recovery_journal_entry() - Unpack the on-disk representation of
 *                                       a recovery journal entry.
 * @entry: The recovery journal entry to unpack.
 *
 * Return: The unpacked entry.
 */
static inline struct recovery_journal_entry
vdo_unpack_recovery_journal_entry(const struct packed_recovery_journal_entry *entry)
{
	physical_block_number_t low32 = __le32_to_cpu(entry->pbn_low_word);
	physical_block_number_t high4 = entry->pbn_high_nibble;

	return (struct recovery_journal_entry) {
		.operation = entry->operation,
		.slot = {
				.pbn = ((high4 << 32) | low32),
				.slot = (entry->slot_low
					 | (entry->slot_high << 6)),
			},
		.mapping = vdo_unpack_block_map_entry(&entry->block_map_entry),
	};
}

#endif /* RECOVERY_JOURNAL_ENTRY_H */
