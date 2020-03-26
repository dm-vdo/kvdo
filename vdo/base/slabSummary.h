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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabSummary.h#14 $
 */

#ifndef SLAB_SUMMARY_H
#define SLAB_SUMMARY_H

#include "completion.h"
#include "fixedLayout.h"
#include "slab.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

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
 * the master version number.
 **/

/**
 * The offset of a slab journal tail block.
 **/
typedef uint8_t TailBlockOffset;

/**
 * A slab status is a very small structure for use in determining the ordering
 * of slabs in the scrubbing process.
 **/
struct slab_status {
	SlabCount slabNumber;
	bool isClean;
	uint8_t emptiness;
};

/**
 * Returns the size on disk of the slab_summary structure.
 *
 * @param block_size  The block size of the physical layer
 *
 * @return the blocks required to store the slab_summary on disk
 **/
BlockCount get_slab_summary_size(BlockSize block_size)
	__attribute__((warn_unused_result));

/**
 * Create a slab summary.
 *
 * @param [in]  layer                         The layer
 * @param [in]  partition                     The partition to hold the summary
 * @param [in]  thread_config                 The thread config of the VDO
 * @param [in]  slab_size_shift               The number of bits in the slab
 *                                            size
 * @param [in]  maximum_free_blocks_per_slab  The maximum number of free blocks
 *                                            a slab can have
 * @param [in]  read_only_notifier            The context for entering read-only
 *                                            mode
 * @param [out] slab_summary_ptr              A pointer to hold the summary
 *
 * @return VDO_SUCCESS or an error
 **/
int make_slab_summary(PhysicalLayer *layer,
		      struct partition *partition,
		      const struct thread_config *thread_config,
		      unsigned int slab_size_shift,
		      BlockCount maximum_free_blocks_per_slab,
		      struct read_only_notifier *read_only_notifier,
		      struct slab_summary **slab_summary_ptr)
	__attribute__((warn_unused_result));

/**
 * Destroy a slab_summary and NULL out the reference to it.
 *
 * @param [in,out] slab_summary_ptr A pointer to the slab_summary to free
 **/
void free_slab_summary(struct slab_summary **slab_summary_ptr);

/**
 * Get the portion of the slab summary for a specified zone.
 *
 * @param summary  The slab summary
 * @param zone     The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
struct slab_summary_zone *get_summary_for_zone(struct slab_summary *summary,
					       ZoneCount zone)
	__attribute__((warn_unused_result));

/**
 * Drain a zone of the slab summary.
 *
 * @param summary_zone  The zone to drain
 * @param operation     The type of drain to perform
 * @param parent        The object to notify when the suspend is complete
 **/
void drain_slab_summary_zone(struct slab_summary_zone *summary_zone,
			     AdminStateCode operation,
			     struct vdo_completion *parent);

/**
 * Resume a zone of the slab summary.
 *
 * @param summary_zone  The zone to resume
 * @param parent        The object to notify when the zone is resumed
 **/
void resume_slab_summary_zone(struct slab_summary_zone *summary_zone,
			      struct vdo_completion *parent);

/**
 * Update the entry for a slab.
 *
 * @param summary_zone      The slab_summary_zone for the zone of the slab
 * @param waiter            The waiter that is updating the summary
 * @param slab_number       The slab number to update
 * @param tail_block_offset The offset of slab journal's tail block
 * @param load_ref_counts   Whether the refCounts must be loaded from the layer
 *                          on the next load
 * @param is_clean          Whether the slab is clean
 * @param free_blocks       The number of free blocks
 **/
void update_slab_summary_entry(struct slab_summary_zone *summary_zone,
			       struct waiter *waiter, SlabCount slab_number,
			       TailBlockOffset tail_block_offset,
			       bool load_ref_counts, bool is_clean,
			       BlockCount free_blocks);

/**
 * Get the stored tail block offset for a slab.
 *
 * @param summary_zone       The slab_summary_zone to use
 * @param slab_number        The slab number to get the offset for
 *
 * @return The tail block offset for the slab
 **/
TailBlockOffset
get_summarized_tail_block_offset(struct slab_summary_zone *summary_zone,
				 SlabCount slab_number)
	__attribute__((warn_unused_result));

/**
 * Whether ref_counts must be loaded from the layer.
 *
 * @param summary_zone   The slab_summary_zone to use
 * @param slab_number    The slab number to get information for
 *
 * @return Whether ref_counts must be loaded
 **/
bool must_load_ref_counts(struct slab_summary_zone *summary_zone,
			  SlabCount slab_number)
	__attribute__((warn_unused_result));

/**
 * Get the stored cleanliness information for a single slab.
 *
 * @param summary_zone   The slab_summary_zone to use
 * @param slab_number    The slab number to get information for
 *
 * @return Whether the slab is clean
 **/
bool get_summarized_cleanliness(struct slab_summary_zone *summary_zone,
				SlabCount slab_number)
	__attribute__((warn_unused_result));

/**
 * Get the stored emptiness information for a single slab.
 *
 * @param summary_zone    The slab_summary_zone to use
 * @param slab_number     The slab number to get information for
 *
 * @return An approximation to the free blocks in the slab
 **/
BlockCount
get_summarized_free_block_count(struct slab_summary_zone *summary_zone,
				SlabCount slab_number)
	__attribute__((warn_unused_result));

/**
 * Get the stored RefCounts state information for a single slab. Used
 * in testing only.
 *
 * @param [in]  summary_zone      The slab_summary_zone to use
 * @param [in]  slab_number       The slab number to get information for
 * @param [out] free_block_hint   The approximate number of free blocks
 * @param [out] is_clean          Whether the slab is clean
 **/
void get_summarized_ref_counts_state(struct slab_summary_zone *summary_zone,
				     SlabCount slab_number,
				     size_t *free_block_hint, bool *is_clean);

/**
 * Get the stored slab statuses for all slabs in a zone.
 *
 * @param [in]     summary_zone   The slab_summary_zone to use
 * @param [in]     slab_count     The number of slabs to fetch
 * @param [in,out] statuses       An array of slab_status structures to populate
 **/
void get_summarized_slab_statuses(struct slab_summary_zone *summary_zone,
				  SlabCount slab_count,
				  struct slab_status *statuses);

/**
 * Set the origin of the slab summary relative to the physical layer.
 *
 * @param summary    The slab_summary to update
 * @param partition  The slab summary partition
 **/
void set_slab_summary_origin(struct slab_summary *summary,
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
void load_slab_summary(struct slab_summary *summary,
		       AdminStateCode operation,
		       ZoneCount zones_to_combine,
		       struct vdo_completion *parent);

/**
 * Fetch the cumulative statistics for all slab summary zones in a summary.
 *
 * @param summary       The summary in question
 *
 * @return the cumulative slab summary statistics for the summary
 **/
struct slab_summary_statistics
get_slab_summary_statistics(const struct slab_summary *summary)
	__attribute__((warn_unused_result));

#endif // SLAB_SUMMARY_H
