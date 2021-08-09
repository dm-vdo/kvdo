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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournalFormat.c#5 $
 */

#include "slabJournalFormat.h"

#include "journalPoint.h"
#include "types.h"

/**********************************************************************/
struct slab_journal_entry
decode_vdo_slab_journal_entry(struct packed_slab_journal_block *block,
			      journal_entry_count_t entry_count)
{
	struct slab_journal_entry entry =
		unpack_vdo_slab_journal_entry(&block->payload.entries[entry_count]);
	if (block->header.has_block_map_increments &&
	    ((block->payload.full_entries.entry_types[entry_count / 8] &
	      ((byte)1 << (entry_count % 8))) != 0)) {
		entry.operation = VDO_JOURNAL_BLOCK_MAP_INCREMENT;
	}
	return entry;
}

