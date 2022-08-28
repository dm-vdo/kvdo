// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab-scrubber.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "constants.h"
#include "io-submitter.h"
#include "read-only-notifier.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-journal.h"
#include "vdo.h"
#include "vio.h"

/**
 * allocate_vio_and_buffer() - Allocate the buffer and vio used for reading the
 *                             slab journal when scrubbing a slab.
 * @scrubber: The slab scrubber for which to allocate.
 * @vdo: The VDO in which the scrubber resides.
 * @slab_journal_size: The size of a slab journal.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
allocate_vio_and_buffer(struct slab_scrubber *scrubber,
			struct vdo *vdo,
			block_count_t slab_journal_size)
{
	size_t buffer_size = VDO_BLOCK_SIZE * slab_journal_size;
	int result = UDS_ALLOCATE(buffer_size, char, __func__,
				  &scrubber->journal_data);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return create_multi_block_metadata_vio(vdo,
					       VIO_TYPE_SLAB_JOURNAL,
					       VIO_PRIORITY_METADATA,
					       scrubber,
					       slab_journal_size,
					       scrubber->journal_data,
					       &scrubber->vio);
}

/**
 * vdo_make_slab_scrubber() - Create a slab scrubber.
 * @vdo: The VDO.
 * @slab_journal_size: The size of a slab journal in blocks.
 * @read_only_notifier: The context for entering read-only mode.
 * @scrubber_ptr: A pointer to hold the scrubber.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_slab_scrubber(struct vdo *vdo,
			   block_count_t slab_journal_size,
			   struct read_only_notifier *read_only_notifier,
			   struct slab_scrubber **scrubber_ptr)
{
	struct slab_scrubber *scrubber;
	int result = UDS_ALLOCATE(1, struct slab_scrubber, __func__, &scrubber);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = allocate_vio_and_buffer(scrubber, vdo, slab_journal_size);
	if (result != VDO_SUCCESS) {
		vdo_free_slab_scrubber(scrubber);
		return result;
	}

	vdo_initialize_completion(&scrubber->completion, vdo,
				  VDO_SLAB_SCRUBBER_COMPLETION);
	INIT_LIST_HEAD(&scrubber->high_priority_slabs);
	INIT_LIST_HEAD(&scrubber->slabs);
	scrubber->read_only_notifier = read_only_notifier;
	vdo_set_admin_state_code(&scrubber->admin_state,
				 VDO_ADMIN_STATE_SUSPENDED);
	*scrubber_ptr = scrubber;
	return VDO_SUCCESS;
}

/**
 * free_vio_and_buffer() - Free the vio and buffer used for reading slab
 *                         journals.
 * @scrubber: The scrubber.
 **/
static void free_vio_and_buffer(struct slab_scrubber *scrubber)
{
	free_vio(UDS_FORGET(scrubber->vio));
	UDS_FREE(UDS_FORGET(scrubber->journal_data));
}

/**
 * vdo_free_slab_scrubber() - Free a slab scrubber.
 * @scrubber: The scrubber to destroy.
 */
void vdo_free_slab_scrubber(struct slab_scrubber *scrubber)
{
	if (scrubber == NULL) {
		return;
	}

	free_vio_and_buffer(scrubber);
	UDS_FREE(scrubber);
}

/**
 * get_next_slab() - Get the next slab to scrub.
 * @scrubber: The slab scrubber.
 *
 * Return: The next slab to scrub or NULL if there are none.
 */
static struct vdo_slab *get_next_slab(struct slab_scrubber *scrubber)
{
	if (!list_empty(&scrubber->high_priority_slabs)) {
		return vdo_slab_from_list_entry(scrubber->high_priority_slabs.next);
	}

	if (!list_empty(&scrubber->slabs)) {
		return vdo_slab_from_list_entry(scrubber->slabs.next);
	}

	return NULL;
}

/**
 * has_slabs_to_scrub() - Check whether a scrubber has slabs to scrub.
 * @scrubber: The scrubber to check.
 *
 * Return: true if the scrubber has slabs to scrub.
 */
static bool __must_check has_slabs_to_scrub(struct slab_scrubber *scrubber)
{
	return (get_next_slab(scrubber) != NULL);
}

/**
 * vdo_get_scrubber_slab_count() - Get the number of slabs that are
 *                                 unrecovered or being scrubbed.
 * @scrubber: The scrubber to query.
 *
 * Return: The number of slabs that are unrecovered or being scrubbed.
 */
slab_count_t vdo_get_scrubber_slab_count(const struct slab_scrubber *scrubber)
{
	return READ_ONCE(scrubber->slab_count);
}

/**
 * vdo_register_slab_for_scrubbing() - Register a slab with a scrubber.
 * @scrubber: The scrubber.
 * @slab: The slab to scrub.
 * @high_priority: true if the slab should be put on the high-priority queue.
 */
void vdo_register_slab_for_scrubbing(struct slab_scrubber *scrubber,
				     struct vdo_slab *slab,
				     bool high_priority)
{
	ASSERT_LOG_ONLY((slab->status != VDO_SLAB_REBUILT),
			"slab to be scrubbed is unrecovered");

	if (slab->status != VDO_SLAB_REQUIRES_SCRUBBING) {
		return;
	}

	list_del_init(&slab->allocq_entry);
	if (!slab->was_queued_for_scrubbing) {
		WRITE_ONCE(scrubber->slab_count, scrubber->slab_count + 1);
		slab->was_queued_for_scrubbing = true;
	}

	if (high_priority) {
		slab->status = VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING;
		list_add_tail(&slab->allocq_entry,
			      &scrubber->high_priority_slabs);
		return;
	}

	list_add_tail(&slab->allocq_entry, &scrubber->slabs);
}

/**
 * finish_scrubbing() - Stop scrubbing, either because there are no more slabs
 *                      to scrub or because there's been an error.
 * @scrubber: The scrubber.
 */
static void finish_scrubbing(struct slab_scrubber *scrubber)
{
	bool notify;

	if (!has_slabs_to_scrub(scrubber)) {
		free_vio_and_buffer(scrubber);
	}

	/* Inform whoever is waiting that scrubbing has completed. */
	vdo_complete_completion(&scrubber->completion);

	notify = has_waiters(&scrubber->waiters);

	/*
	 * Note that the scrubber has stopped, and inform anyone who might be
	 * waiting for that to happen.
	 */
	if (!vdo_finish_draining(&scrubber->admin_state)) {
		WRITE_ONCE(scrubber->admin_state.current_state,
			   VDO_ADMIN_STATE_SUSPENDED);
	}

	/*
	 * We can't notify waiters until after we've finished draining or
	 * they'll just requeue. Fortunately if there were waiters, we can't
	 * have been freed yet.
	 */
	if (notify) {
		notify_all_waiters(&scrubber->waiters, NULL, NULL);
	}
}

static void scrub_next_slab(struct slab_scrubber *scrubber);

/**
 * slab_scrubbed() - Notify the scrubber that a slab has been scrubbed.
 * @completion: The slab rebuild completion.
 *
 * This callback is registered in apply_journal_entries().
 */
static void slab_scrubbed(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber = completion->parent;

	vdo_finish_scrubbing_slab(scrubber->slab);
	WRITE_ONCE(scrubber->slab_count, scrubber->slab_count - 1);
	scrub_next_slab(scrubber);
}

/**
 * abort_scrubbing() - Abort scrubbing due to an error.
 * @scrubber: The slab scrubber.
 * @result: The error.
 */
static void abort_scrubbing(struct slab_scrubber *scrubber, int result)
{
	vdo_enter_read_only_mode(scrubber->read_only_notifier, result);
	vdo_set_completion_result(&scrubber->completion, result);
	scrub_next_slab(scrubber);
}

/**
 * handle_scrubber_error() - Handle errors while rebuilding a slab.
 * @completion: The slab rebuild completion.
 */
static void handle_scrubber_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	abort_scrubbing(completion->parent, completion->result);
}

/**
 * apply_block_entries() - Apply all the entries in a block to the reference
 *                         counts.
 * @block: A block with entries to apply.
 * @entry_count: The number of entries to apply.
 * @block_number: The sequence number of the block.
 * @slab: The slab to apply the entries to.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int apply_block_entries(struct packed_slab_journal_block *block,
			       journal_entry_count_t entry_count,
			       sequence_number_t block_number,
			       struct vdo_slab *slab)
{
	struct journal_point entry_point = {
		.sequence_number = block_number,
		.entry_count = 0,
	};
	int result;

	slab_block_number max_sbn = slab->end - slab->start;

	while (entry_point.entry_count < entry_count) {
		struct slab_journal_entry entry =
			vdo_decode_slab_journal_entry(block,
						      entry_point.entry_count);
		if (entry.sbn > max_sbn) {
			/* This entry is out of bounds. */
			return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						      "vdo_slab journal entry (%llu, %u) had invalid offset %u in slab (size %u blocks)",
						      (unsigned long long) block_number,
						      entry_point.entry_count,
						      entry.sbn,
						      max_sbn);
		}

		result = vdo_replay_reference_count_change(slab->reference_counts,
							   &entry_point, entry);
		if (result != VDO_SUCCESS) {
			uds_log_error_strerror(result,
					       "vdo_slab journal entry (%llu, %u) (%s of offset %u) could not be applied in slab %u",
					       (unsigned long long) block_number,
					       entry_point.entry_count,
					       vdo_get_journal_operation_name(entry.operation),
					       entry.sbn,
					       slab->slab_number);
			return result;
		}
		entry_point.entry_count++;
	}

	return VDO_SUCCESS;
}

/**
 * apply_journal_entries() - Find the relevant vio of the slab journal and
 *                           apply all valid entries.
 * @completion: The metadata read vio completion.
 *
 * This is a callback registered in start_scrubbing().
 */
static void apply_journal_entries(struct vdo_completion *completion)
{
	int result;
	struct slab_scrubber *scrubber = completion->parent;
	struct vdo_slab *slab = scrubber->slab;
	struct slab_journal *journal = slab->journal;
	struct ref_counts *reference_counts = slab->reference_counts;

	/* Find the boundaries of the useful part of the journal. */
	sequence_number_t tail = journal->tail;
	tail_block_offset_t end_index =
		vdo_get_slab_journal_block_offset(journal, tail - 1);
	char *end_data = scrubber->journal_data + (end_index * VDO_BLOCK_SIZE);
	struct packed_slab_journal_block *end_block =
		(struct packed_slab_journal_block *) end_data;

	sequence_number_t head = __le64_to_cpu(end_block->header.head);
	tail_block_offset_t head_index =
		vdo_get_slab_journal_block_offset(journal, head);
	block_count_t index = head_index;

	struct journal_point ref_counts_point =
		reference_counts->slab_journal_point;
	struct journal_point last_entry_applied = ref_counts_point;
	sequence_number_t sequence;

	for (sequence = head; sequence < tail; sequence++) {
		char *block_data =
			scrubber->journal_data + (index * VDO_BLOCK_SIZE);
		struct packed_slab_journal_block *block =
			(struct packed_slab_journal_block *) block_data;
		struct slab_journal_block_header header;

		vdo_unpack_slab_journal_block_header(&block->header, &header);

		if ((header.nonce != slab->allocator->nonce) ||
		    (header.metadata_type != VDO_METADATA_SLAB_JOURNAL) ||
		    (header.sequence_number != sequence) ||
		    (header.entry_count > journal->entries_per_block) ||
		    (header.has_block_map_increments &&
		     (header.entry_count > journal->full_entries_per_block))) {
			/* The block is not what we expect it to be. */
			uds_log_error("vdo_slab journal block for slab %u was invalid",
				      slab->slab_number);
			abort_scrubbing(scrubber, VDO_CORRUPT_JOURNAL);
			return;
		}

		result = apply_block_entries(block, header.entry_count,
					     sequence, slab);
		if (result != VDO_SUCCESS) {
			abort_scrubbing(scrubber, result);
			return;
		}

		last_entry_applied.sequence_number = sequence;
		last_entry_applied.entry_count = header.entry_count - 1;
		index++;
		if (index == journal->size) {
			index = 0;
		}
	}

	/*
	 * At the end of rebuild, the ref_counts should be accurate to the end
	 * of the journal we just applied.
	 */
	result = ASSERT(!vdo_before_journal_point(&last_entry_applied,
						  &ref_counts_point),
			"Refcounts are not more accurate than the slab journal");
	if (result != VDO_SUCCESS) {
		abort_scrubbing(scrubber, result);
		return;
	}

	/* Save out the rebuilt reference blocks. */
	vdo_prepare_completion(completion,
			       slab_scrubbed,
			       handle_scrubber_error,
			       completion->callback_thread_id,
			       scrubber);
	vdo_start_slab_action(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING,
			      completion);
}

static void read_slab_journal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct slab_scrubber *scrubber = vio->completion.parent;

	continue_vio_after_io(bio->bi_private,
			      apply_journal_entries,
			      scrubber->completion.callback_thread_id);
}

/**
 * start_scrubbing() - Read the current slab's journal from disk now that it
 *                     has been flushed.
 * @completion: The scrubber's vio completion.
 *
 * This callback is registered in scrub_next_slab().
 */
static void start_scrubbing(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber = completion->parent;
	struct vdo_slab *slab = scrubber->slab;

	if (vdo_get_summarized_cleanliness(slab->allocator->summary,
					   slab->slab_number)) {
		slab_scrubbed(completion);
		return;
	}

	submit_metadata_vio(scrubber->vio,
			    slab->journal_origin,
			    read_slab_journal_endio,
			    handle_scrubber_error,
			    REQ_OP_READ);
}

/**
 * scrub_next_slab() - Scrub the next slab if there is one.
 * @scrubber: The scrubber.
 */
static void scrub_next_slab(struct slab_scrubber *scrubber)
{
	struct vdo_completion *completion;
	struct vdo_slab *slab;

	/*
	 * Note: this notify call is always safe only because scrubbing can
	 * only be started when the VDO is quiescent.
	 */
	notify_all_waiters(&scrubber->waiters, NULL, NULL);
	if (vdo_is_read_only(scrubber->read_only_notifier)) {
		vdo_set_completion_result(&scrubber->completion, VDO_READ_ONLY);
		finish_scrubbing(scrubber);
		return;
	}

	slab = get_next_slab(scrubber);
	if ((slab == NULL) || (scrubber->high_priority_only &&
			       list_empty(&scrubber->high_priority_slabs))) {
		scrubber->high_priority_only = false;
		finish_scrubbing(scrubber);
		return;
	}

	if (vdo_finish_draining(&scrubber->admin_state)) {
		return;
	}

	list_del_init(&slab->allocq_entry);
	scrubber->slab = slab;
	completion = vio_as_completion(scrubber->vio);
	vdo_prepare_completion(completion,
			       start_scrubbing,
			       handle_scrubber_error,
			       scrubber->completion.callback_thread_id,
			       scrubber);
	vdo_start_slab_action(slab, VDO_ADMIN_STATE_SCRUBBING, completion);
}

/**
 * vdo_scrub_slabs() - Scrub all the slabs which have been registered with a
 *                     slab scrubber.
 * @scrubber: The scrubber.
 * @parent: The object to notify when scrubbing is complete.
 * @callback: The function to run when scrubbing is complete.
 * @error_handler: The handler for scrubbing errors.
 */
void vdo_scrub_slabs(struct slab_scrubber *scrubber,
		     void *parent,
		     vdo_action *callback,
		     vdo_action *error_handler)
{
	thread_id_t thread_id = vdo_get_callback_thread_id();

	vdo_resume_if_quiescent(&scrubber->admin_state);
	vdo_prepare_completion(&scrubber->completion,
			       callback,
			       error_handler,
			       thread_id,
			       parent);
	if (!has_slabs_to_scrub(scrubber)) {
		finish_scrubbing(scrubber);
		return;
	}

	scrub_next_slab(scrubber);
}

/**
 * vdo_scrub_high_priority_slabs() - Scrub any slabs which have been
 *                                   registered at high priority with a slab
 *                                   scrubber.
 * @scrubber: The scrubber.
 * @scrub_at_least_one: true if one slab should always be scrubbed, even if
 *                      there are no high-priority slabs (and there is at
 *                      least one low priority slab).
 * @parent: The completion to notify when scrubbing is complete.
 * @callback: The function to run when scrubbing is complete.
 * @error_handler: The handler for scrubbing errors.
 */
void vdo_scrub_high_priority_slabs(struct slab_scrubber *scrubber,
				   bool scrub_at_least_one,
				   struct vdo_completion *parent,
				   vdo_action *callback,
				   vdo_action *error_handler)
{
	if (scrub_at_least_one && list_empty(&scrubber->high_priority_slabs)) {
		struct vdo_slab *slab = get_next_slab(scrubber);

		if (slab != NULL) {
			vdo_register_slab_for_scrubbing(scrubber, slab, true);
		}
	}
	scrubber->high_priority_only = true;
	vdo_scrub_slabs(scrubber, parent, callback, error_handler);
}

/**
 * vdo_stop_slab_scrubbing() - Tell the scrubber to stop scrubbing after it
 *                             finishes the slab it is currently working on.
 * @scrubber: The scrubber to stop.
 * @parent: The completion to notify when scrubbing has stopped.
 */
void vdo_stop_slab_scrubbing(struct slab_scrubber *scrubber,
			     struct vdo_completion *parent)
{
	if (vdo_is_state_quiescent(&scrubber->admin_state)) {
		vdo_complete_completion(parent);
	} else {
		vdo_start_draining(&scrubber->admin_state,
				   VDO_ADMIN_STATE_SUSPENDING,
				   parent,
				   NULL);
	}
}

/**
 * vdo_resume_slab_scrubbing() - Tell the scrubber to resume scrubbing if it
 *                               has been stopped.
 * @scrubber: The scrubber to resume.
 * @parent: The object to notify once scrubbing has resumed.
 */
void vdo_resume_slab_scrubbing(struct slab_scrubber *scrubber,
			       struct vdo_completion *parent)
{
	int result;

	if (!has_slabs_to_scrub(scrubber)) {
		vdo_complete_completion(parent);
		return;
	}

	result = vdo_resume_if_quiescent(&scrubber->admin_state);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	scrub_next_slab(scrubber);
	vdo_complete_completion(parent);
}

/**
 * vdo_enqueue_clean_slab_waiter() - Wait for a clean slab.
 * @scrubber: The scrubber on which to wait.
 * @waiter: The waiter.
 *
 * Return: VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise.
 */
int vdo_enqueue_clean_slab_waiter(struct slab_scrubber *scrubber,
				  struct waiter *waiter)
{
	if (vdo_is_read_only(scrubber->read_only_notifier)) {
		return VDO_READ_ONLY;
	}

	if (vdo_is_state_quiescent(&scrubber->admin_state)) {
		return VDO_NO_SPACE;
	}

	return enqueue_waiter(&scrubber->waiters, waiter);
}

/**
 * vdo_dump_slab_scrubber() - Dump information about a slab scrubber to the
 *                            log for debugging.
 * @scrubber: The scrubber to dump.
 */
void vdo_dump_slab_scrubber(const struct slab_scrubber *scrubber)
{
	uds_log_info("slab_scrubber slab_count %u waiters %zu %s%s",
		     vdo_get_scrubber_slab_count(scrubber),
		     count_waiters(&scrubber->waiters),
		     vdo_get_admin_state_code(&scrubber->admin_state)->name,
		     scrubber->high_priority_only ? ", high_priority_only " : "");
}
