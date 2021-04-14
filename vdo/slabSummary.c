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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabSummary.c#58 $
 */

#include "slabSummary.h"

#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "constants.h"
#include "extent.h"
#include "readOnlyNotifier.h"
#include "slabSummaryFormat.h"
#include "slabSummaryInternals.h"
#include "threadConfig.h"
#include "types.h"

// FULLNESS HINT COMPUTATION

/**
 * Translate a slab's free block count into a 'fullness hint' that can be
 * stored in a slab_summary_entry's 7 bits that are dedicated to its free
 * count.
 *
 * Note: the number of free blocks must be strictly less than 2^23 blocks,
 * even though theoretically slabs could contain precisely 2^23 blocks; there
 * is an assumption that at least one block is used by metadata. This
 * assumption is necessary; otherwise, the fullness hint might overflow.
 * The fullness hint formula is roughly (fullness >> 16) & 0x7f, but
 * ((1 << 23) >> 16) & 0x7f is the same as (0 >> 16) & 0x7f, namely 0, which
 * is clearly a bad hint if it could indicate both 2^23 free blocks or 0 free
 * blocks.
 *
 * @param summary      The summary which is being updated
 * @param free_blocks  The number of free blocks
 *
 * @return A fullness hint, which can be stored in 7 bits.
 **/
static uint8_t __must_check
compute_fullness_hint(struct slab_summary *summary, block_count_t free_blocks)
{
	block_count_t hint;
	ASSERT_LOG_ONLY((free_blocks < (1 << 23)),
			"free blocks must be less than 2^23");

	if (free_blocks == 0) {
		return 0;
	}

	hint = free_blocks >> summary->hint_shift;
	return ((hint == 0) ? 1 : hint);
}

/**
 * Translate a slab's free block hint into an approximate count, such that
 * compute_fullness_hint() is the inverse function of
 * get_approximate_free_blocks()
 * (i.e. compute_fullness_hint(get_approximate_free_blocks(x)) == x).
 *
 * @param  summary          The summary from which the hint was obtained
 * @param  free_block_hint  The hint read from the summary
 *
 * @return An approximation to the free block count
 **/
static block_count_t __must_check
get_approximate_free_blocks(struct slab_summary *summary,
			    uint8_t free_block_hint)
{
	return ((block_count_t) free_block_hint) << summary->hint_shift;
}

// MAKE/FREE FUNCTIONS

/**********************************************************************/
static void launch_write(struct slab_summary_block *summary_block);

/**
 * Initialize a slab_summary_block.
 *
 * @param vdo                 The vdo
 * @param summary_zone        The parent slab_summary_zone
 * @param thread_id           The ID of the thread of physical zone of this
 *                            block
 * @param entries             The entries this block manages
 * @param index               The index of this block in its zone's summary
 * @param slab_summary_block  The block to intialize
 *
 * @return VDO_SUCCESS or an error
 **/
static int
initialize_slab_summary_block(struct vdo *vdo,
			      struct slab_summary_zone *summary_zone,
			      thread_id_t thread_id,
			      struct slab_summary_entry *entries,
			      block_count_t index,
			      struct slab_summary_block *slab_summary_block)
{
	int result = ALLOCATE(VDO_BLOCK_SIZE, char, __func__,
			      &slab_summary_block->outgoing_entries);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = create_metadata_vio(vdo,
				     VIO_TYPE_SLAB_SUMMARY,
				     VIO_PRIORITY_METADATA,
				     slab_summary_block,
				     slab_summary_block->outgoing_entries,
				     &slab_summary_block->vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	slab_summary_block->vio->completion.callback_thread_id = thread_id;
	slab_summary_block->zone = summary_zone;
	slab_summary_block->entries = entries;
	slab_summary_block->index = index;
	return VDO_SUCCESS;
}

/**
 * Create a new, empty slab_summary_zone object.
 *
 * @param summary      The summary to which the new zone will belong
 * @param vdo          The vdo
 * @param zone_number  The zone this is
 * @param thread_id    The ID of the thread for this zone
 * @param entries      The buffer to hold the entries in this zone
 *
 * @return VDO_SUCCESS or an error
 **/
static int make_slab_summary_zone(struct slab_summary *summary,
				  struct vdo *vdo,
				  zone_count_t zone_number,
				  thread_id_t thread_id,
				  struct slab_summary_entry *entries)
{
	struct slab_summary_zone *summary_zone;
	block_count_t i;
	int result = ALLOCATE_EXTENDED(struct slab_summary_zone,
				       summary->blocks_per_zone,
				       struct slab_summary_block, __func__,
				       &summary->zones[zone_number]);
	if (result != VDO_SUCCESS) {
		return result;
	}

	summary_zone = summary->zones[zone_number];
	summary_zone->summary = summary;
	summary_zone->zone_number = zone_number;
	summary_zone->entries = entries;

	// Initialize each block.
	for (i = 0; i < summary->blocks_per_zone; i++) {
		result = initialize_slab_summary_block(vdo, summary_zone,
						       thread_id, entries, i,
						       &summary_zone->summary_blocks[i]);
		if (result != VDO_SUCCESS) {
			return result;
		}
		entries += summary->entries_per_block;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int make_slab_summary(struct vdo *vdo,
		      struct partition *partition,
		      const struct thread_config *thread_config,
		      unsigned int slab_size_shift,
		      block_count_t maximum_free_blocks_per_slab,
		      struct read_only_notifier *read_only_notifier,
		      struct slab_summary **slab_summary_ptr)
{
	struct slab_summary *summary;
	size_t total_entries, i;
	uint8_t hint;
	zone_count_t zone;
	block_count_t blocks_per_zone =
		get_slab_summary_zone_size(VDO_BLOCK_SIZE);
	slab_count_t entries_per_block = MAX_SLABS / blocks_per_zone;
	int result = ASSERT((entries_per_block * blocks_per_zone) == MAX_SLABS,
			    "block size must be a multiple of entry size");
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (partition == NULL) {
		// Don't make a slab summary for the formatter since it doesn't
		// need it.
		return VDO_SUCCESS;
	}

	result = ALLOCATE_EXTENDED(struct slab_summary,
				   thread_config->physical_zone_count,
				   struct slab_summary_zone *,
				   __func__,
				   &summary);
	if (result != VDO_SUCCESS) {
		return result;
	}

	summary->zone_count = thread_config->physical_zone_count;
	summary->read_only_notifier = read_only_notifier;
	summary->hint_shift = get_slab_summary_hint_shift(slab_size_shift);
	summary->blocks_per_zone = blocks_per_zone;
	summary->entries_per_block = entries_per_block;

	total_entries = MAX_SLABS * MAX_PHYSICAL_ZONES;
	result = ALLOCATE(total_entries, struct slab_summary_entry,
			  "summary entries", &summary->entries);
	if (result != VDO_SUCCESS) {
		free_slab_summary(&summary);
		return result;
	}

	// Initialize all the entries.
	hint = compute_fullness_hint(summary, maximum_free_blocks_per_slab);
	for (i = 0; i < total_entries; i++) {
		// This default tail block offset must be reflected in
		// slabJournal.c::read_slab_journal_tail().
		summary->entries[i] = (struct slab_summary_entry) {
			.tail_block_offset = 0,
			.fullness_hint = hint,
			.load_ref_counts = false,
			.is_dirty = false,
		};
	}

	set_slab_summary_origin(summary, partition);
	for (zone = 0; zone < summary->zone_count; zone++) {
		result =
			make_slab_summary_zone(summary, vdo, zone,
					       get_physical_zone_thread(thread_config,
								        zone),
					       summary->entries +
					       (MAX_SLABS * zone));
		if (result != VDO_SUCCESS) {
			free_slab_summary(&summary);
			return result;
		}
	}

	*slab_summary_ptr = summary;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_slab_summary(struct slab_summary **slab_summary_ptr)
{
	struct slab_summary *summary;
	zone_count_t zone;

	if (*slab_summary_ptr == NULL) {
		return;
	}

	summary = *slab_summary_ptr;
	for (zone = 0; zone < summary->zone_count; zone++) {
		struct slab_summary_zone *summary_zone = summary->zones[zone];
		if (summary_zone != NULL) {
			block_count_t i;
			for (i = 0; i < summary->blocks_per_zone; i++) {
				free_vio(&summary_zone->summary_blocks[i].vio);
				FREE(summary_zone->summary_blocks[i]
					     .outgoing_entries);
			}
			FREE(summary_zone);
		}
	}
	FREE(summary->entries);
	FREE(summary);
	*slab_summary_ptr = NULL;
}

/**********************************************************************/
struct slab_summary_zone *get_summary_for_zone(struct slab_summary *summary,
					       zone_count_t zone)
{
	return summary->zones[zone];
}

// WRITING FUNCTIONALITY

/**
 * Check whether a summary zone has finished draining.
 *
 * @param summary_zone  The zone to check
 **/
static void check_for_drain_complete(struct slab_summary_zone *summary_zone)
{
	if (!is_vdo_state_draining(&summary_zone->state)
	    || (summary_zone->write_count > 0)) {
		return;
	}

	finish_vdo_operation_with_result(&summary_zone->state,
					 (is_read_only(summary_zone->summary->read_only_notifier)
					  ? VDO_READ_ONLY : VDO_SUCCESS));
}

/**
 * Wake all the waiters in a given queue. If the VDO is in read-only mode they
 * will be given a VDO_READ_ONLY error code as their context, otherwise they
 * will be given VDO_SUCCESS.
 *
 * @param summary_zone  The slab summary which owns the queue
 * @param queue         The queue to notify
 **/
static void notify_waiters(struct slab_summary_zone *summary_zone,
			  struct wait_queue *queue)
{
	int result = (is_read_only(summary_zone->summary->read_only_notifier)
		      ? VDO_READ_ONLY
		      : VDO_SUCCESS);
	notify_all_waiters(queue, NULL, &result);
}

/**
 * Finish processing a block which attempted to write, whether or not the
 * attempt succeeded.
 *
 * @param block  The block
 **/
static void finish_updating_slab_summary_block(struct slab_summary_block *block)
{
	notify_waiters(block->zone, &block->current_update_waiters);
	block->writing = false;
	block->zone->write_count--;
	if (has_waiters(&block->next_update_waiters)) {
		launch_write(block);
	} else {
		check_for_drain_complete(block->zone);
	}
}

/**
 * This is the callback for a successful block write.
 *
 * @param completion  The write VIO
 **/
static void finish_update(struct vdo_completion *completion)
{
	struct slab_summary_block *block = completion->parent;
	atomic64_inc(&block->zone->summary->statistics.blocks_written);
	finish_updating_slab_summary_block(block);
}

/**
 * Handle an error writing a slab summary block.
 *
 * @param completion  The write VIO
 **/
static void handle_write_error(struct vdo_completion *completion)
{
	struct slab_summary_block *block = completion->parent;
	enter_read_only_mode(block->zone->summary->read_only_notifier,
			     completion->result);
	finish_updating_slab_summary_block(block);
}

/**
 * Write a slab summary block unless it is currently out for writing.
 *
 * @param [in] block  The block that needs to be committed
 **/
static void launch_write(struct slab_summary_block *block)
{
	struct slab_summary_zone *zone = block->zone;
	struct slab_summary *summary = zone->summary;
	physical_block_number_t pbn;

	if (block->writing) {
		return;
	}

	zone->write_count++;
	transfer_all_waiters(&block->next_update_waiters,
			     &block->current_update_waiters);
	block->writing = true;

	if (is_read_only(summary->read_only_notifier)) {
		finish_updating_slab_summary_block(block);
		return;
	}

	memcpy(block->outgoing_entries, block->entries,
	       sizeof(struct slab_summary_entry) * summary->entries_per_block);

	// Flush before writing to ensure that the slab journal tail blocks and
	// reference updates covered by this summary update are stable
	// (VDO-2332).
	pbn = summary->origin +
	      (summary->blocks_per_zone * zone->zone_number) + block->index;
	launch_write_metadata_vio_with_flush(block->vio, pbn, finish_update,
					     handle_write_error, true, false);
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct slab_summary_zone,
					      state));
}

/**********************************************************************/
void drain_slab_summary_zone(struct slab_summary_zone *summary_zone,
			     enum admin_state_code operation,
			     struct vdo_completion *parent)
{
	start_vdo_draining(&summary_zone->state, operation, parent,
			   initiate_drain);
}

/**********************************************************************/
void resume_slab_summary_zone(struct slab_summary_zone *summary_zone,
			      struct vdo_completion *parent)
{
	finish_completion(parent,
			  resume_vdo_if_quiescent(&summary_zone->state));
}

// READ/UPDATE FUNCTIONS

/**
 * Get the summary block, and offset into it, for storing the summary for a
 * slab.
 *
 * @param summary_zone    The slab_summary_zone being queried
 * @param slab_number     The slab whose summary location is sought
 *
 * @return A pointer to the slab_summary_block containing this
 *         slab_summary_entry
 **/
static struct slab_summary_block *
get_summary_block_for_slab(struct slab_summary_zone *summary_zone,
			   slab_count_t slab_number)
{
	slab_count_t entries_per_block =
		summary_zone->summary->entries_per_block;
	return &summary_zone->summary_blocks[slab_number / entries_per_block];
}

/**********************************************************************/
void update_slab_summary_entry(struct slab_summary_zone *summary_zone,
			       struct waiter *waiter, slab_count_t slab_number,
			       tail_block_offset_t tail_block_offset,
			       bool load_ref_counts, bool is_clean,
			       block_count_t free_blocks)
{
	struct slab_summary_block *block =
		get_summary_block_for_slab(summary_zone, slab_number);
	int result;
	if (is_read_only(summary_zone->summary->read_only_notifier)) {
		result = VDO_READ_ONLY;
	} else if (is_vdo_state_draining(&summary_zone->state)
		   || is_vdo_state_quiescent(&summary_zone->state)) {
		result = VDO_INVALID_ADMIN_STATE;
	} else {
		uint8_t hint = compute_fullness_hint(summary_zone->summary,
						     free_blocks);
		struct slab_summary_entry *entry =
			&summary_zone->entries[slab_number];
		*entry = (struct slab_summary_entry) {
			.tail_block_offset = tail_block_offset,
			.load_ref_counts =
				(entry->load_ref_counts || load_ref_counts),
			.is_dirty = !is_clean,
			.fullness_hint = hint,
		};
		result = enqueue_waiter(&block->next_update_waiters, waiter);
	}

	if (result != VDO_SUCCESS) {
		waiter->callback(waiter, &result);
		return;
	}

	launch_write(block);
}

/**********************************************************************/
tail_block_offset_t
get_summarized_tail_block_offset(struct slab_summary_zone *summary_zone,
				 slab_count_t slab_number)
{
	return summary_zone->entries[slab_number].tail_block_offset;
}

/**********************************************************************/
bool must_load_ref_counts(struct slab_summary_zone *summary_zone,
			  slab_count_t slab_number)
{
	return summary_zone->entries[slab_number].load_ref_counts;
}

/**********************************************************************/
bool get_summarized_cleanliness(struct slab_summary_zone *summary_zone,
				slab_count_t slab_number)
{
	return !summary_zone->entries[slab_number].is_dirty;
}

/**********************************************************************/
block_count_t
get_summarized_free_block_count(struct slab_summary_zone *summary_zone,
				slab_count_t slab_number)
{
	struct slab_summary_entry *entry = &summary_zone->entries[slab_number];
	return get_approximate_free_blocks(summary_zone->summary,
					   entry->fullness_hint);
}

/**********************************************************************/
void get_summarized_ref_counts_state(struct slab_summary_zone *summary_zone,
				     slab_count_t slab_number,
				     size_t *free_block_hint, bool *is_clean)
{
	struct slab_summary_entry *entry = &summary_zone->entries[slab_number];
	*free_block_hint = entry->fullness_hint;
	*is_clean = !entry->is_dirty;
}

/**********************************************************************/
void get_summarized_slab_statuses(struct slab_summary_zone *summary_zone,
				  slab_count_t slab_count,
				  struct slab_status *statuses)
{
	slab_count_t i;
	for (i = 0; i < slab_count; i++) {
		statuses[i] = (struct slab_status){
			.slab_number = i,
			.is_clean = !summary_zone->entries[i].is_dirty,
			.emptiness = summary_zone->entries[i].fullness_hint};
	}
}

// RESIZE FUNCTIONS

/**********************************************************************/
void set_slab_summary_origin(struct slab_summary *summary,
			     struct partition *partition)
{
	summary->origin = get_fixed_layout_partition_offset(partition);
}

// COMBINING FUNCTIONS (LOAD)

/**
 * Clean up after saving out the combined slab summary. This callback is
 * registered in finish_loading_summary() and load_slab_summary().
 *
 * @param completion  The extent which was used to write the summary data
 **/
static void finish_combining_zones(struct vdo_completion *completion)
{
	struct slab_summary *summary = completion->parent;
	int result = completion->result;
	struct vdo_extent *extent = as_vdo_extent(completion);
	free_extent(&extent);
	finish_vdo_loading_with_result(&summary->zones[0]->state, result);
}

/**********************************************************************/
void combine_zones(struct slab_summary *summary)
{
	// Combine all the old summary data into the portion of the buffer
	// corresponding to the first zone.
	zone_count_t zone = 0;
	if (summary->zones_to_combine > 1) {
		slab_count_t entry_number;
		for (entry_number = 0; entry_number < MAX_SLABS;
		     entry_number++) {
			if (zone != 0) {
				memcpy(summary->entries + entry_number,
				       summary->entries + (zone * MAX_SLABS)
					       + entry_number,
				       sizeof(struct slab_summary_entry));
			}
			zone++;
			if (zone == summary->zones_to_combine) {
				zone = 0;
			}
		}
	}

	// Copy the combined data to each zones's region of the buffer.
	for (zone = 1; zone < MAX_PHYSICAL_ZONES; zone++) {
		memcpy(summary->entries + (zone * MAX_SLABS), summary->entries,
		       MAX_SLABS * sizeof(struct slab_summary_entry));
	}
}

/**
 * Combine the slab summary data from all the previously written zones
 * and copy the combined summary to each partition's data region. Then write
 * the combined summary back out to disk. This callback is registered in
 * load_slab_summary().
 *
 * @param completion  The extent which was used to read the summary data
 **/
static void finish_loading_summary(struct vdo_completion *completion)
{
	struct slab_summary *summary = completion->parent;
	struct vdo_extent *extent = as_vdo_extent(completion);

	// Combine the zones so each zone is correct for all slabs.
	combine_zones(summary);

	// Write the combined summary back out.
	extent->completion.callback = finish_combining_zones;
	write_metadata_extent(extent, summary->origin);
}

/**********************************************************************/
void load_slab_summary(struct slab_summary *summary,
		       enum admin_state_code operation,
		       zone_count_t zones_to_combine,
		       struct vdo_completion *parent)
{
	struct vdo_extent *extent;
	block_count_t blocks;
	int result;

	struct slab_summary_zone *zone = summary->zones[0];
	if (!start_vdo_loading(&zone->state, operation, parent, NULL)) {
		return;
	}

	blocks = summary->blocks_per_zone * MAX_PHYSICAL_ZONES;
	result = create_extent(parent->vdo, VIO_TYPE_SLAB_SUMMARY,
			       VIO_PRIORITY_METADATA, blocks,
			       (char *)summary->entries, &extent);
	if (result != VDO_SUCCESS) {
		finish_vdo_loading_with_result(&zone->state, result);
		return;
	}

	if ((operation == ADMIN_STATE_FORMATTING)
	    || (operation == ADMIN_STATE_LOADING_FOR_REBUILD)) {
		prepare_completion(&extent->completion, finish_combining_zones,
				   finish_combining_zones, 0, summary);
		write_metadata_extent(extent, summary->origin);
		return;
	}

	summary->zones_to_combine = zones_to_combine;
	prepare_completion(&extent->completion, finish_loading_summary,
			   finish_combining_zones, 0, summary);
	read_metadata_extent(extent, summary->origin);
}

/**********************************************************************/
struct slab_summary_statistics
get_slab_summary_statistics(const struct slab_summary *summary)
{
	const struct atomic_slab_summary_statistics *atoms =
		&summary->statistics;
	return (struct slab_summary_statistics) {
		.blocks_written = atomic64_read(&atoms->blocks_written),
	};
}
