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
 */

#ifndef SLAB_SUMMARY_H
#define SLAB_SUMMARY_H

#include <linux/atomic.h>

#include "admin-state.h"
#include "completion.h"
#include "fixed-layout.h"
#include "slab.h"
#include "slab-summary-format.h"
#include "statistics.h"
#include "types.h"
#include "wait-queue.h"

/**
 * The slab_summary provides hints during load and recovery about the state
 * of the slabs in order to avoid the need to read the slab journals in their
 * entirety before a VDO can come online.
 *
 * The information in the summary for each slab includes the rough number of
 * free blocks (which is used to prioritize scrubbing), the cleanliness of a
 * slab (so that clean slabs containing free space will be used on restart),
 * and the location of the tail block of the slab's journal.
 *
 * The slab_summary has its own partition at the end of the volume which is
 * sized to allow for a complete copy of the summary for each of up to 16
 * physical zones.
 *
 * During resize, the slab_summary moves its backing partition and is saved
 * once moved; the slab_summary is not permitted to overwrite the previous
 * recovery journal space.
 *
 * The slab_summary does not have its own version information, but relies on
 * the VDO volume version number.
 **/

/**
 * A slab status is a very small structure for use in determining the ordering
 * of slabs in the scrubbing process.
 **/
struct slab_status {
	slab_count_t slab_number;
	bool is_clean;
	uint8_t emptiness;
};

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
	/** The number of blocks (calculated based on MAX_VDO_SLABS) */
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
 * Create a slab summary.
 *
 * @param [in]  vdo                           The vdo
 * @param [in]  partition                     The partition to hold the summary
 * @param [in]  thread_config                 The thread config of the VDO
 * @param [in]  slab_size_shift               The number of bits in the slab
 *                                            size
 * @param [in]  maximum_free_blocks_per_slab  The maximum number of free blocks
 *                                            a slab can have
 * @param [in]  read_only_notifier            The context for entering
 *                                            read-only mode
 * @param [out] slab_summary_ptr              A pointer to hold the summary
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_vdo_slab_summary(struct vdo *vdo,
		      struct partition *partition,
		      const struct thread_config *thread_config,
		      unsigned int slab_size_shift,
		      block_count_t maximum_free_blocks_per_slab,
		      struct read_only_notifier *read_only_notifier,
		      struct slab_summary **slab_summary_ptr);

/**
 * Destroy a slab summary.
 *
 * @param summary  The slab summary to free
 **/
void free_vdo_slab_summary(struct slab_summary *summary);

/**
 * Get the portion of the slab summary for a specified zone.
 *
 * @param summary  The slab summary
 * @param zone     The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
struct slab_summary_zone * __must_check
vdo_get_slab_summary_for_zone(struct slab_summary *summary, zone_count_t zone);

/**
 * Drain a zone of the slab summary.
 *
 * @param summary_zone  The zone to drain
 * @param operation     The type of drain to perform
 * @param parent        The object to notify when the suspend is complete
 **/
void drain_vdo_slab_summary_zone(struct slab_summary_zone *summary_zone,
				 const struct admin_state_code *operation,
				 struct vdo_completion *parent);

/**
 * Resume a zone of the slab summary.
 *
 * @param summary_zone  The zone to resume
 * @param parent        The object to notify when the zone is resumed
 **/
void resume_vdo_slab_summary_zone(struct slab_summary_zone *summary_zone,
				  struct vdo_completion *parent);

/**
 * Update the entry for a slab.
 *
 * @param summary_zone       The slab_summary_zone for the zone of the slab
 * @param waiter             The waiter that is updating the summary
 * @param slab_number        The slab number to update
 * @param tail_block_offset  The offset of slab journal's tail block
 * @param load_ref_counts    Whether the ref_counts must be loaded from the
 *                           layer on the next load
 * @param is_clean           Whether the slab is clean
 * @param free_blocks        The number of free blocks
 **/
void vdo_update_slab_summary_entry(struct slab_summary_zone *summary_zone,
				   struct waiter *waiter,
				   slab_count_t slab_number,
				   tail_block_offset_t tail_block_offset,
				   bool load_ref_counts,
				   bool is_clean,
				   block_count_t free_blocks);

/**
 * Get the stored tail block offset for a slab.
 *
 * @param summary_zone       The slab_summary_zone to use
 * @param slab_number        The slab number to get the offset for
 *
 * @return The tail block offset for the slab
 **/
tail_block_offset_t __must_check
vdo_get_summarized_tail_block_offset(struct slab_summary_zone *summary_zone,
				     slab_count_t slab_number);

/**
 * Whether ref_counts must be loaded from the layer.
 *
 * @param summary_zone   The slab_summary_zone to use
 * @param slab_number    The slab number to get information for
 *
 * @return Whether ref_counts must be loaded
 **/
bool __must_check vdo_must_load_ref_counts(struct slab_summary_zone *summary_zone,
					   slab_count_t slab_number);

/**
 * Get the stored cleanliness information for a single slab.
 *
 * @param summary_zone   The slab_summary_zone to use
 * @param slab_number    The slab number to get information for
 *
 * @return Whether the slab is clean
 **/
bool __must_check
vdo_get_summarized_cleanliness(struct slab_summary_zone *summary_zone,
			       slab_count_t slab_number);

/**
 * Get the stored emptiness information for a single slab.
 *
 * @param summary_zone    The slab_summary_zone to use
 * @param slab_number     The slab number to get information for
 *
 * @return An approximation to the free blocks in the slab
 **/
block_count_t __must_check
vdo_get_summarized_free_block_count(struct slab_summary_zone *summary_zone,
				    slab_count_t slab_number);

/**
 * Get the stored slab statuses for all slabs in a zone.
 *
 * @param [in]     summary_zone  The slab_summary_zone to use
 * @param [in]     slab_count    The number of slabs to fetch
 * @param [in,out] statuses      An array of slab_status structures to populate
 **/
void vdo_get_summarized_slab_statuses(struct slab_summary_zone *summary_zone,
				      slab_count_t slab_count,
				      struct slab_status *statuses);

/**
 * Set the origin of the slab summary relative to the physical layer.
 *
 * @param summary    The slab_summary to update
 * @param partition  The slab summary partition
 **/
void set_vdo_slab_summary_origin(struct slab_summary *summary,
				 struct partition *partition);

/**
 * Read in all the slab summary data from the slab summary partition,
 * combine all the previously used zones into a single zone, and then
 * write the combined summary back out to each possible zones' summary
 * region.
 *
 * @param summary           The summary to load
 * @param operation         The type of load to perform
 * @param zones_to_combine  The number of zones to be combined; if set to 0,
 *                          all of the summary will be initialized as new.
 * @param parent            The parent of this operation
 **/
void load_vdo_slab_summary(struct slab_summary *summary,
			   const struct admin_state_code *operation,
			   zone_count_t zones_to_combine,
			   struct vdo_completion *parent);

/**
 * Fetch the cumulative statistics for all slab summary zones in a summary.
 *
 * @param summary       The summary in question
 *
 * @return the cumulative slab summary statistics for the summary
 **/
struct slab_summary_statistics __must_check
get_vdo_slab_summary_statistics(const struct slab_summary *summary);

#endif /* SLAB_SUMMARY_H */
