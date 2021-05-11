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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournal.c#106 $
 */

#include "recoveryJournal.h"
#include "recoveryJournalInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "constants.h"
#include "dataVIO.h"
#include "extent.h"
#include "header.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalBlock.h"
#include "recoveryJournalFormat.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "vdo.h"
#include "vdoInternal.h"
#include "waitQueue.h"

static const uint64_t RECOVERY_COUNT_MASK = 0xff;

enum {
	/*
	 * The number of reserved blocks must be large enough to prevent a
	 * new recovery journal block write from overwriting a block which
	 * appears to still be a valid head block of the journal. Currently,
	 * that means reserving enough space for all 2048 VIOs, or 8 blocks.
	 */
	RECOVERY_JOURNAL_RESERVED_BLOCKS = 8,
};

/**
 * Get a block from the end of the free list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static struct recovery_journal_block *
pop_free_list(struct recovery_journal *journal)
{
	struct list_head *entry;
	if (list_empty(&journal->free_tail_blocks)) {
		return NULL;
	}
	entry = journal->free_tail_blocks.prev;
	list_del_init(entry);
	return vdo_recovery_block_from_list_entry(entry);
}

/**
 * Get a block from the end of the active list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static struct recovery_journal_block *
pop_active_list(struct recovery_journal *journal)
{
	struct list_head *entry;
	if (list_empty(&journal->active_tail_blocks)) {
		return NULL;
	}
	entry = journal->active_tail_blocks.prev;
	list_del_init(entry);
	return vdo_recovery_block_from_list_entry(entry);
}

/**
 * Assert that we are running on the journal thread.
 *
 * @param journal        The journal
 * @param function_name  The function doing the check (for logging)
 **/
static void assert_on_journal_thread(struct recovery_journal *journal,
				     const char *function_name)
{
	ASSERT_LOG_ONLY((get_callback_thread_id() == journal->thread_id),
			"%s() called on journal thread", function_name);
}

/**
 * waiter_callback implementation invoked whenever a data_vio is to be released
 * from the journal, either because its entry was committed to disk,
 * or because there was an error.
 **/
static void continue_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	int wait_result = *((int *)context);

	continue_data_vio(data_vio, wait_result);
}

/**
 * Check whether the journal has any waiters on any blocks.
 *
 * @param journal  The journal in question
 *
 * @return <code>true</code> if any block has a waiter
 **/
static inline bool has_block_waiters(struct recovery_journal *journal)
{
	struct recovery_journal_block *block;

	// Either the first active tail block (if it exists) has waiters,
	// or no active tail block has waiters.
	if (list_empty(&journal->active_tail_blocks)) {
		return false;
	}

	block = vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);
	return (has_waiters(&block->entry_waiters)
		|| has_waiters(&block->commit_waiters));
}

/**********************************************************************/
static void recycle_journal_blocks(struct recovery_journal *journal);
static void recycle_journal_block(struct recovery_journal_block *block);
static void notify_commit_waiters(struct recovery_journal *journal);

/**
 * Check whether the journal has drained.
 *
 * @param journal The journal which may have just drained
 **/
static void check_for_drain_complete(struct recovery_journal *journal)
{
	int result = VDO_SUCCESS;
	if (vdo_is_read_only(journal->read_only_notifier)) {
		result = VDO_READ_ONLY;
		/*
		 * Clean up any full active blocks which were not written due
		 * to being in read-only mode.
		 *
		 * XXX: This would probably be better as a short-circuit in
		 * write_block().
		 */
		notify_commit_waiters(journal);
		recycle_journal_blocks(journal);

		// Release any data_vios waiting to be assigned entries.
		notify_all_waiters(&journal->decrement_waiters,
				   continue_waiter, &result);
		notify_all_waiters(&journal->increment_waiters,
				   continue_waiter, &result);
	}

	if (!is_vdo_state_draining(&journal->state) || journal->reaping
	    || has_block_waiters(journal)
	    || has_waiters(&journal->increment_waiters)
	    || has_waiters(&journal->decrement_waiters)
	    || !suspend_vdo_lock_counter(journal->lock_counter)) {
		return;
	}

	if (is_vdo_state_saving(&journal->state)) {
		if (journal->active_block != NULL) {
			ASSERT_LOG_ONLY(((result == VDO_READ_ONLY)
					 || !is_vdo_recovery_block_dirty(journal->active_block)),
					"journal being saved has clean active block");
			recycle_journal_block(journal->active_block);
		}

		ASSERT_LOG_ONLY(list_empty(&journal->active_tail_blocks),
				"all blocks in a journal being saved must be inactive");
	}

	finish_vdo_draining_with_result(&journal->state, result);
}

/**
 * Notifiy a recovery journal that the VDO has gone read-only.
 *
 * <p>Implements vdo_read_only_notification.
 *
 * @param listener  The journal
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void
notify_recovery_journal_of_read_only_mode(void *listener,
					  struct vdo_completion *parent)
{
	check_for_drain_complete(listener);
	complete_vdo_completion(parent);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All VIOs waiting for commits will be
 * awakened with an error.
 *
 * @param journal     The journal which has failed
 * @param error_code  The error result triggering this call
 **/
static void enter_journal_read_only_mode(struct recovery_journal *journal,
					 int error_code)
{
	vdo_enter_read_only_mode(journal->read_only_notifier, error_code);
	check_for_drain_complete(journal);
}

/**********************************************************************/
sequence_number_t
get_current_journal_sequence_number(struct recovery_journal *journal)
{
	return journal->tail;
}

/**
 * Get the head of the recovery journal, which is the lowest sequence number of
 * the block map head and the slab journal head.
 *
 * @param journal    The journal
 *
 * @return the head of the journal
 **/
static inline sequence_number_t
get_recovery_journal_head(const struct recovery_journal *journal)
{
	return min(journal->block_map_head, journal->slab_journal_head);
}

/**
 * Compute the recovery count byte for a given recovery count.
 *
 * @param recovery_count  The recovery count
 *
 * @return The byte corresponding to the recovery count
 **/
static inline uint8_t __must_check
compute_recovery_count_byte(uint64_t recovery_count)
{
	return (uint8_t)(recovery_count & RECOVERY_COUNT_MASK);
}

/**
 * Check whether the journal is over the threshold, and if so, force the oldest
 * slab journal tail block to commit.
 *
 * @param journal    The journal
 **/
static void
check_slab_journal_commit_threshold(struct recovery_journal *journal)
{
	block_count_t current_length = journal->tail -
		journal->slab_journal_head;
	if (current_length > journal->slab_journal_commit_threshold) {
		journal->events.slab_journal_commits_requested++;
		vdo_commit_oldest_slab_journal_tail_blocks(journal->depot,
							   journal->slab_journal_head);
	}
}

/**********************************************************************/
static void reap_recovery_journal(struct recovery_journal *journal);
static void assign_entries(struct recovery_journal *journal);

/**
 * Finish reaping the journal.
 *
 * @param journal The journal being reaped
 **/
static void finish_reaping(struct recovery_journal *journal)
{
	block_count_t blocks_reaped;
	sequence_number_t old_head = get_recovery_journal_head(journal);
	journal->block_map_head = journal->block_map_reap_head;
	journal->slab_journal_head = journal->slab_journal_reap_head;
	blocks_reaped = get_recovery_journal_head(journal) - old_head;
	journal->available_space += blocks_reaped * journal->entries_per_block;
	journal->reaping = false;
	check_slab_journal_commit_threshold(journal);
	assign_entries(journal);
	check_for_drain_complete(journal);
}

/**
 * Finish reaping the journal after flushing the lower layer. This is the
 * callback registered in reap_recovery_journal().
 *
 * @param completion  The journal's flush VIO
 **/
static void complete_reaping(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;
	finish_reaping(journal);

	// Try reaping again in case more locks were released while flush was
	// out.
	reap_recovery_journal(journal);
}

/**
 * Handle an error when flushing the lower layer due to reaping.
 *
 * @param completion  The journal's flush VIO
 **/
static void handle_flush_error(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;
	journal->reaping = false;
	enter_journal_read_only_mode(journal, completion->result);
}

/**
 * Set all journal fields appropriately to start journaling from the current
 * active block.
 *
 * @param journal  The journal to be reset based on its active block
 **/
static void initialize_journal_state(struct recovery_journal *journal)
{
	journal->append_point.sequence_number = journal->tail;
	journal->last_write_acknowledged = journal->tail;
	journal->block_map_head = journal->tail;
	journal->slab_journal_head = journal->tail;
	journal->block_map_reap_head = journal->tail;
	journal->slab_journal_reap_head = journal->tail;
	journal->block_map_head_block_number =
		get_recovery_journal_block_number(journal,
						  journal->block_map_head);
	journal->slab_journal_head_block_number =
		get_recovery_journal_block_number(journal,
						  journal->slab_journal_head);
}

/**********************************************************************/
block_count_t get_recovery_journal_length(block_count_t journal_size)
{
	block_count_t reserved_blocks = journal_size / 4;
	if (reserved_blocks > RECOVERY_JOURNAL_RESERVED_BLOCKS) {
		reserved_blocks = RECOVERY_JOURNAL_RESERVED_BLOCKS;
	}
	return (journal_size - reserved_blocks);
}

/**
 * Attempt to reap the journal now that all the locks on some journal block
 * have been released. This is the callback registered with the lock counter.
 *
 * @param completion  The lock counter completion
 **/
static void reap_recovery_journal_callback(struct vdo_completion *completion)
{
	struct recovery_journal *journal =
		(struct recovery_journal *)completion->parent;
	/*
	 * The acknowledgement must be done before reaping so that there is no
	 * race between acknowledging the notification and unlocks wishing to
	 * notify.
	 */
	acknowledge_vdo_lock_unlock(journal->lock_counter);

	if (is_vdo_state_quiescing(&journal->state)) {
		/*
		 * Don't start reaping when the journal is trying to quiesce.
		 * Do check if this notification is the last thing the is
		 * waiting on.
		 */
		check_for_drain_complete(journal);
		return;
	}

	reap_recovery_journal(journal);
	check_slab_journal_commit_threshold(journal);
}

/**********************************************************************
 * Set the journal's tail sequence number.
 *
 * @param journal The journal whose tail is to be set
 * @param tail    The new tail value
 **/
static void set_journal_tail(struct recovery_journal *journal,
			     sequence_number_t tail)
{
	// VDO does not support sequence numbers above 1 << 48 in the slab
	// journal.
	if (tail >= (1ULL << 48)) {
		enter_journal_read_only_mode(journal, VDO_JOURNAL_OVERFLOW);
	}

	journal->tail = tail;
}

/**********************************************************************/
int decode_recovery_journal(struct recovery_journal_state_7_0 state,
			    nonce_t nonce,
			    struct vdo *vdo,
			    struct partition *partition,
			    uint64_t recovery_count,
			    block_count_t journal_size,
			    block_count_t tail_buffer_size,
			    struct read_only_notifier *read_only_notifier,
			    const struct thread_config *thread_config,
			    struct recovery_journal **journal_ptr)
{
	block_count_t journal_length;
	block_count_t i;
	struct recovery_journal *journal;
	int result = ALLOCATE(1, struct recovery_journal, __func__, &journal);
	if (result != VDO_SUCCESS) {
		return result;
	}

	INIT_LIST_HEAD(&journal->free_tail_blocks);
	INIT_LIST_HEAD(&journal->active_tail_blocks);
	initialize_wait_queue(&journal->pending_writes);

	journal->thread_id = get_journal_zone_thread(thread_config);
	journal->partition = partition;
	journal->nonce = nonce;
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	journal->size = journal_size;
	journal->read_only_notifier = read_only_notifier;
	journal->slab_journal_commit_threshold = (journal_size * 2) / 3;
	journal->logical_blocks_used = state.logical_blocks_used;
	journal->block_map_data_blocks = state.block_map_data_blocks;
	set_journal_tail(journal, state.journal_start);
	initialize_journal_state(journal);

	// XXX: this is a hack until we make initial resume of a VDO a real
	// resume
	journal->state.current_state = ADMIN_STATE_SUSPENDED;

	journal->entries_per_block = RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
	journal_length = get_recovery_journal_length(journal_size);
	journal->available_space = journal->entries_per_block * journal_length;

	for (i = 0; i < tail_buffer_size; i++) {
		struct recovery_journal_block *block;
		result = make_vdo_recovery_block(vdo, journal, &block);
		if (result != VDO_SUCCESS) {
			free_recovery_journal(&journal);
			return result;
		}

		list_move_tail(&block->list_node, &journal->free_tail_blocks);
	}

	result = make_vdo_lock_counter(vdo,
				       journal,
				       reap_recovery_journal_callback,
				       journal->thread_id,
				       thread_config->logical_zone_count,
				       thread_config->physical_zone_count,
				       journal->size,
				       &journal->lock_counter);
	if (result != VDO_SUCCESS) {
		free_recovery_journal(&journal);
		return result;
	}

	result = create_metadata_vio(vdo,
				     VIO_TYPE_RECOVERY_JOURNAL,
				     VIO_PRIORITY_HIGH,
				     journal,
				     NULL,
				     &journal->flush_vio);
	if (result != VDO_SUCCESS) {
		free_recovery_journal(&journal);
		return result;
	}

	result = register_vdo_read_only_listener(read_only_notifier,
						journal,
						notify_recovery_journal_of_read_only_mode,
						journal->thread_id);
	if (result != VDO_SUCCESS) {
		free_recovery_journal(&journal);
		return result;
	}

	journal->flush_vio->completion.callback_thread_id =
		journal->thread_id;
	*journal_ptr = journal;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_recovery_journal(struct recovery_journal **journal_ptr)
{
	struct recovery_journal_block *block;
	struct recovery_journal *journal = *journal_ptr;
	if (journal == NULL) {
		return;
	}

	free_vdo_lock_counter(&journal->lock_counter);
	free_vio(&journal->flush_vio);

	// XXX: eventually, the journal should be constructed in a quiescent
	// state
	//      which requires opening before use.
	if (!is_vdo_state_quiescent(&journal->state)) {
		ASSERT_LOG_ONLY(list_empty(&journal->active_tail_blocks),
				"journal being freed has no active tail blocks");
	} else if (!is_vdo_state_saved(&journal->state)
		   && !list_empty(&journal->active_tail_blocks)) {
		uds_log_warning("journal being freed has uncommitted entries");
	}

	while ((block = pop_active_list(journal)) != NULL) {
		free_vdo_recovery_block(&block);
	}

	while ((block = pop_free_list(journal)) != NULL) {
		free_vdo_recovery_block(&block);
	}

	FREE(journal);
	*journal_ptr = NULL;
}

/**********************************************************************/
void set_recovery_journal_partition(struct recovery_journal *journal,
				    struct partition *partition)
{
	journal->partition = partition;
}

/**********************************************************************/
void initialize_recovery_journal_post_recovery(struct recovery_journal *journal,
					       uint64_t recovery_count,
					       sequence_number_t tail)
{
	set_journal_tail(journal, tail + 1);
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	initialize_journal_state(journal);
}

/**********************************************************************/
void
initialize_recovery_journal_post_rebuild(struct recovery_journal *journal,
					 uint64_t recovery_count,
					 sequence_number_t tail,
					 block_count_t logical_blocks_used,
					 block_count_t block_map_data_blocks)
{
	initialize_recovery_journal_post_recovery(journal, recovery_count,
						  tail);
	journal->logical_blocks_used = logical_blocks_used;
	journal->block_map_data_blocks = block_map_data_blocks;
}

/**********************************************************************/
block_count_t
get_journal_block_map_data_blocks_used(struct recovery_journal *journal)
{
	return journal->block_map_data_blocks;
}

/**********************************************************************/
void set_journal_block_map_data_blocks_used(struct recovery_journal *journal,
					    block_count_t pages)
{
	journal->block_map_data_blocks = pages;
}

/**********************************************************************/
thread_id_t get_recovery_journal_thread_id(struct recovery_journal *journal)
{
	return journal->thread_id;
}

/**********************************************************************/
void open_recovery_journal(struct recovery_journal *journal,
			   struct slab_depot *depot,
			   struct block_map *block_map)
{
	journal->depot = depot;
	journal->block_map = block_map;
	WRITE_ONCE(journal->state.current_state, ADMIN_STATE_NORMAL_OPERATION);
}

/**********************************************************************/
struct recovery_journal_state_7_0
record_recovery_journal(const struct recovery_journal *journal)
{
	struct recovery_journal_state_7_0 state = {
		.logical_blocks_used = journal->logical_blocks_used,
		.block_map_data_blocks = journal->block_map_data_blocks,
	};

	if (is_vdo_state_saved(&journal->state)) {
		// If the journal is saved, we should start one past the active
		// block (since the active block is not guaranteed to be empty).
		state.journal_start = journal->tail;
	} else {
		// When we're merely suspended or have gone read-only, we must
		// record the first block that might have entries that need to
		// be applied.
		state.journal_start = get_recovery_journal_head(journal);
	}

	return state;
}

/**
 * Advance the tail of the journal.
 *
 * @param journal  The journal whose tail should be advanced
 *
 * @return <code>true</code> if the tail was advanced
 **/
static bool advance_tail(struct recovery_journal *journal)
{
	journal->active_block = pop_free_list(journal);
	if (journal->active_block == NULL) {
		return false;
	}

	list_move_tail(&journal->active_block->list_node,
		       &journal->active_tail_blocks);
	initialize_vdo_recovery_block(journal->active_block);
	set_journal_tail(journal, journal->tail + 1);
	advance_block_map_era(journal->block_map, journal->tail);
	return true;
}

/**
 * Check whether there is space to make a given type of entry.
 *
 * @param journal    The journal to check
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to make an
 *         entry of the specified type
 **/
static bool check_for_entry_space(struct recovery_journal *journal,
				  bool increment)
{
	if (increment) {
		return ((journal->available_space
			 - journal->pending_decrement_count)
			> 1);
	}

	return (journal->available_space > 0);
}

/**
 * Prepare the currently active block to receive an entry and check whether
 * an entry of the given type may be assigned at this time.
 *
 * @param journal    The journal receiving an entry
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to store an
 *         entry of the specified type
 **/
static bool prepare_to_assign_entry(struct recovery_journal *journal,
				    bool increment)
{
	if (!check_for_entry_space(journal, increment)) {
		if (!increment) {
			// There must always be room to make a decrement entry.
			uds_log_error("No space for decrement entry in recovery journal");
			enter_journal_read_only_mode(journal,
						     VDO_RECOVERY_JOURNAL_FULL);
		}
		return false;
	}

	if (is_vdo_recovery_block_full(journal->active_block)
	    && !advance_tail(journal)) {
		return false;
	}

	if (!is_vdo_recovery_block_empty(journal->active_block)) {
		return true;
	}

	if ((journal->tail - get_recovery_journal_head(journal)) >
	    journal->size) {
		// Cannot use this block since the journal is full.
		journal->events.disk_full++;
		return false;
	}

	/*
	 * Don't allow the new block to be reaped until all of its entries have
	 * been committed to the block map and until the journal block has been
	 * fully committed as well. Because the block map update is done only
	 * after any slab journal entries have been made, the per-entry lock
	 * for the block map entry serves to protect those as well.
	 */
	initialize_vdo_lock_count(journal->lock_counter,
				  journal->active_block->block_number,
				  journal->entries_per_block + 1);
	return true;
}

static void write_blocks(struct recovery_journal *journal);

/**
 * Queue a block for writing. The block is expected to be full. If the block
 * is currently writing, this is a noop as the block will be queued for
 * writing when the write finishes. The block must not currently be queued
 * for writing.
 *
 * @param journal  The journal in question
 * @param block    The block which is now ready to write
 **/
static void schedule_block_write(struct recovery_journal *journal,
				 struct recovery_journal_block *block)
{
	int result;

	if (block->committing) {
		return;
	}

	result = enqueue_waiter(&journal->pending_writes,
			        &block->write_waiter);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
	}

	/*
	 * At the end of adding entries, or discovering this partial
	 * block is now full and ready to rewrite, we will call
	 * write_blocks() and write a whole batch.
	 */
}

/**
 * Release a reference to a journal block.
 *
 * @param block  The journal block from which to release a reference
 **/
static void release_journal_block_reference(struct recovery_journal_block *block)
{
	release_vdo_journal_zone_reference(block->journal->lock_counter,
					   block->block_number);
}

/**
 * Implements waiter_callback. Assign an entry waiter to the active block.
 **/
static void assign_entry(struct waiter *waiter, void *context)
{
	int result;
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal_block *block =
		(struct recovery_journal_block *)context;
	struct recovery_journal *journal = block->journal;

	// Record the point at which we will make the journal entry.
	data_vio->recovery_journal_point = (struct journal_point) {
		.sequence_number = block->sequence_number,
		.entry_count = block->entry_count,
	};

	switch (data_vio->operation.type) {
	case DATA_INCREMENT:
		if (data_vio->operation.state != MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used++;
		}
		journal->pending_decrement_count++;
		break;

	case DATA_DECREMENT:
		if (data_vio->operation.state != MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used--;
		}

		// Per-entry locks need not be held for decrement entries since
		// the lock held for the incref entry will protect this entry
		// as well.
		release_journal_block_reference(block);
		ASSERT_LOG_ONLY((journal->pending_decrement_count != 0),
				"decrement follows increment");
		journal->pending_decrement_count--;
		break;

	case BLOCK_MAP_INCREMENT:
		journal->block_map_data_blocks++;
		break;

	default:
		uds_log_error("Invalid journal operation %u",
			      data_vio->operation.type);
		enter_journal_read_only_mode(journal, VDO_NOT_IMPLEMENTED);
		continue_data_vio(data_vio, VDO_NOT_IMPLEMENTED);
		return;
	}

	journal->available_space--;
	result = enqueue_vdo_recovery_block_entry(block, data_vio);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		continue_data_vio(data_vio, result);
	}

	if (is_vdo_recovery_block_full(block)) {
		// The block is full, so we can write it anytime henceforth. If
		// it is already committing, we'll queue it for writing when it
		// comes back.
		schedule_block_write(journal, block);
	}

	// Force out slab journal tail blocks when threshold is reached.
	check_slab_journal_commit_threshold(journal);
}

/**********************************************************************/
static bool assign_entries_from_queue(struct recovery_journal *journal,
				      struct wait_queue *queue, bool increment)
{
	while (has_waiters(queue)) {
		if (!prepare_to_assign_entry(journal, increment)) {
			return false;
		}

		notify_next_waiter(queue, assign_entry, journal->active_block);
	}

	return true;
}

/**********************************************************************/
static void assign_entries(struct recovery_journal *journal)
{
	if (journal->adding_entries) {
		// Protect against re-entrancy.
		return;
	}

	journal->adding_entries = true;
	if (assign_entries_from_queue(journal, &journal->decrement_waiters,
				      false)) {
		assign_entries_from_queue(journal, &journal->increment_waiters,
					  true);
	}

	// Now that we've finished with entries, see if we have a batch of
	// blocks to write.
	write_blocks(journal);
	journal->adding_entries = false;
}

/**
 * Prepare an in-memory journal block to be reused now that it has been fully
 * committed.
 *
 * @param block  The block to be recycled
 **/
static void recycle_journal_block(struct recovery_journal_block *block)
{
	struct recovery_journal *journal = block->journal;
	block_count_t i;
	list_move_tail(&block->list_node, &journal->free_tail_blocks);

	// Release any unused entry locks.
	for (i = block->entry_count; i < journal->entries_per_block; i++) {
		release_journal_block_reference(block);
	}

	// Release our own lock against reaping now that the block is completely
	// committed, or we're giving up because we're in read-only mode.
	if (block->entry_count > 0) {
		release_journal_block_reference(block);
	}

	if (block == journal->active_block) {
		journal->active_block = NULL;
	}
}

/**
 * waiter_callback implementation invoked whenever a VIO is to be released
 * from the journal because its entry was committed to disk.
 **/
static void continue_committed_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal *journal = (struct recovery_journal *)context;
	int result = (vdo_is_read_only(journal->read_only_notifier) ?
			VDO_READ_ONLY : VDO_SUCCESS);
	ASSERT_LOG_ONLY(before_vdo_journal_point(&journal->commit_point,
						 &data_vio->recovery_journal_point),
			"DataVIOs released from recovery journal in order. Recovery journal point is (%llu, %u), but commit waiter point is (%llu, %u)",
			journal->commit_point.sequence_number,
			journal->commit_point.entry_count,
			data_vio->recovery_journal_point.sequence_number,
			data_vio->recovery_journal_point.entry_count);
	journal->commit_point = data_vio->recovery_journal_point;

	continue_waiter(waiter, &result);
}

/**
 * Notify any VIOs whose entries have now committed.
 *
 * @param journal  The recovery journal to update
 **/
static void notify_commit_waiters(struct recovery_journal *journal)
{
	struct list_head *entry;
	if (list_empty(&journal->active_tail_blocks)) {
		return;
	}

	list_for_each(entry, &journal->active_tail_blocks) {
		struct recovery_journal_block *block
			= vdo_recovery_block_from_list_entry(entry);

		if (block->committing) {
			return;
		}

		notify_all_waiters(&block->commit_waiters,
				   continue_committed_waiter,
				   journal);
		if (vdo_is_read_only(journal->read_only_notifier)) {
			notify_all_waiters(&block->entry_waiters,
					   continue_committed_waiter,
					   journal);
		} else if (is_vdo_recovery_block_dirty(block)
		           || !is_vdo_recovery_block_full(block)) {
			// Stop at partially-committed or partially-filled
			// blocks.
			return;
		}
	}
}

/**
 * Recycle any journal blocks which have been fully committed.
 *
 * @param journal  The recovery journal to update
 **/
static void recycle_journal_blocks(struct recovery_journal *journal)
{
	while (!list_empty(&journal->active_tail_blocks)) {
		struct recovery_journal_block *block
			= vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);

		if (block->committing) {
			// Don't recycle committing blocks.
			 return;
		}

		if (!vdo_is_read_only(journal->read_only_notifier)
		    && (is_vdo_recovery_block_dirty(block)
			|| !is_vdo_recovery_block_full(block))) {
			// Don't recycle partially written or partially full
			// blocks, except in read-only mode.
			return;
		}
		recycle_journal_block(block);
	}
}

/**
 * Handle post-commit processing. This is the callback registered by
 * write_block(). If more entries accumulated in the block being committed
 * while the commit was in progress, another commit will be initiated.
 *
 * @param completion  The completion of the VIO writing this block
 **/
static void complete_write(struct vdo_completion *completion)
{
	struct recovery_journal_block *block = completion->parent;
	struct recovery_journal *journal = block->journal;
	struct recovery_journal_block *last_active_block;
	assert_on_journal_thread(journal, __func__);

	journal->pending_write_count -= 1;
	journal->events.blocks.committed += 1;
	journal->events.entries.committed += block->entries_in_commit;
	block->uncommitted_entry_count -= block->entries_in_commit;
	block->entries_in_commit = 0;
	block->committing = false;

	// If this block is the latest block to be acknowledged, record that
	// fact.
	if (block->sequence_number > journal->last_write_acknowledged) {
		journal->last_write_acknowledged = block->sequence_number;
	}

	last_active_block =
		vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);
	ASSERT_LOG_ONLY((block->sequence_number >=
			 last_active_block->sequence_number),
			"completed journal write is still active");

	notify_commit_waiters(journal);

	// Is this block now full? Reaping, and adding entries, might have
	// already sent it off for rewriting; else, queue it for rewrite.
	if (is_vdo_recovery_block_dirty(block) && is_vdo_recovery_block_full(block)) {
		schedule_block_write(journal, block);
	}

	recycle_journal_blocks(journal);
	write_blocks(journal);

	check_for_drain_complete(journal);
}

/**********************************************************************/
static void handle_write_error(struct vdo_completion *completion)
{
	struct recovery_journal_block *block = completion->parent;
	struct recovery_journal *journal = block->journal;
	log_error_strerror(completion->result,
			   "cannot write recovery journal block %llu",
			   block->sequence_number);
	enter_journal_read_only_mode(journal, completion->result);
	complete_write(completion);
}

/**
 * Issue a block for writing. Implements waiter_callback.
 **/
static void write_block(struct waiter *waiter, void *context __always_unused)
{
	int result;
	struct recovery_journal_block *block
		= container_of(waiter, struct recovery_journal_block,
			       write_waiter);

	if (vdo_is_read_only(block->journal->read_only_notifier)) {
		return;
	}

	result = commit_vdo_recovery_block(block, complete_write,
					   handle_write_error);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(block->journal, result);
	}
}

/**
 * Attempt to commit blocks, according to write policy.
 *
 * @param journal     The recovery journal
 **/
static void write_blocks(struct recovery_journal *journal)
{
	assert_on_journal_thread(journal, __func__);
	/*
	 * We call this function after adding entries to the journal and after
	 * finishing a block write. Thus, when this function terminates we must
	 * either have no VIOs waiting in the journal or have some outstanding
	 * IO to provide a future wakeup.
	 *
	 * We want to only issue full blocks if there are no pending writes.
	 * However, if there are no outstanding writes and some unwritten
	 * entries, we must issue a block, even if it's the active block and it
	 * isn't full.
	 */
	if (journal->pending_write_count > 0) {
		return;
	}

	// Write all the full blocks.
	notify_all_waiters(&journal->pending_writes, write_block, NULL);

	// Do we need to write the active block? Only if we have no outstanding
	// writes, even after issuing all of the full writes.
	if ((journal->pending_write_count == 0)
	    && can_commit_vdo_recovery_block(journal->active_block)) {
		write_block(&journal->active_block->write_waiter, NULL);
	}
}

/**********************************************************************/
void add_recovery_journal_entry(struct recovery_journal *journal,
				struct data_vio *data_vio)
{
	bool increment;
	int result;

	assert_on_journal_thread(journal, __func__);
	if (!is_vdo_state_normal(&journal->state)) {
		continue_data_vio(data_vio, VDO_INVALID_ADMIN_STATE);
		return;
	}

	if (vdo_is_read_only(journal->read_only_notifier)) {
		continue_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	increment = is_increment_operation(data_vio->operation.type);
	ASSERT_LOG_ONLY((!increment ||
			 (data_vio->recovery_sequence_number == 0)),
			"journal lock not held for increment");

	advance_vdo_journal_point(&journal->append_point,
				  journal->entries_per_block);
	result = enqueue_data_vio((increment ? &journal->increment_waiters
				  : &journal->decrement_waiters),
				  data_vio);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		continue_data_vio(data_vio, result);
		return;
	}

	assign_entries(journal);
}

/**
 * Conduct a sweep on a recovery journal to reclaim unreferenced blocks.
 *
 * @param journal  The recovery journal
 **/
static void reap_recovery_journal(struct recovery_journal *journal)
{
	if (journal->reaping) {
		// We already have an outstanding reap in progress. We need to
		// wait for it to finish.
		return;
	}

	if (is_vdo_state_quiescent(&journal->state)) {
		// We are supposed to not do IO. Don't botch it by reaping.
		return;
	}

	// Start reclaiming blocks only when the journal head has no
	// references. Then stop when a block is referenced.
	while ((journal->block_map_reap_head < journal->last_write_acknowledged)
	       && !is_vdo_lock_locked(journal->lock_counter,
				      journal->block_map_head_block_number,
				      ZONE_TYPE_LOGICAL)) {
		journal->block_map_reap_head++;
		if (++journal->block_map_head_block_number == journal->size) {
			journal->block_map_head_block_number = 0;
		}
	}

	while ((journal->slab_journal_reap_head < journal->last_write_acknowledged)
	       && !is_vdo_lock_locked(journal->lock_counter,
				      journal->slab_journal_head_block_number,
				      ZONE_TYPE_PHYSICAL)) {
		journal->slab_journal_reap_head++;
		if (++journal->slab_journal_head_block_number == journal->size) {
			journal->slab_journal_head_block_number = 0;
		}
	}

	if ((journal->block_map_reap_head == journal->block_map_head)
	    && (journal->slab_journal_reap_head == journal->slab_journal_head)) {
		// Nothing happened.
		return;
	}

	/*
	 * If the block map head will advance, we must flush any block
	 * map page modified by the entries we are reaping. If the slab
	 * journal head will advance, we must flush the slab summary
	 * update covering the slab journal that just released some
	 * lock.
	 */
	journal->reaping = true;
	launch_flush(journal->flush_vio, complete_reaping, handle_flush_error);
}

/**********************************************************************/
void acquire_recovery_journal_block_reference(struct recovery_journal *journal,
					      sequence_number_t sequence_number,
					      enum vdo_zone_type zone_type,
					      zone_count_t zone_id)
{
	block_count_t block_number;
	if (sequence_number == 0) {
		return;
	}

	block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	acquire_vdo_lock_count_reference(journal->lock_counter, block_number,
					 zone_type, zone_id);
}

/**********************************************************************/
void release_recovery_journal_block_reference(struct recovery_journal *journal,
					      sequence_number_t sequence_number,
					      enum vdo_zone_type zone_type,
					      zone_count_t zone_id)
{
	block_count_t block_number;

	if (sequence_number == 0) {
		return;
	}

	block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	release_vdo_lock_count_reference(journal->lock_counter, block_number,
					 zone_type, zone_id);
}

/**********************************************************************/
void release_per_entry_lock_from_other_zone(struct recovery_journal *journal,
					    sequence_number_t sequence_number)
{
	block_count_t block_number;
	if (sequence_number == 0) {
		return;
	}

	block_number =
		get_recovery_journal_block_number(journal, sequence_number);
	release_vdo_journal_zone_reference_from_other_zone(journal->lock_counter,
							   block_number);
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct recovery_journal,
					      state));
}

/**********************************************************************/
void drain_recovery_journal(struct recovery_journal *journal,
			    enum admin_state_code operation,
			    struct vdo_completion *parent)
{
	assert_on_journal_thread(journal, __func__);
	start_vdo_draining(&journal->state, operation, parent, initiate_drain);
}

/**********************************************************************/
void resume_recovery_journal(struct recovery_journal *journal,
			     struct vdo_completion *parent)
{
	bool saved;

	assert_on_journal_thread(journal, __func__);
	saved = is_vdo_state_saved(&journal->state);
	set_vdo_completion_result(parent, resume_vdo_if_quiescent(&journal->state));

	if (vdo_is_read_only(journal->read_only_notifier)) {
		finish_vdo_completion(parent, VDO_READ_ONLY);
		return;
	}

	if (saved) {
		initialize_journal_state(journal);
	}

	if (resume_vdo_lock_counter(journal->lock_counter)) {
		// We might have missed a notification.
		reap_recovery_journal(journal);
	}

	complete_vdo_completion(parent);
}

/**********************************************************************/
block_count_t
get_journal_logical_blocks_used(const struct recovery_journal *journal)
{
	return journal->logical_blocks_used;
}

/**********************************************************************/
struct recovery_journal_statistics
get_recovery_journal_statistics(const struct recovery_journal *journal)
{
	return journal->events;
}

/**********************************************************************/
void dump_recovery_journal_statistics(const struct recovery_journal *journal)
{
	const struct list_head *head;
	struct list_head *entry;

	struct recovery_journal_statistics stats =
		get_recovery_journal_statistics(journal);
	log_info("Recovery Journal");
	log_info("  block_map_head=%llu slab_journal_head=%llu last_write_acknowledged=%llu tail=%llu block_map_reap_head=%llu slab_journal_reap_head=%llu disk_full=%llu slab_journal_commits_requested=%llu increment_waiters=%zu decrement_waiters=%zu",
		 journal->block_map_head, journal->slab_journal_head,
		 journal->last_write_acknowledged, journal->tail,
		 journal->block_map_reap_head, journal->slab_journal_reap_head,
		 stats.disk_full, stats.slab_journal_commits_requested,
		 count_waiters(&journal->increment_waiters),
		 count_waiters(&journal->decrement_waiters));
	log_info("  entries: started=%llu written=%llu committed=%llu",
		 stats.entries.started, stats.entries.written,
		 stats.entries.committed);
	log_info("  blocks: started=%llu written=%llu committed=%llu",
		 stats.blocks.started, stats.blocks.written,
		 stats.blocks.committed);

	log_info("  active blocks:");
	head = &journal->active_tail_blocks;
	list_for_each(entry, head) {
		dump_vdo_recovery_block(vdo_recovery_block_from_list_entry(entry));
	}
}
