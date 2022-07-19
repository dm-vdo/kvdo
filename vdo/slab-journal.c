// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab-journal.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "data-vio.h"
#include "io-submitter.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vio.h"

/**
 * vdo_slab_journal_from_dirty_entry() - Obtain a pointer to a slab_journal
 *                                       structure from a pointer to the dirty
 *                                       list entry field within it.
 * @entry: The list entry to convert.
 *
 * Return: The entry as a slab_journal.
 */
struct slab_journal *vdo_slab_journal_from_dirty_entry(struct list_head *entry)
{
	if (entry == NULL) {
		return NULL;
	}
	return list_entry(entry, struct slab_journal, dirty_entry);
}

/**
 * get_block_number() - Get the physical block number for a given sequence
 *                      number.
 * @journal: The journal.
 * @sequence: The sequence number of the desired block.
 *
 * Return: The block number corresponding to the sequence number.
 */
static inline physical_block_number_t __must_check
get_block_number(struct slab_journal *journal, sequence_number_t sequence)
{
	tail_block_offset_t offset = vdo_get_slab_journal_block_offset(journal,
							       sequence);
	return (journal->slab->journal_origin + offset);
}

/**
 * get_lock() - Get the lock object for a slab journal block by sequence
 *              number.
 * @journal: vdo_slab journal to retrieve from.
 * @sequence_number: Sequence number of the block.
 *
 * Return: The lock object for the given sequence number.
 */
static inline struct journal_lock * __must_check
get_lock(struct slab_journal *journal, sequence_number_t sequence_number)
{
	tail_block_offset_t offset =
		vdo_get_slab_journal_block_offset(journal, sequence_number);
	return &journal->locks[offset];
}

/**
 * is_vdo_read_only() - Check whether the VDO is in read-only mode.
 * @journal: The journal whose owning VDO should be checked.
 *
 * Return: true if the VDO is in read-only mode.
 */
static inline bool __must_check is_vdo_read_only(struct slab_journal *journal)
{
	return vdo_is_read_only(journal->slab->allocator->read_only_notifier);
}

/**
 * must_make_entries_to_flush() - Check whether there are entry waiters which
 *                                should delay a flush.
 * @journal: The journal to check.
 *
 * Return: true if there are no entry waiters, or if the slab is unrecovered.
 */
static inline bool __must_check
must_make_entries_to_flush(struct slab_journal *journal)
{
	return (!vdo_is_slab_rebuilding(journal->slab) &&
		has_waiters(&journal->entry_waiters));
}

/**
 * is_reaping() - Check whether a reap is currently in progress.
 * @journal: The journal which may be reaping.
 *
 * Return: true if the journal is reaping.
 */
static inline bool __must_check is_reaping(struct slab_journal *journal)
{
	return (journal->head != journal->unreapable);
}

/**
 * vdo_is_slab_journal_active() - Check whether a slab journal is active.
 * @journal: The slab journal to check.
 *
 * Return: true if the journal is active.
 */
bool vdo_is_slab_journal_active(struct slab_journal *journal)
{
	return (must_make_entries_to_flush(journal) ||
		is_reaping(journal) ||
		journal->waiting_to_commit ||
		!list_empty(&journal->uncommitted_blocks) ||
		journal->updating_slab_summary);
}

/**
 * initialize_tail_block() - Initialize tail block as a new block.
 * @journal: The journal whose tail block is being initialized.
 */
static void initialize_tail_block(struct slab_journal *journal)
{
	struct slab_journal_block_header *header = &journal->tail_header;

	header->sequence_number = journal->tail;
	header->entry_count = 0;
	header->has_block_map_increments = false;
}

/**
 * initialize_journal_state() - Set all journal fields appropriately to start
 *                              journaling.
 * @journal: The journal to be reset, based on its tail sequence number.
 */
static void initialize_journal_state(struct slab_journal *journal)
{
	journal->unreapable = journal->head;
	journal->reap_lock = get_lock(journal, journal->unreapable);
	journal->next_commit = journal->tail;
	journal->summarized = journal->last_summarized = journal->tail;
	initialize_tail_block(journal);
}

/**
 * block_is_full() - Check whether a journal block is full.
 * @journal: The slab journal for the block.
 *
 * Return: true if the tail block is full.
 */
static bool __must_check block_is_full(struct slab_journal *journal)
{
	journal_entry_count_t count = journal->tail_header.entry_count;

	return (journal->tail_header.has_block_map_increments ?
			(journal->full_entries_per_block == count) :
			(journal->entries_per_block == count));
}

static void add_entries(struct slab_journal *journal);
static void update_tail_block_location(struct slab_journal *journal);
static void release_journal_locks(struct waiter *waiter, void *context);

/**
 * vdo_make_slab_journal() - Create a slab journal.
 * @allocator: The block allocator which owns this journal.
 * @slab: The parent slab of the journal.
 * @recovery_journal: The recovery journal of the VDO.
 * @journal_ptr: The pointer to hold the new slab journal.
 *
 * Return: VDO_SUCCESS or error code.
 */
int vdo_make_slab_journal(struct block_allocator *allocator,
			  struct vdo_slab *slab,
			  struct recovery_journal *recovery_journal,
			  struct slab_journal **journal_ptr)
{
	struct slab_journal *journal;
	const struct slab_config *slab_config =
		vdo_get_slab_config(allocator->depot);
	int result = UDS_ALLOCATE_EXTENDED(struct slab_journal,
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
	journal->entries_per_block = VDO_SLAB_JOURNAL_ENTRIES_PER_BLOCK;
	journal->full_entries_per_block = VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK;
	journal->events = &allocator->slab_journal_statistics;
	journal->recovery_journal = recovery_journal;
	journal->summary = allocator->summary;
	journal->tail = 1;
	journal->head = 1;

	journal->flushing_deadline = journal->flushing_threshold;
	/*
	 * Set there to be some time between the deadline and the blocking
	 * threshold, so that hopefully all are done before blocking.
	 */
	if ((journal->blocking_threshold - journal->flushing_threshold) > 5) {
		journal->flushing_deadline = journal->blocking_threshold - 5;
	}

	journal->slab_summary_waiter.callback = release_journal_locks;

	result = UDS_ALLOCATE(VDO_BLOCK_SIZE,
			      char,
			      "struct packed_slab_journal_block",
			      (char **)&journal->block);
	if (result != VDO_SUCCESS) {
		vdo_free_slab_journal(journal);
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

/**
 * vdo_free_slab_journal() - Free a slab journal.
 * @journal: The slab journal to free.
 */
void vdo_free_slab_journal(struct slab_journal *journal)
{
	if (journal == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(journal->block));
	UDS_FREE(journal);
}

/**
 * vdo_is_slab_journal_blank() - Check whether a slab journal is blank.
 * @journal: The journal to query.
 *
 * A slab journal is blank if it has never had any entries recorded in it.
 *
 * Return: true if the slab journal has never been modified.
 */
bool vdo_is_slab_journal_blank(const struct slab_journal *journal)
{
	return ((journal != NULL) && (journal->tail == 1) &&
		(journal->tail_header.entry_count == 0));
}

/**
 * vdo_is_slab_journal_dirty() - Check whether the slab journal is on the
 *                               block allocator's list of dirty journals.
 * @journal: The journal to query.
 *
 * Return: true if the journal has been added to the dirty list.
 */
static bool
vdo_is_slab_journal_dirty(const struct slab_journal *journal)
{
	return (journal->recovery_lock != 0);
}

/**
 * mark_slab_journal_dirty() - Put a slab journal on the dirty ring of its
 *                             allocator in the correct order.
 * @journal: The journal to be marked dirty.
 * @lock: The recovery journal lock held by the slab journal.
 */
static void mark_slab_journal_dirty(struct slab_journal *journal,
				    sequence_number_t lock)
{
	struct list_head *entry;
	struct list_head *dirty_list =
		&journal->slab->allocator->dirty_slab_journals;
	ASSERT_LOG_ONLY(!vdo_is_slab_journal_dirty(journal),
			"slab journal was clean");

	journal->recovery_lock = lock;
	list_for_each_prev(entry, dirty_list) {
		struct slab_journal *dirty_journal =
			vdo_slab_journal_from_dirty_entry(entry);
		if (dirty_journal->recovery_lock <= journal->recovery_lock) {
			break;
		}
	}

	list_move_tail(&journal->dirty_entry, entry->next);
}

static void mark_slab_journal_clean(struct slab_journal *journal)
{
	journal->recovery_lock = 0;
	list_del_init(&journal->dirty_entry);
}

/**
 * abort_waiter() - Abort vios waiting to make journal entries when read-only.
 *
 * This callback is invoked on all vios waiting to make slab journal entries
 * after the VDO has gone into read-only mode. Implements waiter_callback.
 */
static void abort_waiter(struct waiter *waiter, void *context __always_unused)
{
	continue_data_vio(waiter_as_data_vio(waiter), VDO_READ_ONLY);
}

/**
 * vdo_abort_slab_journal_waiters() - Abort any VIOs waiting to make slab
 *                                    journal entries.
 * @journal: The journal to abort.
 */
void vdo_abort_slab_journal_waiters(struct slab_journal *journal)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"vdo_abort_slab_journal_waiters() called on correct thread");
	notify_all_waiters(&journal->entry_waiters, abort_waiter, journal);
	vdo_check_if_slab_drained(journal->slab);
}

/**
 * enter_journal_read_only_mode() - Put the journal in read-only mode.
 * @journal: The journal which has failed.
 * @error_code: The error result triggering this call.
 *
 * All attempts to add entries after this function is called will fail. All
 * vios waiting for to make entries will be awakened with an error. All
 * flushes will complete as soon as all pending IO is done.
 */
static void enter_journal_read_only_mode(struct slab_journal *journal,
					 int error_code)
{
	vdo_enter_read_only_mode(journal->slab->allocator->read_only_notifier,
				 error_code);
	vdo_abort_slab_journal_waiters(journal);
}

/**
 * finish_reaping() - Actually advance the head of the journal now that any
 *                    necessary flushes are complete.
 * @journal: The journal to be reaped.
 */
static void finish_reaping(struct slab_journal *journal)
{
	journal->head = journal->unreapable;
	add_entries(journal);
	vdo_check_if_slab_drained(journal->slab);
}

static void reap_slab_journal(struct slab_journal *journal);

/**
 * complete_reaping() - Finish reaping now that we have flushed the lower
 *                      layer and then try reaping again in case we deferred
 *                      reaping due to an outstanding vio.
 * @completion: The flush vio.
 */
static void complete_reaping(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;

	vdo_return_block_allocator_vio(journal->slab->allocator, entry);
	finish_reaping(journal);
	reap_slab_journal(journal);
}

/**
 * handle_flush_error() - Handle an error flushing the lower layer.
 * @completion: The flush vio.
 */
static void handle_flush_error(struct vdo_completion *completion)
{
	struct slab_journal *journal =
		((struct vio_pool_entry *)completion->parent)->parent;

	record_metadata_io_error(as_vio(completion));
	enter_journal_read_only_mode(journal, completion->result);
	complete_reaping(completion);
}

static void flush_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct slab_journal *journal = entry->parent;

	continue_vio_after_io(vio,
			      complete_reaping,
			      journal->slab->allocator->thread_id);
}

/**
 * flush_for_reaping() - A waiter callback for getting a vio with which to
 *                       flush the lower layer prior to reaping.
 * @waiter: The journal as a flush waiter.
 * @vio_context: The newly acquired flush vio.
 */
static void flush_for_reaping(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, flush_waiter);
	struct vio_pool_entry *entry = vio_context;
	struct vio *vio = entry->vio;

	entry->parent = journal;
	submit_flush_vio(vio, flush_endio, handle_flush_error);
}

/**
 * reap_slab_journal() - Conduct a reap on a slab journal to reclaim
 *                       unreferenced blocks.
 * @journal: The slab journal.
 */
static void reap_slab_journal(struct slab_journal *journal)
{
	bool reaped = false;
	int result;

	if (is_reaping(journal)) {
		/* We already have a reap in progress so wait for it to finish. */
		return;
	}

	if (vdo_is_unrecovered_slab(journal->slab) ||
	    !vdo_is_state_normal(&journal->slab->state) ||
	    is_vdo_read_only(journal)) {
		/*
		 * We must not reap in the first two cases, and there's no
		 * point in read-only mode.
		 */
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
	result = vdo_acquire_block_allocator_vio(journal->slab->allocator,
						 &journal->flush_waiter);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		return;
	}
}

/**
 * release_journal_locks() - Callback invoked after a slab summary update
 *                           completes.
 * @waiter: The slab summary waiter that has just been notified.
 * @context: The result code of the update.
 *
 * Registered in the constructor on behalf of update_tail_block_location().
 *
 * Implements waiter_callback.
 */
static void release_journal_locks(struct waiter *waiter, void *context)
{
	sequence_number_t first, i;
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, slab_summary_waiter);
	int result = *((int *)context);

	if (result != VDO_SUCCESS) {
		if (result != VDO_READ_ONLY) {
			/*
			 * Don't bother logging what might be lots of errors if
			 * we are already in read-only mode.
			 */
			uds_log_error_strerror(result,
					       "failed slab summary update %llu",
					       (unsigned long long) journal->summarized);
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
		/*
		 * Release the lock the summarized block held on the recovery
		 * journal. (During replay, recovery_start will always be 0.)
		 */
		if (journal->recovery_journal != NULL) {
			zone_count_t zone_number =
				journal->slab->allocator->zone_number;
			vdo_release_recovery_journal_block_reference(journal->recovery_journal,
								     get_lock(journal, i)->recovery_start,
								     VDO_ZONE_TYPE_PHYSICAL,
								     zone_number);
		}

		/*
		 * Release our own lock against reaping for blocks that are
		 * committed. (This function will not change locks during
		 * replay.)
		 */
		vdo_adjust_slab_journal_block_reference(journal, i, -1);
	}

	journal->updating_slab_summary = false;

	reap_slab_journal(journal);

	/* Check if the slab summary needs to be updated again. */
	update_tail_block_location(journal);
}

/**
 * update_tail_block_location() - Update the tail block location in the slab
 *                                summary, if necessary.
 * @journal: The slab journal that is updating its tail block location.
 */
static void update_tail_block_location(struct slab_journal *journal)
{
	block_count_t free_block_count;
	tail_block_offset_t block_offset;

	if (journal->updating_slab_summary || is_vdo_read_only(journal) ||
	    (journal->last_summarized >= journal->next_commit)) {
		vdo_check_if_slab_drained(journal->slab);
		return;
	}

	if (vdo_is_unrecovered_slab(journal->slab)) {
		free_block_count =
			vdo_get_summarized_free_block_count(journal->summary,
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
		vdo_get_slab_journal_block_offset(journal, journal->summarized);
	vdo_update_slab_summary_entry(journal->summary,
				      &journal->slab_summary_waiter,
				      journal->slab->slab_number,
				      block_offset,
				      (journal->head > 1),
				      false,
				      free_block_count);
}

/**
 * vdo_reopen_slab_journal() - Reopen a slab journal by emptying it and then
 *                             adding any pending entries.
 * @journal: The journal to reopen.
 */
void vdo_reopen_slab_journal(struct slab_journal *journal)
{
	sequence_number_t block;

	ASSERT_LOG_ONLY(journal->tail_header.entry_count == 0,
			"vdo_slab journal's active block empty before reopening");
	journal->head = journal->tail;
	initialize_journal_state(journal);

	/* Ensure no locks are spuriously held on an empty journal. */
	for (block = 1; block <= journal->size; block++) {
		ASSERT_LOG_ONLY((get_lock(journal, block)->count == 0),
				"Scrubbed journal's block %llu is not locked",
				(unsigned long long) block);
	}

	add_entries(journal);
}

static sequence_number_t
get_committing_sequence_number(const struct vio_pool_entry *entry)
{
	const struct packed_slab_journal_block *block = entry->buffer;

	return __le64_to_cpu(block->header.sequence_number);
}

/**
 * complete_write() - Handle post-commit processing.
 * @completion: The write vio as a completion.
 *
 * This is the callback registered by write_slab_journal_block().
 */
static void complete_write(struct vdo_completion *completion)
{
	int write_result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;
	sequence_number_t committed = get_committing_sequence_number(entry);

	list_del_init(&entry->available_entry);
	vdo_return_block_allocator_vio(journal->slab->allocator, entry);

	if (write_result != VDO_SUCCESS) {
		record_metadata_io_error(as_vio(completion));
		uds_log_error_strerror(write_result,
				       "cannot write slab journal block %llu",
				       (unsigned long long) committed);
		enter_journal_read_only_mode(journal, write_result);
		return;
	}

	WRITE_ONCE(journal->events->blocks_written,
		   journal->events->blocks_written + 1);

	if (list_empty(&journal->uncommitted_blocks)) {
		/*
		 * If no blocks are outstanding, then the commit point is at
		 * the tail.
		 */
		journal->next_commit = journal->tail;
	} else {
		/*
		 * The commit point is always the beginning of the oldest
		 * incomplete block.
		 */
		struct vio_pool_entry *oldest =
			as_vio_pool_entry(journal->uncommitted_blocks.next);
		journal->next_commit = get_committing_sequence_number(oldest);
	}

	update_tail_block_location(journal);
}

static void write_slab_journal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct slab_journal *journal = entry->parent;

	continue_vio_after_io(vio,
			      complete_write,
			      journal->slab->allocator->thread_id);
}

/**
 * write_slab_journal_block() - Write a slab journal block.
 * @waiter: The vio pool waiter which was just notified.
 * @vio_context: The vio pool entry for the write.
 *
 * Callback from vdo_acquire_block_allocator_vio() registered in
 * commit_tail().
 */
static void write_slab_journal_block(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, resource_waiter);
	struct vio_pool_entry *entry = vio_context;
	struct slab_journal_block_header *header = &journal->tail_header;
	int unused_entries = journal->entries_per_block - header->entry_count;
	physical_block_number_t block_number;
	const struct admin_state_code *operation;

	header->head = journal->head;
	list_move_tail(&entry->available_entry, &journal->uncommitted_blocks);
	vdo_pack_slab_journal_block_header(header, &journal->block->header);

	/* Copy the tail block into the vio. */
	memcpy(entry->buffer, journal->block, VDO_BLOCK_SIZE);

	ASSERT_LOG_ONLY(unused_entries >= 0,
			"vdo_slab journal block is not overfull");
	if (unused_entries > 0) {
		/*
		 * Release the per-entry locks for any unused entries in the
		 * block we are about to write.
		 */
		vdo_adjust_slab_journal_block_reference(journal,
							header->sequence_number,
							-unused_entries);
		journal->partial_write_in_progress = !block_is_full(journal);
	}

	block_number = get_block_number(journal, header->sequence_number);
	entry->parent = journal;

	/*
	 * This block won't be read in recovery until the slab summary is
	 * updated to refer to it. The slab summary update does a flush which
	 * is sufficient to protect us from VDO-2331.
	 */
	submit_metadata_vio(entry->vio,
			    block_number,
			    write_slab_journal_endio,
			    complete_write,
			    REQ_OP_WRITE);

	/*
         * Since the write is submitted, the tail block structure can be
         * reused.
	 */
	journal->tail++;
	initialize_tail_block(journal);
	journal->waiting_to_commit = false;

	operation = vdo_get_admin_state_code(&journal->slab->state);
	if (operation == VDO_ADMIN_STATE_WAITING_FOR_RECOVERY) {
		vdo_finish_operation(&journal->slab->state,
				     (is_vdo_read_only(journal) ?
				      VDO_READ_ONLY : VDO_SUCCESS));
		return;
	}

	add_entries(journal);
}

/**
 * commit_tail() - Commit the tail block of the slab journal.
 * @journal: The journal whose tail block should be committed.
 */
static void commit_tail(struct slab_journal *journal)
{
	int result;

	if ((journal->tail_header.entry_count == 0) &&
	    must_make_entries_to_flush(journal)) {
		/*
		 * There are no entries at the moment, but there are some
		 * waiters, so defer initiating the flush until those entries
		 * are ready to write.
		 */
		return;
	}

	if (is_vdo_read_only(journal) || journal->waiting_to_commit ||
	    (journal->tail_header.entry_count == 0)) {
		/*
		 * There is nothing to do since the tail block is empty, or
		 * writing, or the journal is in read-only mode.
		 */
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
	result = vdo_acquire_block_allocator_vio(journal->slab->allocator,
						 &journal->resource_waiter);
	if (result != VDO_SUCCESS) {
		journal->waiting_to_commit = false;
		enter_journal_read_only_mode(journal, result);
		return;
	}
}

/**
 * vdo_encode_slab_journal_entry() - Encode a slab journal entry.
 * @tail_header: The unpacked header for the block.
 * @payload: The journal block payload to hold the entry.
 * @sbn: The slab block number of the entry to encode.
 * @operation: The type of the entry.
 *
 * Exposed for unit tests.
 */
static void
vdo_encode_slab_journal_entry(struct slab_journal_block_header *tail_header,
			      slab_journal_payload *payload,
			      slab_block_number sbn,
			      enum journal_operation operation)
{
	journal_entry_count_t entry_number = tail_header->entry_count++;

	if (operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) {
		if (!tail_header->has_block_map_increments) {
			memset(payload->full_entries.entry_types,
			       0,
			       VDO_SLAB_JOURNAL_ENTRY_TYPES_SIZE);
			tail_header->has_block_map_increments = true;
		}

		payload->full_entries.entry_types[entry_number / 8] |=
			((byte)1 << (entry_number % 8));
	}

	vdo_pack_slab_journal_entry(&payload->entries[entry_number],
				sbn,
				vdo_is_journal_increment_operation(operation));
}

/**
 * add_entry() - Actually add an entry to the slab journal, potentially firing
 *               off a write if a block becomes full.
 * @journal: The slab journal to append to.
 * @pbn: The pbn being adjusted.
 * @operation: The type of entry to make.
 * @recovery_point: The recovery journal point for this entry.
 *
 * This function is synchronous.
 */
static void add_entry(struct slab_journal *journal,
		      physical_block_number_t pbn,
		      enum journal_operation operation,
		      const struct journal_point *recovery_point)
{
	struct packed_slab_journal_block *block = journal->block;

	int result =
		ASSERT(vdo_before_journal_point(&journal->tail_header.recovery_point,
						recovery_point),
		       "recovery journal point is monotonically increasing, recovery point: %llu.%u, block recovery point: %llu.%u",
		       (unsigned long long) recovery_point->sequence_number,
		       recovery_point->entry_count,
		       (unsigned long long) journal->tail_header.recovery_point.sequence_number,
		       journal->tail_header.recovery_point.entry_count);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		return;
	}

	if (operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) {
		result = ASSERT_LOG_ONLY((journal->tail_header.entry_count <
					  journal->full_entries_per_block),
					 "block has room for full entries");
		if (result != VDO_SUCCESS) {
			enter_journal_read_only_mode(journal, result);
			return;
		}
	}

	vdo_encode_slab_journal_entry(&journal->tail_header,
				  &block->payload,
				  pbn - journal->slab->start,
				  operation);
	journal->tail_header.recovery_point = *recovery_point;
	if (block_is_full(journal)) {
		commit_tail(journal);
	}
}

/**
 * vdo_attempt_replay_into_slab_journal() - Attempt to replay a recovery
 *                                          journal entry into a slab journal.
 * @journal: The slab journal to use.
 * @pbn: The PBN for the entry.
 * @operation: The type of entry to add.
 * @recovery_point: The recovery journal point corresponding to this entry.
 * @parent: The completion to notify when there is space to add the entry if
 *          the entry could not be added immediately.
 *
 * Return: true if the entry was added immediately.
 */
bool vdo_attempt_replay_into_slab_journal(struct slab_journal *journal,
					  physical_block_number_t pbn,
					  enum journal_operation operation,
					  struct journal_point *recovery_point,
					  struct vdo_completion *parent)
{
	struct slab_journal_block_header *header = &journal->tail_header;

	/* Only accept entries after the current recovery point. */
	if (!vdo_before_journal_point(&journal->tail_header.recovery_point,
				      recovery_point)) {
		return true;
	}

	if ((header->entry_count >= journal->full_entries_per_block) &&
	    (header->has_block_map_increments ||
	     (operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT))) {
		/*
		 * The tail block does not have room for the entry we are
		 * attempting to add so commit the tail block now.
		 */
		commit_tail(journal);
	}

	if (journal->waiting_to_commit) {
		vdo_start_operation_with_waiter(&journal->slab->state,
						VDO_ADMIN_STATE_WAITING_FOR_RECOVERY,
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

	vdo_mark_slab_replaying(journal->slab);
	add_entry(journal, pbn, operation, recovery_point);
	return true;
}

/**
 * requires_flushing() - Check whether the journal should be saving reference
 *                       blocks out.
 * @journal: The journal to check.
 *
 * Return: true if the journal should be requesting reference block writes.
 */
static bool requires_flushing(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);

	return (journal_length >= journal->flushing_threshold);
}

/**
 * requires_reaping() - Check whether the journal must be reaped before adding
 *                      new entries.
 * @journal: The journal to check.
 *
 * Return: true if the journal must be reaped.
 */
static bool requires_reaping(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);

	return (journal_length >= journal->blocking_threshold);
}

/**
 * vdo_slab_journal_requires_scrubbing() - Check to see if the journal should
 *                                         be scrubbed.
 * @journal: The slab journal.
 *
 * Return: true if the journal requires scrubbing.
 */
bool vdo_slab_journal_requires_scrubbing(const struct slab_journal *journal)
{
	block_count_t journal_length = (journal->tail - journal->head);

	return (journal_length >= journal->scrubbing_threshold);
}

/**
 * add_entry_from_waiter() - Add an entry to the slab journal.
 * @waiter: The vio which should make an entry now.
 * @context: The slab journal to make an entry in.
 *
 * This callback is invoked by add_entries() once it has determined that we
 * are ready to make another entry in the slab journal. Implements
 * waiter_callback.
 */
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
			vdo_acquire_recovery_journal_block_reference(journal->recovery_journal,
								     recovery_block,
								     VDO_ZONE_TYPE_PHYSICAL,
								     zone_number);
		}
		mark_slab_journal_dirty(journal, recovery_block);

		/*
		 * If the slab journal is over the first threshold, tell the
		 * ref_counts to write some reference blocks soon.
		 */
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
			vdo_save_several_reference_blocks(journal->slab->reference_counts,
							  blocks_to_deadline + 1);
		}
	}

	add_entry(journal,
		  data_vio->operation.pbn,
		  data_vio->operation.type,
		  &data_vio->recovery_journal_point);

	/*
	 * Now that an entry has been made in the slab journal, update the
	 * reference counts.
	 */
	result = vdo_modify_slab_reference_count(journal->slab,
						 &slab_journal_point,
						 data_vio->operation);
	continue_data_vio(data_vio, result);
}

/**
 * is_next_entry_a_block_map_increment() - Check whether the next entry to be
 *                                         made is a block map increment.
 * @journal: The journal.
 *
 * Return: true if the first entry waiter's operation is a block map increment.
 */
static inline bool
is_next_entry_a_block_map_increment(struct slab_journal *journal)
{
	struct data_vio *data_vio =
		waiter_as_data_vio(get_first_waiter(&journal->entry_waiters));
	return (data_vio->operation.type == VDO_JOURNAL_BLOCK_MAP_INCREMENT);
}

/**
 * add_entries() - Add as many entries as possible from the queue of vios
 *                 waiting to make entries.
 * @journal: The journal to which entries may be added.
 *
 * By processing the queue in order, we ensure that slab journal entries are
 * made in the same order as recovery journal entries for the same increment
 * or decrement.
 */
static void add_entries(struct slab_journal *journal)
{
	if (journal->adding_entries) {
		/* Protect against re-entrancy. */
		return;
	}

	journal->adding_entries = true;
	while (has_waiters(&journal->entry_waiters)) {
		struct slab_journal_block_header *header = &journal->tail_header;

		if (journal->partial_write_in_progress ||
		    vdo_is_slab_rebuilding(journal->slab)) {
			/*
			 * Don't add entries while rebuilding or while a
			 * partial write is outstanding (VDO-2399).
			 */
			break;
		}

		if (journal->waiting_to_commit) {
			/*
			 * If we are waiting for resources to write the tail
			 * block, and the tail block is full, we can't make
			 * another entry.
			 */
			WRITE_ONCE(journal->events->tail_busy_count,
				   journal->events->tail_busy_count + 1);
			break;
		} else if (is_next_entry_a_block_map_increment(journal) &&
			   (header->entry_count >=
			    journal->full_entries_per_block)) {
			/*
			 * The tail block does not have room for a block map
			 * increment, so commit it now.
			 */
			commit_tail(journal);
			if (journal->waiting_to_commit) {
				WRITE_ONCE(journal->events->tail_busy_count,
					   journal->events->tail_busy_count
					   + 1);
				break;
			}
		}

		/*
		 * If the slab is over the blocking threshold, make the vio
		 * wait.
		 */
		if (requires_reaping(journal)) {
			WRITE_ONCE(journal->events->blocked_count,
				   journal->events->blocked_count + 1);
			vdo_save_dirty_reference_blocks(journal->slab->reference_counts);
			break;
		}

		if (header->entry_count == 0) {
			struct journal_lock *lock =
				get_lock(journal, header->sequence_number);
			/*
			 * Check if the on disk slab journal is full. Because
			 * of the blocking and scrubbing thresholds, this
			 * should never happen.
			 */
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
				vdo_save_dirty_reference_blocks(journal->slab->reference_counts);
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
				vdo_acquire_dirty_block_locks(journal->slab->reference_counts);
			}
		}

		notify_next_waiter(&journal->entry_waiters,
				   add_entry_from_waiter,
				   journal);
	}

	journal->adding_entries = false;

	/*
	 * If there are no waiters, and we are flushing or saving, commit the
	 * tail block.
	 */
	if (vdo_is_slab_draining(journal->slab) &&
	    !vdo_is_state_suspending(&journal->slab->state) &&
	    !has_waiters(&journal->entry_waiters)) {
		commit_tail(journal);
	}
}

/**
 * vdo_add_slab_journal_entry() - Add an entry to a slab journal.
 * @journal: The slab journal to use.
 * @data_vio: The data_vio for which to add the entry.
 */
void vdo_add_slab_journal_entry(struct slab_journal *journal,
				struct data_vio *data_vio)
{
	struct vdo_slab *slab = journal->slab;
	int result;

	if (!vdo_is_slab_open(slab)) {
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

	if (vdo_is_unrecovered_slab(slab) && requires_reaping(journal)) {
		struct slab_scrubber *scrubber
			= slab->allocator->slab_scrubber;
		vdo_register_slab_for_scrubbing(scrubber, slab, true);
	}

	add_entries(journal);
}

/**
 * vdo_adjust_slab_journal_block_reference() - Adjust the reference count for
 *                                             a slab journal block.
 * @journal: The slab journal.
 * @sequence_number: The journal sequence number of the referenced block.
 * @adjustment: Amount to adjust the reference counter.
 *
 * Note that when the adjustment is negative, the slab journal will be reaped.
 */
void vdo_adjust_slab_journal_block_reference(struct slab_journal *journal,
					     sequence_number_t sequence_number,
					     int adjustment)
{
	struct journal_lock *lock;

	if (sequence_number == 0) {
		return;
	}

	if (vdo_is_replaying_slab(journal->slab)) {
		/* Locks should not be used during offline replay. */
		return;
	}

	ASSERT_LOG_ONLY((adjustment != 0), "adjustment must be non-zero");
	lock = get_lock(journal, sequence_number);
	if (adjustment < 0) {
		ASSERT_LOG_ONLY((-adjustment <= lock->count),
				"adjustment %d of lock count %u for slab journal block %llu must not underflow",
				adjustment, lock->count,
				(unsigned long long) sequence_number);
	}

	lock->count += adjustment;
	if (lock->count == 0) {
		reap_slab_journal(journal);
	}
}

/**
 * vdo_release_recovery_journal_lock() - Request the slab journal to release
 *                                       the recovery journal lock it may hold
 *                                       on a specified recovery journal
 *                                       block.
 * @journal: The slab journal.
 * @recovery_lock: The sequence number of the recovery journal block whose
 *                 locks should be released.
 *
 * Return: true if the journal does hold a lock on the specified
 *         block (which it will release).
 */
bool vdo_release_recovery_journal_lock(struct slab_journal *journal,
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

	/* All locks are held by the block which is in progress; write it. */
	commit_tail(journal);
	return true;
}

/**
 * vdo_resume_slab_journal() - Reset slab journal state, if necessary, for
 *                             a suspend-resume cycle.
 * @journal: The journal to reset
 *
 * After a successful save, any info about locks, journal blocks
 * partially filled, etc., is out of date and should be reset.
 **/
void vdo_resume_slab_journal(struct slab_journal *journal)
{
	struct vdo *vdo = journal->slab->allocator->depot->vdo;
	if ((vdo->suspend_type == VDO_ADMIN_STATE_SAVING) &&
	    !is_vdo_read_only(journal)) {
		vdo_reopen_slab_journal(journal);
	}
}

/**
 * vdo_drain_slab_journal() - Drain slab journal I/O.
 * @journal: The journal to drain.
 *
 * Depending upon the type of drain (as recorded in the journal's slab), any
 * dirty journal blocks may be written out.
 */
void vdo_drain_slab_journal(struct slab_journal *journal)
{
	const struct admin_state_code *code
		= vdo_get_admin_state_code(&journal->slab->state);

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"vdo_drain_slab_journal() called on correct thread");
	if (code->quiescing) {
		/*
		 * XXX: we should revisit this assertion since it is no longer
		 * clear what it is for.
		 */
		ASSERT_LOG_ONLY((!(vdo_is_slab_rebuilding(journal->slab) &&
				   has_waiters(&journal->entry_waiters))),
				"slab is recovered or has no waiters");
	}

	if ((code == VDO_ADMIN_STATE_REBUILDING)
	    || (code == VDO_ADMIN_STATE_SUSPENDING)
	    || (code == VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING)) {
		return;
	}

	commit_tail(journal);
}

/**
 * finish_decoding_journal() - Finish the decode process by returning the vio
 *                             and notifying the slab that we're done.
 * @completion: The vio as a completion.
 */
static void finish_decoding_journal(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;

	vdo_return_block_allocator_vio(journal->slab->allocator, entry);
	vdo_notify_slab_journal_is_loaded(journal->slab, result);
}

/**
 * set_decoded_state() - Set up the in-memory journal state to the state which
 *                       was written to disk.
 * @completion: The vio which was used to read the journal tail.
 * 
 * This is the callback registered in read_slab_journal_tail().
 */
static void set_decoded_state(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct slab_journal *journal = entry->parent;
	struct packed_slab_journal_block *block = entry->buffer;

	struct slab_journal_block_header header;

	vdo_unpack_slab_journal_block_header(&block->header, &header);

	if ((header.metadata_type != VDO_METADATA_SLAB_JOURNAL) ||
	    (header.nonce != journal->slab->allocator->nonce)) {
		finish_decoding_journal(completion);
		return;
	}

	journal->tail = header.sequence_number + 1;

	/*
	 * If the slab is clean, this implies the slab journal is empty, so
	 * advance the head appropriately.
	 */
	if (vdo_get_summarized_cleanliness(journal->summary,
					   journal->slab->slab_number)) {
		journal->head = journal->tail;
	} else {
		journal->head = header.head;
	}

	journal->tail_header = header;
	initialize_journal_state(journal);
	finish_decoding_journal(completion);
}

static void read_slab_journal_tail_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct slab_journal *journal = entry->parent;

	continue_vio_after_io(vio,
			      set_decoded_state,
			      journal->slab->allocator->thread_id);
}

static void handle_decode_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	finish_decoding_journal(completion);
}

/**
 * read_slab_journal_tail() - Read the slab journal tail block by using a vio
 *                            acquired from the vio pool.
 * @waiter: The vio pool waiter which has just been notified.
 * @vio_context: The vio pool entry given to the waiter.
 *
 * This is the success callback from vdo_acquire_block_allocator_vio() when
 * decoding the slab journal.
 */
static void read_slab_journal_tail(struct waiter *waiter, void *vio_context)
{
	struct slab_journal *journal =
		container_of(waiter, struct slab_journal, resource_waiter);
	struct vdo_slab *slab = journal->slab;
	struct vio_pool_entry *entry = vio_context;
	tail_block_offset_t last_commit_point =
		vdo_get_summarized_tail_block_offset(journal->summary,
						     slab->slab_number);
	/*
	 * Slab summary keeps the commit point offset, so the tail block is
	 * the block before that. Calculation supports small journals in unit
	 * tests.
	 */
	tail_block_offset_t tail_block =
		((last_commit_point == 0) ? (tail_block_offset_t)(journal->size - 1) :
		 (last_commit_point - 1));

	entry->parent = journal;

	entry->vio->completion.callback_thread_id = slab->allocator->thread_id;
	submit_metadata_vio(entry->vio,
			    slab->journal_origin + tail_block,
			    read_slab_journal_tail_endio,
			    handle_decode_error,
			    REQ_OP_READ);
}

/**
 * vdo_decode_slab_journal() - Decode the slab journal by reading its tail.
 * @journal: The journal to decode.
 */
void vdo_decode_slab_journal(struct slab_journal *journal)
{
	struct vdo_slab *slab = journal->slab;
	tail_block_offset_t last_commit_point;
	int result;

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 journal->slab->allocator->thread_id),
			"vdo_decode_slab_journal() called on correct thread");
	last_commit_point =
		vdo_get_summarized_tail_block_offset(journal->summary,
						     slab->slab_number);
	if ((last_commit_point == 0) &&
	    !vdo_must_load_ref_counts(journal->summary, slab->slab_number)) {
		/*
		 * This slab claims that it has a tail block at (journal->size
		 * - 1), but a head of 1. This is impossible, due to the
		 * scrubbing threshold, on a real system, so don't bother
		 * reading the (bogus) data off disk.
		 */
		ASSERT_LOG_ONLY(((journal->size < 16) ||
				 (journal->scrubbing_threshold < (journal->size - 1))),
				"Scrubbing threshold protects against reads of unwritten slab journal blocks");
		vdo_notify_slab_journal_is_loaded(slab, VDO_SUCCESS);
		return;
	}

	journal->resource_waiter.callback = read_slab_journal_tail;
	result = vdo_acquire_block_allocator_vio(slab->allocator,
						 &journal->resource_waiter);
	if (result != VDO_SUCCESS) {
		vdo_notify_slab_journal_is_loaded(slab, result);
	}
}

/**
 * vdo_dump_slab_journal() - Dump the slab journal.
 * @journal: The slab journal to dump.
 */
void vdo_dump_slab_journal(const struct slab_journal *journal)
{
	uds_log_info("  slab journal: entry_waiters=%zu waiting_to_commit=%s updating_slab_summary=%s head=%llu unreapable=%llu tail=%llu next_commit=%llu summarized=%llu last_summarized=%llu recovery_lock=%llu dirty=%s",
		     count_waiters(&journal->entry_waiters),
		     uds_bool_to_string(journal->waiting_to_commit),
		     uds_bool_to_string(journal->updating_slab_summary),
		     (unsigned long long) journal->head,
		     (unsigned long long) journal->unreapable,
		     (unsigned long long) journal->tail,
		     (unsigned long long) journal->next_commit,
		     (unsigned long long) journal->summarized,
		     (unsigned long long) journal->last_summarized,
		     (unsigned long long) journal->recovery_lock,
		     uds_bool_to_string(vdo_is_slab_journal_dirty(journal)));
	/*
	 * Given the frequency with which the locks are just a tiny bit off, it
	 * might be worth dumping all the locks, but that might be too much
	 * logging.
	 */
}
