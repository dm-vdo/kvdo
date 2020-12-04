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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournalFormat.c#2 $
 */

#include "slabJournalFormat.h"

#include "journalPoint.h"
#include "types.h"

/**********************************************************************/
struct slab_journal_entry
decode_slab_journal_entry(struct packed_slab_journal_block *block,
			  JournalEntryCount entry_count)
{
	struct slab_journal_entry entry =
		unpack_slab_journal_entry(&block->payload.entries[entry_count]);
	if (block->header.has_block_map_increments &&
	    ((block->payload.full_entries.entry_types[entry_count / 8] &
	      ((byte)1 << (entry_count % 8))) != 0)) {
		entry.operation = BLOCK_MAP_INCREMENT;
	}
	return entry;
}

