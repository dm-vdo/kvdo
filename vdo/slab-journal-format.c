// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab-journal-format.h"

#include "journal-point.h"
#include "types.h"

/**
 * vdo_decode_slab_journal_entry() - Decode a slab journal entry.
 * @block: The journal block holding the entry.
 * @entry_count: The number of the entry.
 *
 * Return: The decoded entry.
 */
struct slab_journal_entry
vdo_decode_slab_journal_entry(struct packed_slab_journal_block *block,
			      journal_entry_count_t entry_count)
{
	struct slab_journal_entry entry =
		vdo_unpack_slab_journal_entry(&block->payload.entries[entry_count]);
	if (block->header.has_block_map_increments &&
	    ((block->payload.full_entries.entry_types[entry_count / 8] &
	      ((byte)1 << (entry_count % 8))) != 0)) {
		entry.operation = VDO_JOURNAL_BLOCK_MAP_INCREMENT;
	}
	return entry;
}

