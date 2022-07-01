// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "recovery-journal.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "constants.h"
#include "data-vio.h"
#include "header.h"
#include "io-submitter.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal-block.h"
#include "recovery-journal-format.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"
#include "vio.h"
#include "wait-queue.h"

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
 * pop_free_list() - Get a block from the end of the free list.
 * @journal: The journal.
 *
 * Return: The block or NULL if the list is empty.
 */
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
 * pop_active_list() - Get a block from the end of the active list.
 * @journal: The journal.
 *
 * Return: The block or NULL if the list is empty.
 */
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
 * assert_on_journal_thread() - Assert that we are running on the journal
 *                              thread.
 * @journal: The journal.
 * @function_name: The function doing the check (for logging).
 */
static void assert_on_journal_thread(struct recovery_journal *journal,
				     const char *function_name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == journal->thread_id),
			"%s() called on journal thread", function_name);
}

/**
 * continue_waiter() - Release a data_vio from the journal.
 *
 * Invoked whenever a data_vio is to be released from the journal, either
 * because its entry was committed to disk, or because there was an error.
 * Implements waiter_callback.
 */
static void continue_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	int wait_result = *((int *)context);

	continue_data_vio(data_vio, wait_result);
}

/**
 * has_block_waiters() - Check whether the journal has any waiters on any
 *                       blocks.
 * @journal: The journal in question.
 *
 * Return: true if any block has a waiter.
 */
static inline bool has_block_waiters(struct recovery_journal *journal)
{
	struct recovery_journal_block *block;

	/*
	 * Either the first active tail block (if it exists) has waiters,
	 * or no active tail block has waiters.
	 */
	if (list_empty(&journal->active_tail_blocks)) {
		return false;
	}

	block = vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);
	return (has_waiters(&block->entry_waiters)
		|| has_waiters(&block->commit_waiters));
}

static void recycle_journal_blocks(struct recovery_journal *journal);
static void recycle_journal_block(struct recovery_journal_block *block);
static void notify_commit_waiters(struct recovery_journal *journal);

/**
 * check_for_drain_complete() - Check whether the journal has drained.
 * @journal: The journal which may have just drained.
 */
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

		/* Release any data_vios waiting to be assigned entries. */
		notify_all_waiters(&journal->decrement_waiters,
				   continue_waiter, &result);
		notify_all_waiters(&journal->increment_waiters,
				   continue_waiter, &result);
	}

	if (!vdo_is_state_draining(&journal->state) || journal->reaping
	    || has_block_waiters(journal)
	    || has_waiters(&journal->increment_waiters)
	    || has_waiters(&journal->decrement_waiters)
	    || !vdo_suspend_lock_counter(journal->lock_counter)) {
		return;
	}

	if (vdo_is_state_saving(&journal->state)) {
		if (journal->active_block != NULL) {
			ASSERT_LOG_ONLY(((result == VDO_READ_ONLY)
					 || !vdo_is_recovery_block_dirty(journal->active_block)),
					"journal being saved has clean active block");
			recycle_journal_block(journal->active_block);
		}

		ASSERT_LOG_ONLY(list_empty(&journal->active_tail_blocks),
				"all blocks in a journal being saved must be inactive");
	}

	vdo_finish_draining_with_result(&journal->state, result);
}

/**
 * notify_recovery_journal_of_read_only_mode() - Notify a recovery journal
 *                                               that the VDO has gone
 *                                               read-only.
 * @listener: The journal.
 * @parent: The completion to notify in order to acknowledge the notification.
 *
 * Implements vdo_read_only_notification.
 */
static void
notify_recovery_journal_of_read_only_mode(void *listener,
					  struct vdo_completion *parent)
{
	check_for_drain_complete(listener);
	vdo_complete_completion(parent);
}

/**
 * enter_journal_read_only_mode() - Put the journal in read-only mode.
 * @journal: The journal which has failed.
 * @error_code: The error result triggering this call.
 *
 * All attempts to add entries after this function is called will fail. All
 * VIOs waiting for commits will be awakened with an error.
 */
static void enter_journal_read_only_mode(struct recovery_journal *journal,
					 int error_code)
{
	vdo_enter_read_only_mode(journal->read_only_notifier, error_code);
	check_for_drain_complete(journal);
}

/**
 * vdo_get_recovery_journal_current_sequence_number() - Obtain the recovery
 *                                                      journal's current
 *                                                      sequence number.
 * @journal: The journal in question.
 *
 * Exposed only so the block map can be initialized therefrom.
 *
 * Return: The sequence number of the tail block.
 */
sequence_number_t
vdo_get_recovery_journal_current_sequence_number(struct recovery_journal *journal)
{
	return journal->tail;
}

/**
 * get_recovery_journal_head() - Get the head of the recovery journal.
 * @journal: The journal.
 *
 * The head is the lowest sequence number of the block map head and the slab
 * journal head.
 *
 * Return: the head of the journal.
 */
static inline sequence_number_t
get_recovery_journal_head(const struct recovery_journal *journal)
{
	return min(journal->block_map_head, journal->slab_journal_head);
}

/**
 * compute_recovery_count_byte() - Compute the recovery count byte for a given
 *                                 recovery count.
 * @recovery_count: The recovery count.
 *
 * Return: The byte corresponding to the recovery count.
 */
static inline uint8_t __must_check
compute_recovery_count_byte(uint64_t recovery_count)
{
	return (uint8_t)(recovery_count & RECOVERY_COUNT_MASK);
}

/**
 * check_slab_journal_commit_threshold() - Check whether the journal is over
 *                                         the threshold, and if so, force the
 *                                         oldest slab journal tail block to
 *                                         commit.
 * @journal: The journal.
 */
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

static void reap_recovery_journal(struct recovery_journal *journal);
static void assign_entries(struct recovery_journal *journal);

/**
 * finish_reaping() - Finish reaping the journal.
 * @journal: The journal being reaped.
 */
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
 * complete_reaping() - Finish reaping the journal after flushing the lower
 *                      layer.
 * @completion: The journal's flush VIO.
 *
 * This is the callback registered in reap_recovery_journal().
 */
static void complete_reaping(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;

	finish_reaping(journal);

	/*
	 * Try reaping again in case more locks were released while flush was
	 * out.
	 */
	reap_recovery_journal(journal);
}

/**
 * handle_flush_error() - Handle an error when flushing the lower layer due to
 *                        reaping.
 * @completion: The journal's flush VIO.
 */
static void handle_flush_error(struct vdo_completion *completion)
{
	struct recovery_journal *journal = completion->parent;

	record_metadata_io_error(as_vio(completion));
	journal->reaping = false;
	enter_journal_read_only_mode(journal, completion->result);
}

static void flush_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct recovery_journal *journal = vio->completion.parent;

	continue_vio_after_io(vio,
			      complete_reaping,
			      journal->thread_id);
}

/**
 * initialize_journal_state() - Set all journal fields appropriately to start
 *                              journaling from the current active block.
 * @journal: The journal to be reset based on its active block.
 */
static void initialize_journal_state(struct recovery_journal *journal)
{
	journal->append_point.sequence_number = journal->tail;
	journal->last_write_acknowledged = journal->tail;
	journal->block_map_head = journal->tail;
	journal->slab_journal_head = journal->tail;
	journal->block_map_reap_head = journal->tail;
	journal->slab_journal_reap_head = journal->tail;
	journal->block_map_head_block_number =
		vdo_get_recovery_journal_block_number(journal,
						      journal->block_map_head);
	journal->slab_journal_head_block_number =
		vdo_get_recovery_journal_block_number(journal,
						      journal->slab_journal_head);
	journal->available_space =
		(journal->entries_per_block *
		 vdo_get_recovery_journal_length(journal->size));
}

/**
 * vdo_get_recovery_journal_length() - Get the number of usable recovery
 *                                     journal blocks.
 * @journal_size: The size of the recovery journal in blocks.
 *
 * Return: the number of recovery journal blocks usable for entries.
 */
block_count_t vdo_get_recovery_journal_length(block_count_t journal_size)
{
	block_count_t reserved_blocks = journal_size / 4;

	if (reserved_blocks > RECOVERY_JOURNAL_RESERVED_BLOCKS) {
		reserved_blocks = RECOVERY_JOURNAL_RESERVED_BLOCKS;
	}
	return (journal_size - reserved_blocks);
}

/**
 * reap_recovery_journal_callback() - Attempt to reap the journal.
 * @completion: The lock counter completion.
 *
 * Attempts to reap the journal now that all the locks on some journal block
 * have been released. This is the callback registered with the lock counter.
 */
static void reap_recovery_journal_callback(struct vdo_completion *completion)
{
	struct recovery_journal *journal =
		(struct recovery_journal *)completion->parent;
	/*
	 * The acknowledgement must be done before reaping so that there is no
	 * race between acknowledging the notification and unlocks wishing to
	 * notify.
	 */
	vdo_acknowledge_lock_unlock(journal->lock_counter);

	if (vdo_is_state_quiescing(&journal->state)) {
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

/**
 * set_journal_tail() - Set the journal's tail sequence number.
 * @journal: The journal whose tail is to be set.
 * @tail: The new tail value.
 */
static void set_journal_tail(struct recovery_journal *journal,
			     sequence_number_t tail)
{
	/*
	 * VDO does not support sequence numbers above 1 << 48 in the slab
	 * journal.
	 */
	if (tail >= (1ULL << 48)) {
		enter_journal_read_only_mode(journal, VDO_JOURNAL_OVERFLOW);
	}

	journal->tail = tail;
}

/**
 * vdo_decode_recovery_journal() - Make a recovery journal and initialize it
 *                                 with the state that was decoded from the
 *                                 super block.
 *
 * @state: The decoded state of the journal.
 * @nonce: The nonce of the VDO.
 * @vdo: The VDO.
 * @partition: The partition for the journal.
 * @recovery_count: The VDO's number of completed recoveries.
 * @journal_size: The number of blocks in the journal on disk.
 * @tail_buffer_size: The number of blocks for tail buffer.
 * @read_only_notifier: The read-only mode notifier.
 * @thread_config: The thread configuration of the VDO.
 * @journal_ptr: The pointer to hold the new recovery journal.
 *
 * Return: A success or error code.
 */
int vdo_decode_recovery_journal(struct recovery_journal_state_7_0 state,
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
	block_count_t i;
	struct recovery_journal *journal;
	int result = UDS_ALLOCATE(1, struct recovery_journal, __func__, &journal);

	if (result != VDO_SUCCESS) {
		return result;
	}

	INIT_LIST_HEAD(&journal->free_tail_blocks);
	INIT_LIST_HEAD(&journal->active_tail_blocks);
	initialize_wait_queue(&journal->pending_writes);

	journal->thread_id = thread_config->journal_thread;
	journal->partition = partition;
	journal->nonce = nonce;
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	journal->size = journal_size;
	journal->read_only_notifier = read_only_notifier;
	journal->slab_journal_commit_threshold = (journal_size * 2) / 3;
	journal->logical_blocks_used = state.logical_blocks_used;
	journal->block_map_data_blocks = state.block_map_data_blocks;
	journal->entries_per_block = RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
	set_journal_tail(journal, state.journal_start);
	initialize_journal_state(journal);

	/*
	 * XXX: this is a hack until we make initial resume of a VDO a real
	 * resume
	 */
	vdo_set_admin_state_code(&journal->state, VDO_ADMIN_STATE_SUSPENDED);

	for (i = 0; i < tail_buffer_size; i++) {
		struct recovery_journal_block *block;

		result = vdo_make_recovery_block(vdo, journal, &block);
		if (result != VDO_SUCCESS) {
			vdo_free_recovery_journal(journal);
			return result;
		}

		list_move_tail(&block->list_node, &journal->free_tail_blocks);
	}

	result = vdo_make_lock_counter(vdo,
				       journal,
				       reap_recovery_journal_callback,
				       journal->thread_id,
				       thread_config->logical_zone_count,
				       thread_config->physical_zone_count,
				       journal->size,
				       &journal->lock_counter);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_journal(journal);
		return result;
	}

	result = create_metadata_vio(vdo,
				     VIO_TYPE_RECOVERY_JOURNAL,
				     VIO_PRIORITY_HIGH,
				     journal,
				     NULL,
				     &journal->flush_vio);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_journal(journal);
		return result;
	}

	result = vdo_register_read_only_listener(read_only_notifier,
						journal,
						notify_recovery_journal_of_read_only_mode,
						journal->thread_id);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_journal(journal);
		return result;
	}

	result = vdo_make_default_thread(vdo, journal->thread_id);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_journal(journal);
		return result;
	}

	journal->flush_vio->completion.callback_thread_id = journal->thread_id;
	*journal_ptr = journal;
	return VDO_SUCCESS;
}

/**
 * vdo_free_recovery_journal() - Free a recovery journal.
 * @journal: The recovery journal to free.
 */
void vdo_free_recovery_journal(struct recovery_journal *journal)
{
	struct recovery_journal_block *block;

	if (journal == NULL) {
		return;
	}

	vdo_free_lock_counter(UDS_FORGET(journal->lock_counter));
	free_vio(UDS_FORGET(journal->flush_vio));

	/*
	 * XXX: eventually, the journal should be constructed in a quiescent
	 * state
	 *      which requires opening before use.
	 */
	if (!vdo_is_state_quiescent(&journal->state)) {
		ASSERT_LOG_ONLY(list_empty(&journal->active_tail_blocks),
				"journal being freed has no active tail blocks");
	} else if (!vdo_is_state_saved(&journal->state)
		   && !list_empty(&journal->active_tail_blocks)) {
		uds_log_warning("journal being freed has uncommitted entries");
	}

	while ((block = pop_active_list(journal)) != NULL) {
		vdo_free_recovery_block(block);
	}

	while ((block = pop_free_list(journal)) != NULL) {
		vdo_free_recovery_block(block);
	}

	UDS_FREE(journal);
}

/**
 * vdo_set_recovery_journal_partition() - Move the backing partition pointer
 *                                        of the recovery journal.
 * @journal: The journal being moved.
 * @partition: The new journal partition.
 *
 * Assumes that the data in the old and the new partitions is identical.
 */
void vdo_set_recovery_journal_partition(struct recovery_journal *journal,
					struct partition *partition)
{
	journal->partition = partition;
}

/**
 * vdo_initialize_recovery_journal_post_recovery() - Initialize the journal
 *                                                   after a recovery.
 * @journal: The journal in question.
 * @recovery_count: The number of completed recoveries.
 * @tail: The new tail block sequence number.
 */
void vdo_initialize_recovery_journal_post_recovery(struct recovery_journal *journal,
						   uint64_t recovery_count,
						   sequence_number_t tail)
{
	set_journal_tail(journal, tail + 1);
	journal->recovery_count = compute_recovery_count_byte(recovery_count);
	initialize_journal_state(journal);
}

/**
 * vdo_initialize_recovery_journal_post_rebuild() - Initialize the journal
 *                                                  after a rebuild.
 * @journal: The journal in question.
 * @recovery_count: The number of completed recoveries.
 * @tail: The new tail block sequence number.
 * @logical_blocks_used: The new number of logical blocks used.
 * @block_map_data_blocks: The new number of block map data blocks.
 */
void
vdo_initialize_recovery_journal_post_rebuild(struct recovery_journal *journal,
					     uint64_t recovery_count,
					     sequence_number_t tail,
					     block_count_t logical_blocks_used,
					     block_count_t block_map_data_blocks)
{
	vdo_initialize_recovery_journal_post_recovery(journal, recovery_count,
						      tail);
	journal->logical_blocks_used = logical_blocks_used;
	journal->block_map_data_blocks = block_map_data_blocks;
}

/**
 * vdo_get_journal_block_map_data_blocks_used() - Get the number of block map
 *                                                pages, allocated from data
 *                                                blocks, currently in use.
 * @journal: The journal in question.
 *
 * Return: The number of block map pages allocated from slabs.
 */
block_count_t
vdo_get_journal_block_map_data_blocks_used(struct recovery_journal *journal)
{
	return journal->block_map_data_blocks;
}

/**
 * vdo_get_recovery_journal_thread_id() - Get the ID of a recovery journal's
 *                                        thread.
 * @journal: The journal to query.
 *
 * Return: The ID of the journal's thread.
 */
thread_id_t vdo_get_recovery_journal_thread_id(struct recovery_journal *journal)
{
	return journal->thread_id;
}

/**
 * vdo_open_recovery_journal() - Prepare the journal for new entries.
 * @journal: The journal in question.
 * @depot: The slab depot for this VDO.
 * @block_map: The block map for this VDO.
 */
void vdo_open_recovery_journal(struct recovery_journal *journal,
			       struct slab_depot *depot,
			       struct block_map *block_map)
{
	journal->depot = depot;
	journal->block_map = block_map;
	WRITE_ONCE(journal->state.current_state,
		   VDO_ADMIN_STATE_NORMAL_OPERATION);
}

/**
 * vdo_record_recovery_journal() - Record the state of a recovery journal for
 *                                 encoding in the super block.
 * @journal: the recovery journal.
 *
 * Return: the state of the journal.
 */
struct recovery_journal_state_7_0
vdo_record_recovery_journal(const struct recovery_journal *journal)
{
	struct recovery_journal_state_7_0 state = {
		.logical_blocks_used = journal->logical_blocks_used,
		.block_map_data_blocks = journal->block_map_data_blocks,
	};

	if (vdo_is_state_saved(&journal->state)) {
		/*
		 * If the journal is saved, we should start one past the active
		 * block (since the active block is not guaranteed to be empty).
		 */
		state.journal_start = journal->tail;
	} else {
		/*
		 * When we're merely suspended or have gone read-only, we must
		 * record the first block that might have entries that need to
		 * be applied.
		 */
		state.journal_start = get_recovery_journal_head(journal);
	}

	return state;
}

/**
 * advance_tail() - Advance the tail of the journal.
 * @journal: The journal whose tail should be advanced.
 *
 * Return: true if the tail was advanced.
 */
static bool advance_tail(struct recovery_journal *journal)
{
	journal->active_block = pop_free_list(journal);
	if (journal->active_block == NULL) {
		return false;
	}

	list_move_tail(&journal->active_block->list_node,
		       &journal->active_tail_blocks);
	vdo_initialize_recovery_block(journal->active_block);
	set_journal_tail(journal, journal->tail + 1);
	vdo_advance_block_map_era(journal->block_map, journal->tail);
	return true;
}

/**
 * check_for_entry_space() - Check whether there is space to make a given type
 *                           of entry.
 * @journal: The journal to check.
 * @increment: Set to true if the desired entry is an increment.
 *
 * Return: true if there is space in the journal to make an entry of the
 *         specified type.
 */
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
 * prepare_to_assign_entry() - Prepare the currently active block to receive
 *                             an entry and check whether an entry of the
 *                             given type may be assigned at this time.
 * @journal: The journal receiving an entry.
 * @increment: Set to true if the desired entry is an increment.
 *
 * Return: true if there is space in the journal to store an entry of the
 *         specified type.
 */
static bool prepare_to_assign_entry(struct recovery_journal *journal,
				    bool increment)
{
	if (!check_for_entry_space(journal, increment)) {
		if (!increment) {
			/* There must always be room to make a decrement entry. */
			uds_log_error("No space for decrement entry in recovery journal");
			enter_journal_read_only_mode(journal,
						     VDO_RECOVERY_JOURNAL_FULL);
		}
		return false;
	}

	if (vdo_is_recovery_block_full(journal->active_block)
	    && !advance_tail(journal)) {
		return false;
	}

	if (!vdo_is_recovery_block_empty(journal->active_block)) {
		return true;
	}

	if ((journal->tail - get_recovery_journal_head(journal)) >
	    journal->size) {
		/* Cannot use this block since the journal is full. */
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
	vdo_initialize_lock_count(journal->lock_counter,
				  journal->active_block->block_number,
				  journal->entries_per_block + 1);
	return true;
}

static void write_blocks(struct recovery_journal *journal);

/**
 * schedule_block_write() - Queue a block for writing.
 * @journal: The journal in question.
 * @block: The block which is now ready to write.
 *
 * The block is expected to be full. If the block is currently writing, this
 * is a noop as the block will be queued for writing when the write finishes.
 * The block must not currently be queued for writing.
 */
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
 * release_journal_block_reference() - Release a reference to a journal block.
 * @block: The journal block from which to release a reference.
 */
static void release_journal_block_reference(struct recovery_journal_block *block)
{
	vdo_release_journal_zone_reference(block->journal->lock_counter,
					   block->block_number);
}

/**
 * assign_entry() - Assign an entry waiter to the active block.
 *
 * Implements waiter_callback.
 */
static void assign_entry(struct waiter *waiter, void *context)
{
	int result;
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal_block *block =
		(struct recovery_journal_block *)context;
	struct recovery_journal *journal = block->journal;

	/* Record the point at which we will make the journal entry. */
	data_vio->recovery_journal_point = (struct journal_point) {
		.sequence_number = block->sequence_number,
		.entry_count = block->entry_count,
	};

	switch (data_vio->operation.type) {
	case VDO_JOURNAL_DATA_INCREMENT:
		if (data_vio->operation.state != VDO_MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used++;
		}
		journal->pending_decrement_count++;
		break;

	case VDO_JOURNAL_DATA_DECREMENT:
		if (data_vio->operation.state != VDO_MAPPING_STATE_UNMAPPED) {
			journal->logical_blocks_used--;
		}

		/*
		 * Per-entry locks need not be held for decrement entries since
		 * the lock held for the incref entry will protect this entry
		 * as well.
		 */
		release_journal_block_reference(block);
		ASSERT_LOG_ONLY((journal->pending_decrement_count != 0),
				"decrement follows increment");
		journal->pending_decrement_count--;
		break;

	case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
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
	result = vdo_enqueue_recovery_block_entry(block, data_vio);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(journal, result);
		continue_data_vio(data_vio, result);
	}

	if (vdo_is_recovery_block_full(block)) {
		/*
		 * The block is full, so we can write it anytime henceforth. If
		 * it is already committing, we'll queue it for writing when it
		 * comes back.
		 */
		schedule_block_write(journal, block);
	}

	/* Force out slab journal tail blocks when threshold is reached. */
	check_slab_journal_commit_threshold(journal);
}

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

static void assign_entries(struct recovery_journal *journal)
{
	if (journal->adding_entries) {
		/* Protect against re-entrancy. */
		return;
	}

	journal->adding_entries = true;
	if (assign_entries_from_queue(journal, &journal->decrement_waiters,
				      false)) {
		assign_entries_from_queue(journal, &journal->increment_waiters,
					  true);
	}

	/*
	 * Now that we've finished with entries, see if we have a batch of
	 * blocks to write.
	 */
	write_blocks(journal);
	journal->adding_entries = false;
}

/**
 * recycle_journal_block() - Prepare an in-memory journal block to be reused
 *                           now that it has been fully committed.
 * @block: The block to be recycled.
 */
static void recycle_journal_block(struct recovery_journal_block *block)
{
	struct recovery_journal *journal = block->journal;
	block_count_t i;

	list_move_tail(&block->list_node, &journal->free_tail_blocks);

	/* Release any unused entry locks. */
	for (i = block->entry_count; i < journal->entries_per_block; i++) {
		release_journal_block_reference(block);
	}

	/*
	 * Release our own lock against reaping now that the block is completely
	 * committed, or we're giving up because we're in read-only mode.
	 */
	if (block->entry_count > 0) {
		release_journal_block_reference(block);
	}

	if (block == journal->active_block) {
		journal->active_block = NULL;
	}
}

/**
 * continue_committed_waiter() - invoked whenever a VIO is to be released from
 *                               the journal because its entry was committed
 *                               to disk.
 *
 * Implements waiter_callback.
 */
static void continue_committed_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct recovery_journal *journal = (struct recovery_journal *)context;
	int result = (vdo_is_read_only(journal->read_only_notifier) ?
			VDO_READ_ONLY : VDO_SUCCESS);
	ASSERT_LOG_ONLY(vdo_before_journal_point(&journal->commit_point,
						 &data_vio->recovery_journal_point),
			"DataVIOs released from recovery journal in order. Recovery journal point is (%llu, %u), but commit waiter point is (%llu, %u)",
			(unsigned long long) journal->commit_point.sequence_number,
			journal->commit_point.entry_count,
			(unsigned long long) data_vio->recovery_journal_point.sequence_number,
			data_vio->recovery_journal_point.entry_count);
	journal->commit_point = data_vio->recovery_journal_point;

	continue_waiter(waiter, &result);
}

/**
 * notify_commit_waiters() - Notify any VIOs whose entries have now committed.
 * @journal: The recovery journal to update.
 */
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
		} else if (vdo_is_recovery_block_dirty(block)
			   || !vdo_is_recovery_block_full(block)) {
			/*
			 * Stop at partially-committed or partially-filled
			 * blocks.
			 */
			return;
		}
	}
}

/**
 * recycle_journal_blocks() - Recycle any journal blocks which have been fully
 *                            committed.
 * @journal: The recovery journal to update.
 */
static void recycle_journal_blocks(struct recovery_journal *journal)
{
	while (!list_empty(&journal->active_tail_blocks)) {
		struct recovery_journal_block *block
			= vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);

		if (block->committing) {
			/* Don't recycle committing blocks. */
			return;
		}

		if (!vdo_is_read_only(journal->read_only_notifier)
		    && (vdo_is_recovery_block_dirty(block)
			|| !vdo_is_recovery_block_full(block))) {
			/*
			 * Don't recycle partially written or partially full
			 * blocks, except in read-only mode.
			 */
			return;
		}
		recycle_journal_block(block);
	}
}

/**
 * complete_write() - Handle post-commit processing.
 * @completion: The completion of the VIO writing this block.
 *
 * This is the callback registered by write_block(). If more entries
 * accumulated in the block being committed while the commit was in progress,
 * another commit will be initiated.
 */
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

	/*
	 * If this block is the latest block to be acknowledged, record that
	 * fact.
	 */
	if (block->sequence_number > journal->last_write_acknowledged) {
		journal->last_write_acknowledged = block->sequence_number;
	}

	last_active_block =
		vdo_recovery_block_from_list_entry(journal->active_tail_blocks.next);
	ASSERT_LOG_ONLY((block->sequence_number >=
			 last_active_block->sequence_number),
			"completed journal write is still active");

	notify_commit_waiters(journal);

	/*
	 * Is this block now full? Reaping, and adding entries, might have
	 * already sent it off for rewriting; else, queue it for rewrite.
	 */
	if (vdo_is_recovery_block_dirty(block) && vdo_is_recovery_block_full(block)) {
		schedule_block_write(journal, block);
	}

	recycle_journal_blocks(journal);
	write_blocks(journal);

	check_for_drain_complete(journal);
}

static void handle_write_error(struct vdo_completion *completion)
{
	struct recovery_journal_block *block = completion->parent;
	struct recovery_journal *journal = block->journal;

	record_metadata_io_error(as_vio(completion));
	uds_log_error_strerror(completion->result,
			       "cannot write recovery journal block %llu",
			       (unsigned long long) block->sequence_number);
	enter_journal_read_only_mode(journal, completion->result);
	complete_write(completion);
}

static void complete_write_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct recovery_journal_block *block = vio->completion.parent;
	struct recovery_journal *journal = block->journal;

	continue_vio_after_io(vio, complete_write, journal->thread_id);
}

/**
 * write_block() - Issue a block for writing.
 *
 * Implements waiter_callback.
 */
static void write_block(struct waiter *waiter, void *context __always_unused)
{
	int result;
	struct recovery_journal_block *block
		= container_of(waiter, struct recovery_journal_block,
			       write_waiter);

	if (vdo_is_read_only(block->journal->read_only_notifier)) {
		return;
	}

	result = vdo_commit_recovery_block(block,
					   complete_write_endio,
					   handle_write_error);
	if (result != VDO_SUCCESS) {
		enter_journal_read_only_mode(block->journal, result);
	}
}

/**
 * write_blocks() - Attempt to commit blocks, according to write policy.
 * @journal: The recovery journal.
 */
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

	/* Write all the full blocks. */
	notify_all_waiters(&journal->pending_writes, write_block, NULL);

	/*
	 * Do we need to write the active block? Only if we have no outstanding
	 * writes, even after issuing all of the full writes.
	 */
	if ((journal->pending_write_count == 0)
	    && vdo_can_commit_recovery_block(journal->active_block)) {
		write_block(&journal->active_block->write_waiter, NULL);
	}
}

/**
 * vdo_add_recovery_journal_entry() - Add an entry to a recovery journal.
 * @journal: The journal in which to make an entry.
 * @data_vio: The data_vio for which to add the entry. The entry will be taken
 *            from the logical and new_mapped fields of the data_vio. The
 *            data_vio's recovery_sequence_number field will be set to the
 *            sequence number of the journal block in which the entry was
 *            made.
 *
 * This method is asynchronous. The data_vio will not be called back until the
 * entry is committed to the on-disk journal.
 */
void vdo_add_recovery_journal_entry(struct recovery_journal *journal,
				    struct data_vio *data_vio)
{
	bool increment;
	int result;

	assert_on_journal_thread(journal, __func__);
	if (!vdo_is_state_normal(&journal->state)) {
		continue_data_vio(data_vio, VDO_INVALID_ADMIN_STATE);
		return;
	}

	if (vdo_is_read_only(journal->read_only_notifier)) {
		continue_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	increment = vdo_is_journal_increment_operation(data_vio->operation.type);
	ASSERT_LOG_ONLY((!increment ||
			 (data_vio->recovery_sequence_number == 0)),
			"journal lock not held for increment");

	vdo_advance_journal_point(&journal->append_point,
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
 * reap_recovery_journal() - Conduct a sweep on a recovery journal to reclaim
 *                           unreferenced blocks.
 * @journal: The recovery journal.
 */
static void reap_recovery_journal(struct recovery_journal *journal)
{
	if (journal->reaping) {
		/*
		 * We already have an outstanding reap in progress. We need to
		 * wait for it to finish.
		 */
		return;
	}

	if (vdo_is_state_quiescent(&journal->state)) {
		/* We are supposed to not do IO. Don't botch it by reaping. */
		return;
	}

	/*
	 * Start reclaiming blocks only when the journal head has no
	 * references. Then stop when a block is referenced.
	 */
	while ((journal->block_map_reap_head < journal->last_write_acknowledged)
	       && !vdo_is_lock_locked(journal->lock_counter,
				      journal->block_map_head_block_number,
				      VDO_ZONE_TYPE_LOGICAL)) {
		journal->block_map_reap_head++;
		if (++journal->block_map_head_block_number == journal->size) {
			journal->block_map_head_block_number = 0;
		}
	}

	while ((journal->slab_journal_reap_head < journal->last_write_acknowledged)
	       && !vdo_is_lock_locked(journal->lock_counter,
				      journal->slab_journal_head_block_number,
				      VDO_ZONE_TYPE_PHYSICAL)) {
		journal->slab_journal_reap_head++;
		if (++journal->slab_journal_head_block_number == journal->size) {
			journal->slab_journal_head_block_number = 0;
		}
	}

	if ((journal->block_map_reap_head == journal->block_map_head)
	    && (journal->slab_journal_reap_head == journal->slab_journal_head)) {
		/* Nothing happened. */
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
	submit_flush_vio(journal->flush_vio, flush_endio, handle_flush_error);
}

/**
 * vdo_acquire_recovery_journal_block_reference() - Acquire a reference to a
 *                                                  recovery journal block
 *                                                  from somewhere other than
 *                                                  the journal itself.
 * @journal: The recovery journal.
 * @sequence_number: The journal sequence number of the referenced block.
 * @zone_type: The type of the zone making the adjustment.
 * @zone_id: The ID of the zone making the adjustment.
 */
void vdo_acquire_recovery_journal_block_reference(struct recovery_journal *journal,
						  sequence_number_t sequence_number,
						  enum vdo_zone_type zone_type,
						  zone_count_t zone_id)
{
	block_count_t block_number;

	if (sequence_number == 0) {
		return;
	}

	block_number =
		vdo_get_recovery_journal_block_number(journal, sequence_number);
	vdo_acquire_lock_count_reference(journal->lock_counter, block_number,
					 zone_type, zone_id);
}

/**
 * vdo_release_recovery_journal_block_reference() - Release a reference to a
 * recovery journal block from somewhere other than the journal itself.
 * @journal: The recovery journal.
 * @sequence_number: The journal sequence number of the referenced block.
 * @zone_type: The type of the zone making the adjustment.
 * @zone_id: The ID of the zone making the adjustment.
 *
 * If this is the last reference for a given zone type, an attempt will be
 * made to reap the journal.
 */
void vdo_release_recovery_journal_block_reference(struct recovery_journal *journal,
						  sequence_number_t sequence_number,
						  enum vdo_zone_type zone_type,
						  zone_count_t zone_id)
{
	block_count_t block_number;

	if (sequence_number == 0) {
		return;
	}

	block_number =
		vdo_get_recovery_journal_block_number(journal, sequence_number);
	vdo_release_lock_count_reference(journal->lock_counter, block_number,
					 zone_type, zone_id);
}

/**
 * vdo_release_journal_per_entry_lock_from_other_zone() - Release a single
 *                                                        per-entry reference
 *                                                        count for a recovery
 *                                                        journal block.
 * @journal: The recovery journal.
 * @sequence_number: The journal sequence number of the referenced block.
 *
 * Context: This method may be called from any zone (but shouldn't be called
 * from the journal zone as it would be inefficient).
 */
void vdo_release_journal_per_entry_lock_from_other_zone(struct recovery_journal *journal,
							sequence_number_t sequence_number)
{
	block_count_t block_number;

	if (sequence_number == 0) {
		return;
	}

	block_number =
		vdo_get_recovery_journal_block_number(journal, sequence_number);
	vdo_release_journal_zone_reference_from_other_zone(journal->lock_counter,
							   block_number);
}

/**
 * initiate_drain() - Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state,
					      struct recovery_journal,
					      state));
}

/**
 * vdo_drain_recovery_journal() - Drain recovery journal I/O.
 * @journal: The journal to drain.
 * @operation: The drain operation (suspend or save).
 * @parent: The completion to finish once the journal is drained.
 *
 * All uncommitted entries will be written out.
 */
void vdo_drain_recovery_journal(struct recovery_journal *journal,
				const struct admin_state_code *operation,
				struct vdo_completion *parent)
{
	assert_on_journal_thread(journal, __func__);
	vdo_start_draining(&journal->state, operation, parent, initiate_drain);
}

/**
 * vdo_resume_recovery_journal() - Resume a recovery journal which has been
 *                                 drained.
 * @journal: The journal to resume.
 * @parent: The completion to finish once the journal is resumed.
 */
void vdo_resume_recovery_journal(struct recovery_journal *journal,
				 struct vdo_completion *parent)
{
	bool saved;

	assert_on_journal_thread(journal, __func__);
	saved = vdo_is_state_saved(&journal->state);
	vdo_set_completion_result(parent, vdo_resume_if_quiescent(&journal->state));

	if (vdo_is_read_only(journal->read_only_notifier)) {
		vdo_finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	if (saved) {
		initialize_journal_state(journal);
	}

	if (vdo_resume_lock_counter(journal->lock_counter)) {
		/* We might have missed a notification. */
		reap_recovery_journal(journal);
	}

	vdo_complete_completion(parent);
}

/**
 * vdo_get_recovery_journal_logical_blocks_used() - Get the number of logical
 *                                                  blocks in use by the VDO.
 * @journal: The journal.
 *
 * Return: The number of logical blocks in use by the VDO.
 */
block_count_t
vdo_get_recovery_journal_logical_blocks_used(const struct recovery_journal *journal)
{
	return journal->logical_blocks_used;
}

/**
 * vdo_get_recovery_journal_statistics() - Get the current statistics from the
 *                                         recovery journal.
 * @journal: The recovery journal to query.
 *
 * Return: A copy of the current statistics for the journal.
 */
struct recovery_journal_statistics
vdo_get_recovery_journal_statistics(const struct recovery_journal *journal)
{
	return journal->events;
}

/**
 * vdo_dump_recovery_journal_statistics() - Dump some current statistics and
 *                                          other debug info from the recovery
 *                                          journal.
 * @journal: The recovery journal to dump.
 */
void vdo_dump_recovery_journal_statistics(const struct recovery_journal *journal)
{
	const struct list_head *head;
	struct list_head *entry;

	struct recovery_journal_statistics stats =
		vdo_get_recovery_journal_statistics(journal);
	uds_log_info("Recovery Journal");
	uds_log_info("  block_map_head=%llu slab_journal_head=%llu last_write_acknowledged=%llu tail=%llu block_map_reap_head=%llu slab_journal_reap_head=%llu disk_full=%llu slab_journal_commits_requested=%llu increment_waiters=%zu decrement_waiters=%zu",
		     (unsigned long long) journal->block_map_head,
		     (unsigned long long) journal->slab_journal_head,
		     (unsigned long long) journal->last_write_acknowledged,
		     (unsigned long long) journal->tail,
		     (unsigned long long) journal->block_map_reap_head,
		     (unsigned long long) journal->slab_journal_reap_head,
		     (unsigned long long) stats.disk_full,
		     (unsigned long long) stats.slab_journal_commits_requested,
		     count_waiters(&journal->increment_waiters),
		     count_waiters(&journal->decrement_waiters));
	uds_log_info("  entries: started=%llu written=%llu committed=%llu",
		     (unsigned long long) stats.entries.started,
		     (unsigned long long) stats.entries.written,
		     (unsigned long long) stats.entries.committed);
	uds_log_info("  blocks: started=%llu written=%llu committed=%llu",
		     (unsigned long long) stats.blocks.started,
		     (unsigned long long) stats.blocks.written,
		     (unsigned long long) stats.blocks.committed);

	uds_log_info("  active blocks:");
	head = &journal->active_tail_blocks;
	list_for_each(entry, head) {
		vdo_dump_recovery_block(vdo_recovery_block_from_list_entry(entry));
	}
}
