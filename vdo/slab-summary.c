// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab-summary.h"

#include <linux/bio.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "completion.h"
#include "constants.h"
#include "io-submitter.h"
#include "read-only-notifier.h"
#include "slab-summary-format.h"
#include "thread-config.h"
#include "types.h"
#include "vio.h"

/* FULLNESS HINT COMPUTATION */

/**
 * compute_fullness_hint() - Translate a slab's free block count into a
 *                           'fullness hint' that can be stored in a
 *                           slab_summary_entry's 7 bits that are dedicated to
 *                           its free count.
 * @summary: The summary which is being updated.
 * @free_blocks: The number of free blocks.
 *
 * Note: the number of free blocks must be strictly less than 2^23 blocks,
 * even though theoretically slabs could contain precisely 2^23 blocks; there
 * is an assumption that at least one block is used by metadata. This
 * assumption is necessary; otherwise, the fullness hint might overflow. The
 * fullness hint formula is roughly (fullness >> 16) & 0x7f, but ((1 > 16) &
 * 0x7f is the same as (0 >> 16) & 0x7f, namely 0, which is clearly a bad hint
 * if it could indicate both 2^23 free blocks or 0 free blocks.
 *
 * Return: A fullness hint, which can be stored in 7 bits.
 */
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
 * get_approximate_free_blocks() - Translate a slab's free block hint into an
 *                                 approximate count.
 * @summary: The summary from which the hint was obtained.
 * @free_block_hint: The hint read from the summary.
 *
 * compute_fullness_hint() is the inverse function of
 * get_approximate_free_blocks() (i.e.
 * compute_fullness_hint(get_approximate_free_blocks(x)) == x).
 *
 * Return: An approximation to the free block count.
 */
static block_count_t __must_check
get_approximate_free_blocks(struct slab_summary *summary,
			    uint8_t free_block_hint)
{
	return ((block_count_t) free_block_hint) << summary->hint_shift;
}

/* MAKE/FREE FUNCTIONS */

static void launch_write(struct slab_summary_block *summary_block);

/**
 * initialize_slab_summary_block() - Initialize a slab_summary_block.
 * @vdo: The vdo.
 * @summary_zone: The parent slab_summary_zone.
 * @entries: The entries this block manages.
 * @index: The index of this block in its zone's summary.
 * @slab_summary_block: The block to intialize.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int
initialize_slab_summary_block(struct vdo *vdo,
			      struct slab_summary_zone *summary_zone,
			      struct slab_summary_entry *entries,
			      block_count_t index,
			      struct slab_summary_block *slab_summary_block)
{
	int result = UDS_ALLOCATE(VDO_BLOCK_SIZE, char, __func__,
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

	slab_summary_block->zone = summary_zone;
	slab_summary_block->entries = entries;
	slab_summary_block->index = index;
	return VDO_SUCCESS;
}

/**
 * make_slab_summary_zone() - Create a new, empty slab_summary_zone object.
 * @summary: The summary to which the new zone will belong.
 * @vdo: The vdo.
 * @zone_number: The zone this is.
 * @thread_id: The ID of the thread for this zone.
 * @entries: The buffer to hold the entries in this zone.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int make_slab_summary_zone(struct slab_summary *summary,
				  struct vdo *vdo,
				  zone_count_t zone_number,
				  thread_id_t thread_id,
				  struct slab_summary_entry *entries)
{
	struct slab_summary_zone *summary_zone;
	block_count_t i;
	int result = UDS_ALLOCATE_EXTENDED(struct slab_summary_zone,
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
	summary_zone->thread_id = thread_id;
	vdo_set_admin_state_code(&summary_zone->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);

	/* Initialize each block. */
	for (i = 0; i < summary->blocks_per_zone; i++) {
		result = initialize_slab_summary_block(vdo,
						       summary_zone,
						       entries,
						       i,
						       &summary_zone->summary_blocks[i]);
		if (result != VDO_SUCCESS) {
			return result;
		}
		entries += summary->entries_per_block;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_make_slab_summary() - Create a slab summary.
 * @vdo: The vdo.
 * @partition: The partition to hold the summary.
 * @thread_config: The thread config of the VDO.
 * @slab_size_shift: The number of bits in the slab size.
 * @maximum_free_blocks_per_slab: The maximum number of free blocks a
 *                                slab can have.
 * @read_only_notifier: The context for entering read-only mode.
 * @slab_summary_ptr: A pointer to hold the summary.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_slab_summary(struct vdo *vdo,
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
		vdo_get_slab_summary_zone_size(VDO_BLOCK_SIZE);
	slab_count_t entries_per_block = MAX_VDO_SLABS / blocks_per_zone;
	int result = ASSERT((entries_per_block * blocks_per_zone) == MAX_VDO_SLABS,
			    "block size must be a multiple of entry size");
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (partition == NULL) {
		/*
		 * Don't make a slab summary for the formatter since it doesn't
		 * need it.
		 */
		return VDO_SUCCESS;
	}

	result = UDS_ALLOCATE_EXTENDED(struct slab_summary,
				       thread_config->physical_zone_count,
				       struct slab_summary_zone *,
				       __func__,
				       &summary);
	if (result != VDO_SUCCESS) {
		return result;
	}

	summary->zone_count = thread_config->physical_zone_count;
	summary->read_only_notifier = read_only_notifier;
	summary->hint_shift = vdo_get_slab_summary_hint_shift(slab_size_shift);
	summary->blocks_per_zone = blocks_per_zone;
	summary->entries_per_block = entries_per_block;

	total_entries = MAX_VDO_SLABS * MAX_VDO_PHYSICAL_ZONES;
	result = UDS_ALLOCATE(total_entries, struct slab_summary_entry,
			      "summary entries", &summary->entries);
	if (result != VDO_SUCCESS) {
		vdo_free_slab_summary(summary);
		return result;
	}

	/* Initialize all the entries. */
	hint = compute_fullness_hint(summary, maximum_free_blocks_per_slab);
	for (i = 0; i < total_entries; i++) {
		/*
		 * This default tail block offset must be reflected in
		 * slabJournal.c::read_slab_journal_tail().
		 */
		summary->entries[i] = (struct slab_summary_entry) {
			.tail_block_offset = 0,
			.fullness_hint = hint,
			.load_ref_counts = false,
			.is_dirty = false,
		};
	}

	vdo_set_slab_summary_origin(summary, partition);
	for (zone = 0; zone < summary->zone_count; zone++) {
		result =
			make_slab_summary_zone(summary, vdo, zone,
					       vdo_get_physical_zone_thread(thread_config,
									    zone),
					       summary->entries +
					       (MAX_VDO_SLABS * zone));
		if (result != VDO_SUCCESS) {
			vdo_free_slab_summary(summary);
			return result;
		}
	}

	*slab_summary_ptr = summary;
	return VDO_SUCCESS;
}

/**
 * free_summary_zone() - Free a slab summary zone.
 * @zone: The zone to free.
 */
static void free_summary_zone(struct slab_summary_zone *zone)
{
	block_count_t i;

	if (zone == NULL) {
		return;
	}

	for (i = 0; i < zone->summary->blocks_per_zone; i++) {
		free_vio(UDS_FORGET(zone->summary_blocks[i].vio));
		UDS_FREE(UDS_FORGET(zone->summary_blocks[i].outgoing_entries));
	}

	UDS_FREE(zone);
}

/**
 * vdo_free_slab_summary() - Destroy a slab summary.
 * @summary: The slab summary to free.
 */
void vdo_free_slab_summary(struct slab_summary *summary)
{
	zone_count_t zone;

	if (summary == NULL) {
		return;
	}

	for (zone = 0; zone < summary->zone_count; zone++) {
		free_summary_zone(UDS_FORGET(summary->zones[zone]));
	}

	UDS_FREE(UDS_FORGET(summary->entries));
	UDS_FREE(summary);
}

/**
 * vdo_get_slab_summary_for_zone() - Get the portion of the slab
 *                                   summary for a specified zone.
 * @summary: The slab summary.
 * @zone: The zone.
 *
 * Return: The portion of the slab summary for the specified zone.
 */
struct slab_summary_zone *
vdo_get_slab_summary_for_zone(struct slab_summary *summary, zone_count_t zone)
{
	return summary->zones[zone];
}

/* WRITING FUNCTIONALITY */

/**
 * check_for_drain_complete() - Check whether a summary zone has finished
 *                              draining.
 * @summary_zone: The zone to check.
 */
static void
check_for_drain_complete(struct slab_summary_zone *summary_zone)
{
	if (!vdo_is_state_draining(&summary_zone->state)
	    || (summary_zone->write_count > 0)) {
		return;
	}

	vdo_finish_operation(&summary_zone->state,
			     (vdo_is_read_only(summary_zone->summary->read_only_notifier)
			      ? VDO_READ_ONLY : VDO_SUCCESS));
}

/**
 * notify_waiters() - Wake all the waiters in a given queue.
 * @summary_zone: The slab summary which owns the queue.
 * @queue: The queue to notify.
 *
 * If the VDO is in read-only mode the waiters will be given a VDO_READ_ONLY
 * error code as their context, otherwise they will be given VDO_SUCCESS.
 */
static void notify_waiters(struct slab_summary_zone *summary_zone,
			   struct wait_queue *queue)
{
	int result = (vdo_is_read_only(summary_zone->summary->read_only_notifier)
		      ? VDO_READ_ONLY
		      : VDO_SUCCESS);
	notify_all_waiters(queue, NULL, &result);
}

/**
 * finish_updating_slab_summary_block() - Finish processing a block which
 *                                        attempted to write, whether or not
 *                                        the attempt succeeded.
 * @block: The block.
 */
static void
finish_updating_slab_summary_block(struct slab_summary_block *block)
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
 * finish_update() - This is the callback for a successful block write.
 * @completion: The write VIO.
 */
static void finish_update(struct vdo_completion *completion)
{
	struct slab_summary_block *block = completion->parent;

	atomic64_inc(&block->zone->summary->statistics.blocks_written);
	finish_updating_slab_summary_block(block);
}

/**
 * handle_write_error() - Handle an error writing a slab summary block.
 * @completion: The write VIO.
 */
static void handle_write_error(struct vdo_completion *completion)
{
	struct slab_summary_block *block = completion->parent;

	record_metadata_io_error(as_vio(completion));
	vdo_enter_read_only_mode(block->zone->summary->read_only_notifier,
				 completion->result);
	finish_updating_slab_summary_block(block);
}

static void write_slab_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct slab_summary_block *block = vio->completion.parent;

	continue_vio_after_io(vio, finish_update, block->zone->thread_id);
}

/**
 * launch_write() - Write a slab summary block unless it is currently out for
 *                  writing.
 * @block: The block that needs to be committed.
 */
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

	if (vdo_is_read_only(summary->read_only_notifier)) {
		finish_updating_slab_summary_block(block);
		return;
	}

	memcpy(block->outgoing_entries, block->entries,
	       sizeof(struct slab_summary_entry) * summary->entries_per_block);

	/*
	 * Flush before writing to ensure that the slab journal tail blocks and
	 * reference updates covered by this summary update are stable
	 * (VDO-2332).
	 */
	pbn = summary->origin +
	      (summary->blocks_per_zone * zone->zone_number) + block->index;
	submit_metadata_vio(block->vio,
			    pbn,
			    write_slab_summary_endio,
			    handle_write_error,
			    REQ_OP_WRITE | REQ_PREFLUSH);
}

/**
 * initiate_drain() - Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct slab_summary_zone,
					      state));
}

/**
 * vdo_drain_slab_summary_zone() - Drain a zone of the slab summary.
 * @summary_zone: The zone to drain.
 * @operation: The type of drain to perform.
 * @parent: The object to notify when the suspend is complete.
 */
void vdo_drain_slab_summary_zone(struct slab_summary_zone *summary_zone,
				 const struct admin_state_code *operation,
				 struct vdo_completion *parent)
{
	vdo_start_draining(&summary_zone->state, operation, parent,
			   initiate_drain);
}

/**
 * vdo_resume_slab_summary_zone() - Resume a zone of the slab summary.
 * @summary_zone: The zone to resume.
 * @parent: The object to notify when the zone is resumed.
 */
void vdo_resume_slab_summary_zone(struct slab_summary_zone *summary_zone,
				  struct vdo_completion *parent)
{
	vdo_finish_completion(parent,
			      vdo_resume_if_quiescent(&summary_zone->state));
}

/* READ/UPDATE FUNCTIONS */

/**
 * get_summary_block_for_slab() - Get the summary block, and offset into it,
 *                                for storing the summary for a slab.
 * @summary_zone: The slab_summary_zone being queried.
 * @slab_number: The slab whose summary location is sought.
 *
 * Return: A pointer to the slab_summary_block containing this
 *         slab_summary_entry.
 */
static struct slab_summary_block *
get_summary_block_for_slab(struct slab_summary_zone *summary_zone,
			   slab_count_t slab_number)
{
	slab_count_t entries_per_block =
		summary_zone->summary->entries_per_block;
	return &summary_zone->summary_blocks[slab_number / entries_per_block];
}

/**
 * vdo_update_slab_summary_entry() - Update the entry for a slab.
 * @summary_zone: The slab_summary_zone for the zone of the slab.
 * @waiter: The waiter that is updating the summary.
 * @slab_number: The slab number to update.
 * @tail_block_offset: The offset of slab journal's tail block.
 * @load_ref_counts: Whether the ref_counts must be loaded from the layer on
 *                   the next load.
 * @is_clean: Whether the slab is clean.
 * @free_blocks: The number of free blocks.
 */
void vdo_update_slab_summary_entry(struct slab_summary_zone *summary_zone,
				   struct waiter *waiter, slab_count_t slab_number,
				   tail_block_offset_t tail_block_offset,
				   bool load_ref_counts, bool is_clean,
				   block_count_t free_blocks)
{
	struct slab_summary_block *block =
		get_summary_block_for_slab(summary_zone, slab_number);
	int result;

	if (vdo_is_read_only(summary_zone->summary->read_only_notifier)) {
		result = VDO_READ_ONLY;
	} else if (vdo_is_state_draining(&summary_zone->state)
		   || vdo_is_state_quiescent(&summary_zone->state)) {
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

/**
 * vdo_get_summarized_tail_block_offset() - Get the stored tail block offset
 *                                          for a slab.
 * @summary_zone: The slab_summary_zone to use.
 * @slab_number: The slab number to get the offset for.
 *
 * Return: The tail block offset for the slab.
 */
tail_block_offset_t
vdo_get_summarized_tail_block_offset(struct slab_summary_zone *summary_zone,
				     slab_count_t slab_number)
{
	return summary_zone->entries[slab_number].tail_block_offset;
}

/**
 * vdo_must_load_ref_counts() - Whether ref_counts must be loaded from the
 *                              layer.
 * @summary_zone: The slab_summary_zone to use.
 * @slab_number: The slab number to get information for.
 *
 * Return: Whether ref_counts must be loaded.
 */
bool vdo_must_load_ref_counts(struct slab_summary_zone *summary_zone,
			      slab_count_t slab_number)
{
	return summary_zone->entries[slab_number].load_ref_counts;
}

/**
 * vdo_get_summarized_cleanliness() - Get the stored cleanliness information
 *                                    for a single slab.
 * @summary_zone: The slab_summary_zone to use.
 * @slab_number: The slab number to get information for.
 *
 * Return: Whether the slab is clean.
 */
bool vdo_get_summarized_cleanliness(struct slab_summary_zone *summary_zone,
				    slab_count_t slab_number)
{
	return !summary_zone->entries[slab_number].is_dirty;
}

/**
 * vdo_get_summarized_free_block_count() - Get the stored emptiness
 *                                         information for a single slab.
 * @summary_zone: The slab_summary_zone to use.
 * @slab_number: The slab number to get information for.
 *
 * Return: An approximation to the free blocks in the slab.
 */
block_count_t
vdo_get_summarized_free_block_count(struct slab_summary_zone *summary_zone,
				    slab_count_t slab_number)
{
	struct slab_summary_entry *entry = &summary_zone->entries[slab_number];

	return get_approximate_free_blocks(summary_zone->summary,
					   entry->fullness_hint);
}

/**
 * vdo_get_summarized_slab_statuses() - Get the stored slab statuses for all
 *                                      slabs in a zone.
 *
 * @summary_zone: The slab_summary_zone to use.
 * @slab_count: The number of slabs to fetch.
 * @statuses: An array of slab_status structures to populate (in, out).
 */
void vdo_get_summarized_slab_statuses(struct slab_summary_zone *summary_zone,
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

/* RESIZE FUNCTIONS */

/**
 * vdo_set_slab_summary_origin() - Set the origin of the slab summary relative
 *                                 to the physical layer.
 * @summary: The slab_summary to update.
 * @partition: The slab summary partition.
 */
void vdo_set_slab_summary_origin(struct slab_summary *summary,
				 struct partition *partition)
{
	summary->origin = vdo_get_fixed_layout_partition_offset(partition);
}

/* COMBINING FUNCTIONS (LOAD) */

/**
 * finish_combining_zones() - Clean up after saving out the combined slab
 *                            summary.
 * @completion: The vio which was used to write the summary data.
 **/
static void finish_combining_zones(struct vdo_completion *completion)
{
	struct slab_summary *summary = completion->parent;
	int result = completion->result;

	free_vio(as_vio(UDS_FORGET(completion)));
	vdo_finish_loading_with_result(&summary->zones[0]->state, result);
}

static void handle_combining_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	finish_combining_zones(completion);
}

/**
 * combine_zones() - Treating the current entries buffer as the on-disk value
 *                   of all zones, update every zone to the correct values for
 *                   every slab.
 * @summary: The summary whose entries should be combined.
 */
static void combine_zones(struct slab_summary *summary)
{
	/*
	 * Combine all the old summary data into the portion of the buffer
	 * corresponding to the first zone.
	 */
	zone_count_t zone = 0;

	if (summary->zones_to_combine > 1) {
		slab_count_t entry_number;

		for (entry_number = 0; entry_number < MAX_VDO_SLABS;
		     entry_number++) {
			if (zone != 0) {
				memcpy(summary->entries + entry_number,
				       summary->entries +
						(zone * MAX_VDO_SLABS) +
						entry_number,
				       sizeof(struct slab_summary_entry));
			}
			zone++;
			if (zone == summary->zones_to_combine) {
				zone = 0;
			}
		}
	}

	/* Copy the combined data to each zones's region of the buffer. */
	for (zone = 1; zone < MAX_VDO_PHYSICAL_ZONES; zone++) {
		memcpy(summary->entries + (zone * MAX_VDO_SLABS),
		       summary->entries,
		       MAX_VDO_SLABS * sizeof(struct slab_summary_entry));
	}
}

static void write_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;

	continue_vio_after_io(vio,
			      finish_combining_zones,
			      vdo_from_vio(vio)->thread_config->admin_thread);
}

/**
 * finish_loading_summary() - Finish loading slab summary data.
 * @completion: The vio which was used to read the summary data.
 *
 * Combines the slab summary data from all the previously written zones and
 * copies the combined summary to each partition's data region. Then writes
 * the combined summary back out to disk. This callback is registered in
 * vdo_load_slab_summary().
 */
static void finish_loading_summary(struct vdo_completion *completion)
{
	struct slab_summary *summary = completion->parent;

	/* Combine the zones so each zone is correct for all slabs. */
	combine_zones(summary);

	/* Write the combined summary back out. */
	submit_metadata_vio(as_vio(completion),
			    summary->origin,
			    write_summary_endio,
			    handle_combining_error,
			    REQ_OP_WRITE);
}

static void load_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;

	continue_vio_after_io(vio,
			      finish_loading_summary,
			      vdo_from_vio(vio)->thread_config->admin_thread);
}

/**
 * vdo_load_slab_summary() - Load slab summary data.
 * @summary: The summary to load.
 * @operation: The type of load to perform.
 * @zones_to_combine: The number of zones to be combined; if set to 0,
 *                    all of the summary will be initialized as new.
 * @parent: The parent of this operation.
 *
 * Reads in all the slab summary data from the slab summary partition,
 * combines all the previously used zones into a single zone, and then writes
 * the combined summary back out to each possible zones' summary region.
 */
void vdo_load_slab_summary(struct slab_summary *summary,
			   const struct admin_state_code *operation,
			   zone_count_t zones_to_combine,
			   struct vdo_completion *parent)
{
	struct vio *vio;
	block_count_t blocks;
	int result;

	struct slab_summary_zone *zone = summary->zones[0];

	if (!vdo_start_loading(&zone->state, operation, parent, NULL)) {
		return;
	}

	blocks = summary->blocks_per_zone * MAX_VDO_PHYSICAL_ZONES;
	result = create_multi_block_metadata_vio(parent->vdo,
						 VIO_TYPE_SLAB_SUMMARY,
						 VIO_PRIORITY_METADATA,
						 summary,
						 blocks,
						 (char *) summary->entries,
						 &vio);
	if (result != VDO_SUCCESS) {
		vdo_finish_loading_with_result(&zone->state, result);
		return;
	}

	if ((operation == VDO_ADMIN_STATE_FORMATTING) ||
	    (operation == VDO_ADMIN_STATE_LOADING_FOR_REBUILD)) {
		finish_loading_summary(vio_as_completion(vio));
		return;
	}

	summary->zones_to_combine = zones_to_combine;
	submit_metadata_vio(vio,
			    summary->origin,
			    load_summary_endio,
			    handle_combining_error,
			    REQ_OP_READ);
}

/**
 * vdo_get_slab_summary_statistics() - Fetch the cumulative statistics for all
 *                                     slab summary zones in a summary.
 * @summary: The summary in question.
 *
 * Return: The cumulative slab summary statistics for the summary.
 */
struct slab_summary_statistics
vdo_get_slab_summary_statistics(const struct slab_summary *summary)
{
	const struct atomic_slab_summary_statistics *atoms =
		&summary->statistics;
	return (struct slab_summary_statistics) {
		.blocks_written = atomic64_read(&atoms->blocks_written),
	};
}
