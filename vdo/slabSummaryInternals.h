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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabSummaryInternals.h#20 $
 */

#ifndef SLAB_SUMMARY_INTERNALS_H
#define SLAB_SUMMARY_INTERNALS_H

#include "slabSummary.h"

#include "atomicDefs.h"

#include "adminState.h"
#include "slabSummaryFormat.h"

struct slab_summary_block {
	/** The zone to which this block belongs */
	struct slab_summary_zone *zone;
	/** The index of this block in its zone's summary */
	block_count_t index;
	/** Whether this block has a write outstanding */
	bool writing;
	/** Ring of updates waiting on the outstanding write */
	struct wait_queue current_update_waiters;
	/** Ring of updates waiting on the next write */
	struct wait_queue next_update_waiters;
	/** The active slab_summary_entry array for this block */
	struct slab_summary_entry *entries;
	/** The vio used to write this block */
	struct vio *vio;
	/** The packed entries, one block long, backing the vio */
	char *outgoing_entries;
};

/**
 * The statistics for all the slab summary zones owned by this slab summary.
 * These fields are all mutated only by their physical zone threads, but are
 * read by other threads when gathering statistics for the entire depot.
 **/
struct atomic_slab_summary_statistics {
	/** Number of blocks written */
	atomic64_t blocks_written;
};

struct slab_summary_zone {
	/** The summary of which this is a zone */
	struct slab_summary *summary;
	/** The number of this zone */
	zone_count_t zone_number;
	/** Count of the number of blocks currently out for writing */
	block_count_t write_count;
	/** The state of this zone */
	struct admin_state state;
	/** The array (owned by the blocks) of all entries */
	struct slab_summary_entry *entries;
	/** The array of slab_summary_blocks */
	struct slab_summary_block summary_blocks[];
};

struct slab_summary {
	/** The context for entering read-only mode */
	struct read_only_notifier *read_only_notifier;
	/** The statistics for this slab summary */
	struct atomic_slab_summary_statistics statistics;
	/** The start of the slab summary partition relative to the layer */
	physical_block_number_t origin;
	/** The number of bits to shift to get a 7-bit fullness hint */
	unsigned int hint_shift;
	/** The number of blocks (calculated based on MAX_SLABS) */
	block_count_t blocks_per_zone;
	/** The number of slabs per block (calculated from block size) */
	slab_count_t entries_per_block;
	/** The entries for all of the zones the partition can hold */
	struct slab_summary_entry *entries;
	/**
	 * The number of zones which were active at the time of the last update
	 */
	zone_count_t zones_to_combine;
	/** The current number of active zones */
	zone_count_t zone_count;
	/** The currently active zones */
	struct slab_summary_zone *zones[];
};

/**
 * Treating the current entries buffer as the on-disk value of all zones,
 * update every zone to the correct values for every slab.
 *
 * @param summary       The summary whose entries should be combined
 **/
void combine_zones(struct slab_summary *summary);

#endif // SLAB_SUMMARY_INTERNALS_H
