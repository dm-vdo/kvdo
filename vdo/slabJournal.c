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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournal.c#84 $
 */

#include "slabJournalInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"

#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "dataVIO.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "slabDepotInternals.h"
#include "slabSummary.h"
#include "vdo.h"

/**********************************************************************/
struct slab_journal *slab_journal_from_dirty_entry(struct list_head *entry)
{
	if (entry == NULL) {
		return NULL;
	}
	return list_entry(entry, struct slab_journal, dirty_entry);
}

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return the block number corresponding to the sequence number
 **/
static inline physical_block_number_t __must_check
get_block_number(struct slab_journal *journal, sequence_number_t sequence)
{
	tail_block_offset_t offset = get_slab_journal_block_offset(journal,
							       sequence);
	return (journal->slab->journal_origin + offset);
}

/**
 * Get the lock object for a slab journal block by sequence number.
 *
 * @param journal          vdo_slab journal to retrieve from
 * @param sequence_number  Sequence number of the block
 *
 * @return the lock object for the given sequence number
 **/
static inline struct journal_lock * __must_check
get_lock(struct slab_journal *journal, sequence_number_t sequence_number)
{
	tail_block_offset_t offset =
		get_slab_journal_block_offset(journal, sequence_number);
	return &journal->locks[offset];
}

/**
 * Check whether the VDO is in read-only mode.
 *
 * @param journal  The journal whose owning VDO should be checked
 *
 * @return <code>true</code> if the VDO is in read-only mode
 **/
static inline bool __must_check is_vdo_read_only(struct slab_journal *journal)
{
	return is_read_only(journal->slab->allocator->read_only_notifier);
}

/**
 * Check whether there are entry waiters which should delay a flush.
 *
 * @param journal  The journal to check
 *
 * @return <code>true</code> if there are no entry waiters, or if the slab
 *         is unrecovered
 **/
static inline bool __must_check
must_make_entries_to_flush(struct slab_journal *journal)
{
	return (!slab_is_rebuilding(journal->slab) &&
		has_waiters(&journal->entry_waiters));
}

/**
 * Check whether a reap is currently in progress.
 *
 * @param journal  The journal which may be reaping
 *
 * @return <code>true</code> if the journal is reaping
 **/
static inline bool __must_check is_reaping(struct slab_journal *journal)
{
	return (journal->head != journal->unreapable);
}

/**********************************************************************/
bool is_slab_journal_active(struct slab_journal *journal)
{
	return (must_make_entries_to_flush(journal) || is_reaping(journal) ||
		journal->waiting_to_commit ||
		!list_empty(&journal->uncommitted_blocks) ||
		journal->updating_slab_summary);
}

/**
 * Initialize tail block as a new block.
 *
 * @param journal  The journal whose tail block is being initialized
 **/
static void initialize_tail_block(struct slab_journal *journal)
{
	struct slab_journal_block_header *header = &journal->tail_header;
	header->sequence_number = journal->tail;
	header->entry_count = 0;
	header->has_block_map_increments = false;
}

/**
 * Set all journal fields appropriately to start journaling.
 *
 * @param journal  The journal to be reset, based on its tail sequence number
 **/
static void initialize_journal_state(struct slab_journal *journal)
{
	journal->unreapable = journal->head;
	journal->reap_lock = get_lock(journal, journal->unreapable);
	journal->next_commit = journal->tail;
	journal->summarized = journal->last_summarized = journal->tail;
	initialize_tail_block(journal);
}

/**
 * Check whether a journal block is full.
 *
 * @param journal The slab journal for the block
 *
 * @return <code>true</code> if the tail block is full
 **/
static bool __must_check block_is_full(struct slab_journal *journal)
{
	journal_entry_count_t count = journal->tail_header.entry_count;
	return (journal->tail_header.has_block_map_increments ?
			(journal->full_entries_per_block == count) :
			(journal->entries_per_block == count));
}

/**********************************************************************/
static void add_entries(struct slab_journal *journal);
static void update_tail_block_location(struct slab_journal *journal);
static void release_journal_locks(struct waiter *waiter, void *context);

/**********************************************************************/
int make_slab_journal(struct block_allocator *allocator,
		      struct vdo_slab *slab,
		      struct recovery_journal *recovery_journal,
		      struct slab_journal **journal_ptr)
{
	struct slab_journal *journal;
	const struct slab_config *slab_config =
		get_slab_config(allocator->depot);
	int result = ALLOCATE_EXTENDED(struct slab_journal,
				       slab_config->slab_journal_blocks,
				       struct journal_lock,
				       __func__,
				       &journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	journal->slab = slab;
	journal->size = slab_config->slab_journal_blocks;
	journal->flushing_threshold =
		slab_config->slab_journal_flushing_threshold;
	journal->blocking_threshold =
		slab_config->slab_journal_blocking_threshold;
	journal->scrubbing_threshold =
		slab_config->slab_journal_scrubbing_threshold;
	journal->entries_per_block = SLAB_JOURNAL_ENTRIES_PER_BLOCK;
	journal->full_entries_per_block = SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK;
	journal->events = &allocator->slab_journal_statistics;
	journal->recovery_journal = recovery_journal;
	journal->summary = get_vdo_slab_summary_zone(allocator);
	journal->tail = 1;
	journal->head = 1;

	journal->flushing_deadline = journal->flushing_threshold;
	// Set there to be some time between the deadline and the blocking
	// threshold, so that hopefully all are done before blocking.
	if ((journal->blocking_threshold - journal->flushing_threshold) > 5) {
		journal->flushing_deadline = journal->blocking_threshold - 5;
	}

	journal->slab_summary_waiter.callback = release_journal_locks;

	result = ALLOCATE(VDO_BLOCK_SIZE,
			  char,
			  "struct packed_slab_journal_block",
			  (char **)&journal->block);
	if (result != VDO_SUCCESS) {
		free_slab_journal(&journal);
		return result;
	}

	INIT_LIST_HEAD(&journal->dirty_entry);
	INIT_LIST_HEAD(&journal->uncommitted_blocks);

	journal->tail_header.nonce = slab->allocator->nonce;
	journal->tail_header.metadata_type = VDO_METADATA_SLAB_JOURNAL;
	initialize_journal_state(journal);

	*journal_ptr = journal;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_slab_journal(struct slab_journal **journal_ptr)
{
	struct slab_journal *journal = *journal_ptr;
	if (journal == NULL) {
		return;
	}

	FREE(journal->block);
	FREE(journal);
	*journal_ptr = NULL;
}

/**********************************************************************/
bool is_slab_journal_blank(const struct slab_journal *journal)
{
	return ((journal != NULL) && (journal->tail == 1) &&
		(journal->tail_header.entry_count == 0));
}

/**********************************************************************/
bool is_slab_journal_dirty(const struct slab_journal *journal)
{
	return (journal->recovery_lock != 0);
}

/**
 * Put a slab journal on the dirty ring of its allocator in the correct order.
 *
 * @param journal  The journal to be marked dirty
 * @param lock     The recovery journal lock held by the slab journal
 **/
static void mark_slab_journal_dirty(struct slab_journal *journal,
				    sequence_number_t lock)
{
	struct list_head *entry;
	struct list_head *dirty_list =
		&journal->slab->allocator->dirty_slab_journals;
	ASSERT_LOG_ONLY(!is_slab_journal_dirty(journal),
			"slab journal was clean");

	journal->recovery_lock = lock;
	list_for_each_prev(entry, dirty_list) {
		struct slab_journal *dirty_journal =
			slab_journal_from_dirty_entry(entry);
		if (dirty_journal->recovery_lock <= journal->recovery_lock) {
			break;
		}
	}

	list_move_tail(&journal->dirty_entry, entry->next);
}

/**********************************************************************/
static void mark_slab_journal_clean(struct slab_journal *journal)
{
	journal->recovery_lock = 0;
	list_del_init(&journal->dirty_entry);
}

/**
 * Implements waiter_callback. This callback is invoked on all vios waiting
 * to make slab journal entries after the VDO has gone into read-only mode.
 **/
static void abort_waiter(struct waiter *waiter, void *context __always_unused)
{
	continue_data_vio(waiter_as_data_vio(waiter), VDO_READ_ONLY);
}

/**********************************************************************/
void abort_slab_journal_waiters(struct slab_journal *journal)
{
	ASSERT_LOG_ONLY((get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"abort_slab_journal_waiters() called on correct thread");
	notify_all_waiters(&journal->entry_waiters, abort_waiter, journal);
	check_if_slab_drained(journal->slab);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All vios waiting for to make entries
 * will be awakened with an error. All flushes will complete as soon as all
 * pending IO is done.
 *
 * @param journal     The journal which has failed
 * @param error_code  The error result triggering this call
 **/
static void enter_journal_read_only_mode(struct slab_journal *journal,
					 int error_code)
{
	enter_read_only_mode(journal->slab->allocator->read_only_notifier,
			     error_code);
	abort_slab_journal_waiters(journal);
}

/**
 * Actually advance the head of the journal now that any necessary flushes
 * are complete.
 *
 * @param journal  The journal to be reaped
 **/
static void finish_reaping(struct slab_journal *journal)
{
	journal->head = journal->unreapable;
	add_entries(journal);
	check_if_slab_drained(journal->slab);
}

/**********************************************************************/
static void reap_slab_journal(struct slab_journal *journal);

/**
 * Finish reaping now that we have flushed the lower layer and then try
 * reaping again in case we deferred reaping due to an outstanding vio.
 *
 * @param completion  The flush vio
 **/
static void complete_reaping(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;
	return_vdo_block_allocator_vio(journal->slab->allocator, entry);
	finish_reaping(journal);
	reap_slab_journal(journal);
}

/**
 * Handle an error flushing the lower layer.
 *
 * @param completion  The flush vio
 **/
static void handle_flush_error(struct vdo_completion *completion)
{
	struct slab_journal *journal =
		((struct vio_pool_entry *)completion->parent)->parent;
	enter_journal_read_only_mode(journal, completion->result);
	complete_reaping(completion);
}

/**
 * A waiter callback for getting a vio with which to flush the lower
 * layer prior to reaping.
 *
 * @param waiter       The journal as a flush waiter
 * @param vio_context  The newly acquired flush vio
 **/
static void flush_for_reaping(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, flush_waiter);
	struct vio_pool_entry *entry = vio_context;
	struct vio *vio = entry->vio;

	entry->parent = journal;
	vio->completion.callback_thread_id =
		journal->slab->allocator->thread_id;
	launch_flush(vio, complete_reaping, handle_flush_error);
}

/**
 * Conduct a reap on a slab journal to reclaim unreferenced blocks.
 *
 * @param journal  The slab journal
 **/
static void reap_slab_journal(struct slab_journal *journal)
{
	bool reaped = false;
	int result;

	if (is_reaping(journal)) {
		// We already have a reap in progress so wait for it to finish.
		return;
	}

	if (is_unrecovered_slab(journal->slab) ||
	    !is_vdo_state_normal(&journal->slab->state) ||
	    is_vdo_read_only(journal)) {
		// We must not reap in the first two cases, and there's no
		// point in read-only mode.
		return;
	}

	/*
	 * Start reclaiming blocks only when the journal head has no
	 * references. Then stop when a block is referenced or reap reaches
	 * the most recently written block, referenced by the slab summary,
	 * which has the sequence number just before the tail.
	 */
	while ((journal->unreapable < journal->tail) &&
	       (journal->reap_lock->count == 0)) {
		reaped = true;
		journal->unreapable++;
		journal->reap_lock++;
		if (journal->reap_lock == &journal->locks[journal->size]) {
			journal->reap_lock = &journal->locks[0];
		}
	}

	if (!reaped) {
		return;
	}

	/*
	 * It is never safe to reap a slab journal block without first issuing
	 * a flush, regardless of whether a user flush has been received or
	 * not. In the absence of the flush, the reference block write which
	 * released the locks allowing the slab journal to reap may not be
	 * persisted. Although slab summary writes will eventually issue
	 * flushes, multiple slab journal block writes can be issued while
	 * previous slab summary updates have not yet been made. Even though
	 * those slab journal block writes will be ignored if the slab summary
	 * update is not persisted, they may still overwrite the to-be-reaped
	 * slab journal block resulting in a loss of reference count updates
	 * (VDO-2912).
	 */
	journal->flush_waiter.callback = flush_for_reaping;
	result = acquire_vdo_block_allocator_vio(journal->slab->allocator,
						 &journal->flush_waiter);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		return;
	}
}

/**
 * This is the callback invoked after a slab summary update completes. It
 * is registered in the constructor on behalf of update_tail_block_location().
 *
 * Implements waiter_callback.
 *
 * @param waiter        The slab summary waiter that has just been notified
 * @param context       The result code of the update
 **/
static void release_journal_locks(struct waiter *waiter, void *context)
{
	sequence_number_t first, i;
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, slab_summary_waiter);
	int result = *((int *)context);
	if (result != VDO_SUCCESS) {
		if (result != VDO_READ_ONLY) {
			// Don't bother logging what might be lots of errors if
			// we are already in read-only mode.
			log_error_strerror(result,
					   "failed slab summary update %llu",
					   journal->summarized);
		}

		journal->updating_slab_summary = false;
		enter_journal_read_only_mode(journal, result);
		return;
	}

	if (journal->partial_write_in_progress &&
	    (journal->summarized == journal->tail)) {
		journal->partial_write_in_progress = false;
		add_entries(journal);
	}

	first = journal->last_summarized;
	journal->last_summarized = journal->summarized;
	for (i = journal->summarized - 1; i >= first; i--) {
		// Release the lock the summarized block held on the recovery
		// journal. (During replay, recovery_start will always be 0.)
		if (journal->recovery_journal != NULL) {
			zone_count_t zone_number =
				journal->slab->allocator->zone_number;
			release_recovery_journal_block_reference(journal->recovery_journal,
								 get_lock(journal, i)->recovery_start,
								 ZONE_TYPE_PHYSICAL,
								 zone_number);
		}

		// Release our own lock against reaping for blocks that are
		// committed. (This function will not change locks during
		// replay.)
		adjust_slab_journal_block_reference(journal, i, -1);
	}

	journal->updating_slab_summary = false;

	reap_slab_journal(journal);

	// Check if the slab summary needs to be updated again.
	update_tail_block_location(journal);
}

/**
 * Update the tail block location in the slab summary, if necessary.
 *
 * @param journal  The slab journal that is updating its tail block location
 **/
static void update_tail_block_location(struct slab_journal *journal)
{
	block_count_t free_block_count;
	tail_block_offset_t block_offset;

	if (journal->updating_slab_summary || is_vdo_read_only(journal) ||
	    (journal->last_summarized >= journal->next_commit)) {
		check_if_slab_drained(journal->slab);
		return;
	}

	if (is_unrecovered_slab(journal->slab)) {
		free_block_count =
			get_summarized_free_block_count(journal->summary,
							journal->slab->slab_number);
	} else {
		free_block_count = get_slab_free_block_count(journal->slab);
	}

	journal->summarized = journal->next_commit;
	journal->updating_slab_summary = true;

	/*
	 * Update slab summary as dirty.
	 * vdo_slab journal can only reap past sequence number 1 when all the
	 * ref counts for this slab have been written to the layer. Therefore,
	 * indicate that the ref counts must be loaded when the journal head
	 * has reaped past sequence number 1.
	 */
	block_offset =
		get_slab_journal_block_offset(journal, journal->summarized);
	update_slab_summary_entry(journal->summary,
				  &journal->slab_summary_waiter,
				  journal->slab->slab_number,
				  block_offset,
				  (journal->head > 1),
				  false,
				  free_block_count);
}

/**********************************************************************/
void reopen_slab_journal(struct slab_journal *journal)
{
	sequence_number_t block;

	ASSERT_LOG_ONLY(journal->tail_header.entry_count == 0,
			"vdo_slab journal's active block empty before reopening");
	journal->head = journal->tail;
	initialize_journal_state(journal);

	// Ensure no locks are spuriously held on an empty journal.
	for (block = 1; block <= journal->size; block++) {
		ASSERT_LOG_ONLY((get_lock(journal, block)->count == 0),
				"Scrubbed journal's block %llu is not locked",
				block);
	}

	add_entries(journal);
}

/**********************************************************************/
static sequence_number_t
get_committing_sequence_number(const struct vio_pool_entry *entry)
{
	const struct packed_slab_journal_block *block = entry->buffer;
	return __le64_to_cpu(block->header.sequence_number);
}

/**
 * Handle post-commit processing. This is the callback registered by
 * write_slab_journal_block().
 *
 * @param completion  The write vio as a completion
 **/
static void complete_write(struct vdo_completion *completion)
{
	int write_result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;

	sequence_number_t committed = get_committing_sequence_number(entry);
	list_del_init(&entry->available_entry);
	return_vdo_block_allocator_vio(journal->slab->allocator, entry);

	if (write_result != VDO_SUCCESS) {
		log_error_strerror(write_result,
				   "cannot write slab journal block %llu",
				   committed);
		enter_journal_read_only_mode(journal, write_result);
		return;
	}

	WRITE_ONCE(journal->events->blocks_written,
		   journal->events->blocks_written + 1);

	if (list_empty(&journal->uncommitted_blocks)) {
		// If no blocks are outstanding, then the commit point is at
		// the tail.
		journal->next_commit = journal->tail;
	} else {
		// The commit point is always the beginning of the oldest
		// incomplete block.
		struct vio_pool_entry *oldest =
			as_vio_pool_entry(journal->uncommitted_blocks.next);
		journal->next_commit = get_committing_sequence_number(oldest);
	}

	update_tail_block_location(journal);
}

/**
 * Callback from acquire_vdo_block_allocator_vio() registered in
 * commit_slab_journal_tail().
 *
 * @param waiter       The vio pool waiter which was just notified
 * @param vio_context  The vio pool entry for the write
 **/
static void write_slab_journal_block(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, resource_waiter);
	struct vio_pool_entry *entry = vio_context;
	struct slab_journal_block_header *header = &journal->tail_header;
	int unused_entries = journal->entries_per_block - header->entry_count;
	physical_block_number_t block_number;
	enum admin_state_code operation;

	header->head = journal->head;
	list_move_tail(&entry->available_entry, &journal->uncommitted_blocks);
	pack_slab_journal_block_header(header, &journal->block->header);

	// Copy the tail block into the vio.
	memcpy(entry->buffer, journal->block, VDO_BLOCK_SIZE);

	ASSERT_LOG_ONLY(unused_entries >= 0,
			"vdo_slab journal block is not overfull");
	if (unused_entries > 0) {
		// Release the per-entry locks for any unused entries in the
		// block we are about to write.
		adjust_slab_journal_block_reference(journal,
						    header->sequence_number,
						    -unused_entries);
		journal->partial_write_in_progress = !block_is_full(journal);
	}

	block_number = get_block_number(journal, header->sequence_number);

	entry->parent = journal;
	entry->vio->completion.callback_thread_id =
		journal->slab->allocator->thread_id;
	/*
	 * This block won't be read in recovery until the slab summary is
	 * updated to refer to it. The slab summary update does a flush which
	 * is sufficient to protect us from VDO-2331.
	 */
	launch_write_metadata_vio(entry->vio, block_number, complete_write,
				  complete_write);

	// Since the write is submitted, the tail block structure can be reused.
	journal->tail++;
	initialize_tail_block(journal);
	journal->waiting_to_commit = false;

	operation = get_vdo_admin_state_code(&journal->slab->state);
	if (operation == ADMIN_STATE_WAITING_FOR_RECOVERY) {
		finish_vdo_operation_with_result(&journal->slab->state,
						 (is_vdo_read_only(journal) ?
						  VDO_READ_ONLY : VDO_SUCCESS));
		return;
	}

	add_entries(journal);
}

/**********************************************************************/
void commit_slab_journal_tail(struct slab_journal *journal)
{
	int result;

	if ((journal->tail_header.entry_count == 0) &&
	    must_make_entries_to_flush(journal)) {
		// There are no entries at the moment, but there are some
		// waiters, so defer initiating the flush until those entries
		// are ready to write.
		return;
	}

	if (is_vdo_read_only(journal) || journal->waiting_to_commit ||
	    (journal->tail_header.entry_count == 0)) {
		// There is nothing to do since the tail block is empty, or
		// writing, or the journal is in read-only mode.
		return;
	}

	/*
	 * Since we are about to commit the tail block, this journal no longer
	 * needs to be on the ring of journals which the recovery journal might
	 * ask to commit.
	 */
	mark_slab_journal_clean(journal);

	journal->waiting_to_commit = true;

	journal->resource_waiter.callback = write_slab_journal_block;
	result = acquire_vdo_block_allocator_vio(journal->slab->allocator,
						 &journal->resource_waiter);
	if (result != VDO_SUCCESS) {
		journal->waiting_to_commit = false;
		enter_journal_read_only_mode(journal, result);
		return;
	}
}

/**********************************************************************/
void encode_slab_journal_entry(struct slab_journal_block_header *tail_header,
			       slab_journal_payload *payload,
			       slab_block_number sbn,
			       enum journal_operation operation)
{
	journal_entry_count_t entry_number = tail_header->entry_count++;
	if (operation == BLOCK_MAP_INCREMENT) {
		if (!tail_header->has_block_map_increments) {
			memset(payload->full_entries.entry_types,
			       0,
			       SLAB_JOURNAL_ENTRY_TYPES_SIZE);
			tail_header->has_block_map_increments = true;
		}

		payload->full_entries.entry_types[entry_number / 8] |=
			((byte)1 << (entry_number % 8));
	}

	pack_slab_journal_entry(&payload->entries[entry_number],
			     sbn,
			     is_increment_operation(operation));
}

/**
 * Actually add an entry to the slab journal, potentially firing off a write
 * if a block becomes full. This function is synchronous.
 *
 * @param journal         The slab journal to append to
 * @param pbn             The pbn being adjusted
 * @param operation       The type of entry to make
 * @param recovery_point  The recovery journal point for this entry
 **/
static void add_entry(struct slab_journal *journal,
		      physical_block_number_t pbn,
		      enum journal_operation operation,
		      const struct journal_point *recovery_point)
{
	struct packed_slab_journal_block *block = journal->block;

	int result =
		ASSERT(before_journal_point(&journal->tail_header.recovery_point,
					    recovery_point),
		       "recovery journal point is monotonically increasing, recovery point: %llu.%u, block recovery point: %llu.%u",
		       recovery_point->sequence_number,
		       recovery_point->entry_count,
		       journal->tail_header.recovery_point.sequence_number,
		       journal->tail_header.recovery_point.entry_count);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		return;
	}

	if (operation == BLOCK_MAP_INCREMENT) {
		result = ASSERT_LOG_ONLY((journal->tail_header.entry_count <
					  journal->full_entries_per_block),
					 "block has room for full entries");
		if (result != VDO_SUCCESS) {
			enter_journal_read_only_mode(journal, result);
			return;
		}
	}

	encode_slab_journal_entry(&journal->tail_header,
				  &block->payload,
				  pbn - journal->slab->start,
				  operation);
	journal->tail_header.recovery_point = *recovery_point;
	if (block_is_full(journal)) {
		commit_slab_journal_tail(journal);
	}
}

/**********************************************************************/
bool attempt_replay_into_slab_journal(struct slab_journal *journal,
				      physical_block_number_t pbn,
				      enum journal_operation operation,
				      struct journal_point *recovery_point,
				      struct vdo_completion *parent)
{
	struct slab_journal_block_header *header = &journal->tail_header;

	// Only accept entries after the current recovery point.
	if (!before_journal_point(&journal->tail_header.recovery_point,
				  recovery_point)) {
		return true;
	}

	if ((header->entry_count >= journal->full_entries_per_block) &&
	    (header->has_block_map_increments ||
	     (operation == BLOCK_MAP_INCREMENT))) {
		// The tail block does not have room for the entry we are
		// attempting to add so commit the tail block now.
		commit_slab_journal_tail(journal);
	}

	if (journal->waiting_to_commit) {
		start_vdo_operation_with_waiter(&journal->slab->state,
						ADMIN_STATE_WAITING_FOR_RECOVERY,
						parent,
						NULL);
		return false;
	}

	if ((journal->tail - journal->head) >= journal->size) {
		/*
		 * We must have reaped the current head before the crash, since
		 * the blocked threshold keeps us from having more entries than
		 * fit in a slab journal; hence we can just advance the head
		 * (and unreapable block), as needed.
		 */
		journal->head++;
		journal->unreapable++;
	}

	mark_slab_replaying(journal->slab);
	add_entry(journal, pbn, operation, recovery_point);
	return true;
}

/**
 * Check whether the journal should be saving reference blocks out.
 *
 * @param journal       The journal to check
 *
 * @return true if the journal should be requesting reference block writes
 **/
static bool requires_flushing(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);
	return (journal_length >= journal->flushing_threshold);
}

/**
 * Check whether the journal must be reaped before adding new entries.
 *
 * @param journal       The journal to check
 *
 * @return true if the journal must be reaped
 **/
static bool requires_reaping(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);
	return (journal_length >= journal->blocking_threshold);
}

/**********************************************************************/
bool requires_scrubbing(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);
	return (journal_length >= journal->scrubbing_threshold);
}

/**
 * Implements waiter_callback. This callback is invoked by add_entries() once
 * it has determined that we are ready to make another entry in the slab
 * journal.
 *
 * @param waiter        The vio which should make an entry now
 * @param context       The slab journal to make an entry in
 **/
static void add_entry_from_waiter(struct waiter *waiter, void *context)
{
	int result;
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct slab_journal *journal = (struct slab_journal *)context;
	struct slab_journal_block_header *header = &journal->tail_header;
	struct journal_point slab_journal_point = {
		.sequence_number = header->sequence_number,
		.entry_count = header->entry_count,
	};
	sequence_number_t recovery_block =
		data_vio->recovery_journal_point.sequence_number;

	if (header->entry_count == 0) {
		/*
		 * This is the first entry in the current tail block, so get a
		 * lock on the recovery journal which we will hold until this
		 * tail block is committed.
		 */
		get_lock(journal, header->sequence_number)->recovery_start =
			 recovery_block;
		if (journal->recovery_journal != NULL) {
			zone_count_t zone_number =
				journal->slab->allocator->zone_number;
			acquire_recovery_journal_block_reference(journal->recovery_journal,
								 recovery_block,
								 ZONE_TYPE_PHYSICAL,
								 zone_number);
		}
		mark_slab_journal_dirty(journal, recovery_block);

		// If the slab journal is over the first threshold, tell the
		// ref_counts to write some reference blocks soon.
		if (requires_flushing(journal)) {
			block_count_t journal_length =
				(journal->tail - journal->head);
			block_count_t blocks_to_deadline = 0;
			WRITE_ONCE(journal->events->flush_count,
				   journal->events->flush_count + 1);
			if (journal_length <= journal->flushing_deadline) {
				blocks_to_deadline = journal->flushing_deadline -
						   journal_length;
			}
			save_several_reference_blocks(journal->slab->reference_counts,
						      blocks_to_deadline + 1);
		}
	}

	add_entry(journal,
		  data_vio->operation.pbn,
		  data_vio->operation.type,
		  &data_vio->recovery_journal_point);

	// Now that an entry has been made in the slab journal, update the
	// reference counts.
	result = modify_slab_reference_count(journal->slab,
					     &slab_journal_point,
					     data_vio->operation);
	continue_data_vio(data_vio, result);
}

/**
 * Check whether the next entry to be made is a block map increment.
 *
 * @param journal  The journal
 *
 * @return <code>true</code> if the first entry waiter's operation is a block
 *         map increment
 **/
static inline bool
is_next_entry_a_block_map_increment(struct slab_journal *journal)
{
	struct data_vio *data_vio =
		waiter_as_data_vio(get_first_waiter(&journal->entry_waiters));
	return (data_vio->operation.type == BLOCK_MAP_INCREMENT);
}

/**
 * Add as many entries as possible from the queue of vios waiting to make
 * entries. By processing the queue in order, we ensure that slab journal
 * entries are made in the same order as recovery journal entries for the
 * same increment or decrement.
 *
 * @param journal  The journal to which entries may be added
 **/
static void add_entries(struct slab_journal *journal)
{
	if (journal->adding_entries) {
		// Protect against re-entrancy.
		return;
	}

	journal->adding_entries = true;
	while (has_waiters(&journal->entry_waiters)) {
		struct slab_journal_block_header *header = &journal->tail_header;
		if (journal->partial_write_in_progress ||
		    slab_is_rebuilding(journal->slab)) {
			// Don't add entries while rebuilding or while a
			// partial write is outstanding (VDO-2399).
			break;
		}

		if (journal->waiting_to_commit) {
			// If we are waiting for resources to write the tail
			// block, and the tail block is full, we can't make
			// another entry.
			WRITE_ONCE(journal->events->tail_busy_count,
				   journal->events->tail_busy_count + 1);
			break;
		} else if (is_next_entry_a_block_map_increment(journal) &&
			   (header->entry_count >=
			    journal->full_entries_per_block)) {
			// The tail block does not have room for a block map
			// increment, so commit it now.
			commit_slab_journal_tail(journal);
			if (journal->waiting_to_commit) {
				WRITE_ONCE(journal->events->tail_busy_count,
					   journal->events->tail_busy_count
					   + 1);
				break;
			}
		}

		// If the slab is over the blocking threshold, make the vio
		// wait.
		if (requires_reaping(journal)) {
			WRITE_ONCE(journal->events->blocked_count,
				   journal->events->blocked_count + 1);
			save_dirty_reference_blocks(journal->slab->reference_counts);
			break;
		}

		if (header->entry_count == 0) {
			struct journal_lock *lock =
				get_lock(journal, header->sequence_number);
			// Check if the on disk slab journal is full. Because
			// of the blocking and scrubbing thresholds, this
			// should never happen.
			if (lock->count > 0) {
				ASSERT_LOG_ONLY((journal->head + journal->size) ==
						journal->tail,
						"New block has locks, but journal is not full");

				/*
				 * The blocking threshold must let the journal
				 * fill up if the new block has locks; if the
				 * blocking threshold is smaller than the
				 * journal size, the new block cannot possibly
				 * have locks already.
				 */
				ASSERT_LOG_ONLY((journal->blocking_threshold >=
						 journal->size),
						"New block can have locks already iff blocking threshold is at the end of the journal");

				WRITE_ONCE(journal->events->disk_full_count,
					   journal->events->disk_full_count
					   + 1);
				save_dirty_reference_blocks(journal->slab->reference_counts);
				break;
			}

			/*
			 * Don't allow the new block to be reaped until all of
			 * the reference count blocks are written and the
			 * journal block has been fully committed as well.
			 */
			lock->count = journal->entries_per_block + 1;

			if (header->sequence_number == 1) {
				/*
				 * This is the first entry in this slab journal,
				 * ever. Dirty all of the reference count
				 * blocks. Each will acquire a lock on the tail
				 * block so that the journal won't be reaped
				 * until the reference counts are initialized.
				 * The lock acquisition must be done by the
				 * ref_counts since here we don't know how many
				 * reference blocks the ref_counts has.
				 */
				acquire_dirty_block_locks(journal->slab->reference_counts);
			}
		}

		notify_next_waiter(&journal->entry_waiters,
				   add_entry_from_waiter,
				   journal);
	}

	journal->adding_entries = false;

	// If there are no waiters, and we are flushing or saving, commit the
	// tail block.
	if (is_slab_draining(journal->slab) &&
	    !is_vdo_state_suspending(&journal->slab->state) &&
	    !has_waiters(&journal->entry_waiters)) {
		commit_slab_journal_tail(journal);
	}
}

/**********************************************************************/
void add_slab_journal_entry(struct slab_journal *journal,
			    struct data_vio *data_vio)
{
	int result;

	if (!is_slab_open(journal->slab)) {
		continue_data_vio(data_vio, VDO_INVALID_ADMIN_STATE);
		return;
	}

	if (is_vdo_read_only(journal)) {
		continue_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	result = enqueue_data_vio(&journal->entry_waiters, data_vio);
	if (result != VDO_SUCCESS) {
		continue_data_vio(data_vio, result);
		return;
	}

	if (is_unrecovered_slab(journal->slab) && requires_reaping(journal)) {
		increase_vdo_slab_scrubbing_priority(journal->slab);
	}

	add_entries(journal);
}

/**********************************************************************/
void adjust_slab_journal_block_reference(struct slab_journal *journal,
					 sequence_number_t sequence_number,
					 int adjustment)
{
	struct journal_lock *lock;

	if (sequence_number == 0) {
		return;
	}

	if (is_replaying_slab(journal->slab)) {
		// Locks should not be used during offline replay.
		return;
	}

	ASSERT_LOG_ONLY((adjustment != 0), "adjustment must be non-zero");
	lock = get_lock(journal, sequence_number);
	if (adjustment < 0) {
		ASSERT_LOG_ONLY((-adjustment <= lock->count),
				"adjustment %d of lock count %u for slab journal block %llu must not underflow",
			        adjustment, lock->count, sequence_number);
	}

	lock->count += adjustment;
	if (lock->count == 0) {
		reap_slab_journal(journal);
	}
}

/**********************************************************************/
bool release_recovery_journal_lock(struct slab_journal *journal,
				   sequence_number_t recovery_lock)
{
	if (recovery_lock > journal->recovery_lock) {
		ASSERT_LOG_ONLY((recovery_lock < journal->recovery_lock),
				"slab journal recovery lock is not older than the recovery journal head");
		return false;
	}

	if ((recovery_lock < journal->recovery_lock) ||
	    is_vdo_read_only(journal)) {
		return false;
	}

	// All locks are held by the block which is in progress; write it.
	commit_slab_journal_tail(journal);
	return true;
}

/**********************************************************************/
void drain_slab_journal(struct slab_journal *journal)
{
	ASSERT_LOG_ONLY((get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"drain_slab_journal() called on correct thread");
	if (is_vdo_state_quiescing(&journal->slab->state)) {
		// XXX: we should revisit this assertion since it is no longer
		// clear what it is for.
		ASSERT_LOG_ONLY((!(slab_is_rebuilding(journal->slab) &&
				   has_waiters(&journal->entry_waiters))),
				"slab is recovered or has no waiters");
	}

	switch (get_vdo_admin_state_code(&journal->slab->state)) {
	case ADMIN_STATE_REBUILDING:
	case ADMIN_STATE_SUSPENDING:
	case ADMIN_STATE_SAVE_FOR_SCRUBBING:
		break;

	default:
		commit_slab_journal_tail(journal);
	}
}

/**
 * Finish the decode process by returning the vio and notifying the slab that
 * we're done.
 *
 * @param completion  The vio as a completion
 **/
static void finish_decoding_journal(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;
	return_vdo_block_allocator_vio(journal->slab->allocator, entry);
	notify_slab_journal_is_loaded(journal->slab, result);
}

/**
 * Set up the in-memory journal state to the state which was written to disk.
 * This is the callback registered in read_slab_journal_tail().
 *
 * @param completion  The vio which was used to read the journal tail
 **/
static void set_decoded_state(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;
	struct packed_slab_journal_block *block = entry->buffer;

	struct slab_journal_block_header header;
	unpack_slab_journal_block_header(&block->header, &header);

	if ((header.metadata_type != VDO_METADATA_SLAB_JOURNAL) ||
	    (header.nonce != journal->slab->allocator->nonce)) {
		finish_decoding_journal(completion);
		return;
	}

	journal->tail = header.sequence_number + 1;

	// If the slab is clean, this implies the slab journal is empty, so
	// advance the head appropriately.
	if (get_summarized_cleanliness(journal->summary,
				       journal->slab->slab_number)) {
		journal->head = journal->tail;
	} else {
		journal->head = header.head;
	}

	journal->tail_header = header;
	initialize_journal_state(journal);
	finish_decoding_journal(completion);
}

/**
 * This reads the slab journal tail block by using a vio acquired from the vio
 * pool. This is the success callback from acquire_vdo_block_allocator_vio()
 * when decoding the slab journal.
 *
 * @param waiter       The vio pool waiter which has just been notified
 * @param vio_context  The vio pool entry given to the waiter
 **/
static void read_slab_journal_tail(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, resource_waiter);
	struct vdo_slab *slab = journal->slab;
	struct vio_pool_entry *entry = vio_context;
	tail_block_offset_t last_commit_point =
		get_summarized_tail_block_offset(journal->summary,
						 slab->slab_number);
	// Slab summary keeps the commit point offset, so the tail block is
	// the block before that. Calculation supports small journals in unit
	// tests.
	tail_block_offset_t tail_block =
		((last_commit_point == 0) ? (tail_block_offset_t)(journal->size - 1) :
		 (last_commit_point - 1));

	entry->parent = journal;

	entry->vio->completion.callback_thread_id = slab->allocator->thread_id;
	launch_read_metadata_vio(entry->vio,
				 slab->journal_origin + tail_block,
				 set_decoded_state,
				 finish_decoding_journal);
}

/**********************************************************************/
void decode_slab_journal(struct slab_journal *journal)
{
	struct vdo_slab *slab = journal->slab;
	tail_block_offset_t last_commit_point;
	int result;
	ASSERT_LOG_ONLY((get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"decode_slab_journal() called on correct thread");
	last_commit_point =
		get_summarized_tail_block_offset(journal->summary,
						 slab->slab_number);
	if ((last_commit_point == 0) &&
	    !must_load_ref_counts(journal->summary, slab->slab_number)) {
		/*
		 * This slab claims that it has a tail block at (journal->size
		 * - 1), but a head of 1. This is impossible, due to the
		 * scrubbing threshold, on a real system, so don't bother
		 * reading the (bogus) data off disk.
		 */
		ASSERT_LOG_ONLY(((journal->size < 16) ||
				 (journal->scrubbing_threshold < (journal->size - 1))),
				"Scrubbing threshold protects against reads of unwritten slab journal blocks");
		notify_slab_journal_is_loaded(slab, VDO_SUCCESS);
		return;
	}

	journal->resource_waiter.callback = read_slab_journal_tail;
	result = acquire_vdo_block_allocator_vio(slab->allocator,
						 &journal->resource_waiter);
	if (result != VDO_SUCCESS) {
		notify_slab_journal_is_loaded(slab, result);
	}
}

/**********************************************************************/
void dump_slab_journal(const struct slab_journal *journal)
{
	log_info("  slab journal: entry_waiters=%zu waiting_to_commit=%s" " updating_slab_summary=%s head=%llu unreapable=%llu tail=%llu next_commit=%llu summarized=%llu last_summarized=%llu recovery_lock=%llu dirty=%s",
		 count_waiters(&journal->entry_waiters),
		 bool_to_string(journal->waiting_to_commit),
		 bool_to_string(journal->updating_slab_summary),
		 journal->head,
		 journal->unreapable,
		 journal->tail,
		 journal->next_commit,
		 journal->summarized,
		 journal->last_summarized,
		 journal->recovery_lock,
		 bool_to_string(is_slab_journal_dirty(journal)));
	// Given the frequency with which the locks are just a tiny bit off, it
	// might be worth dumping all the locks, but that might be too much
	// logging.
}
