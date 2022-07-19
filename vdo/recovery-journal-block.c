// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "recovery-journal-block.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "data-vio.h"
#include "io-submitter.h"
#include "packed-recovery-journal-block.h"
#include "recovery-journal-entry.h"
#include "recovery-journal.h"
#include "vdo-layout.h"
#include "vio.h"
#include "wait-queue.h"

/**
 * vdo_make_recovery_block() - Construct a journal block.
 * @vdo: The vdo from which to construct vios.
 * @journal: The journal to which the block will belong.
 * @block_ptr: A pointer to receive the new block.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_recovery_block(struct vdo *vdo,
			    struct recovery_journal *journal,
			    struct recovery_journal_block **block_ptr)
{
	struct recovery_journal_block *block;
	int result;

	/*
	 * Ensure that a block is large enough to store
	 * RECOVERY_JOURNAL_ENTRIES_PER_BLOCK entries.
	 */
	STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
		      <= ((VDO_BLOCK_SIZE -
			   sizeof(struct packed_journal_header)) /
			  sizeof(struct packed_recovery_journal_entry)));

	result = UDS_ALLOCATE(1, struct recovery_journal_block, __func__, &block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * Allocate a full block for the journal block even though not all of
	 * the space is used since the VIO needs to write a full disk block.
	 */
	result = UDS_ALLOCATE(VDO_BLOCK_SIZE, char, "PackedJournalBlock",
			      &block->block);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_block(block);
		return result;
	}

	result = create_metadata_vio(vdo,
				     VIO_TYPE_RECOVERY_JOURNAL,
				     VIO_PRIORITY_HIGH,
				     block,
				     block->block,
				     &block->vio);
	if (result != VDO_SUCCESS) {
		vdo_free_recovery_block(block);
		return result;
	}

	block->vio->completion.callback_thread_id = journal->thread_id;
	INIT_LIST_HEAD(&block->list_node);
	block->journal = journal;

	*block_ptr = block;
	return VDO_SUCCESS;
}

/**
 * vdo_free_recovery_block() - Free a tail block.
 * @block: The tail block to free.
 */
void vdo_free_recovery_block(struct recovery_journal_block *block)
{
	if (block == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(block->block));
	free_vio(UDS_FORGET(block->vio));
	UDS_FREE(block);
}

/**
 * get_block_header() - Get a pointer to the packed journal block
 *                      header in the block buffer.
 * @block: The recovery block.
 *
 * Return: The block's header.
 */
static inline struct packed_journal_header *
get_block_header(const struct recovery_journal_block *block)
{
	return (struct packed_journal_header *) block->block;
}

/**
 * set_active_sector() - Set the current sector of the current block
 *                       and initialize it.
 * @block: The block to update.
 * @sector: A pointer to the first byte of the new sector.
 */
static void set_active_sector(struct recovery_journal_block *block,
			      void *sector)
{
	block->sector = (struct packed_journal_sector *) sector;
	block->sector->check_byte = get_block_header(block)->check_byte;
	block->sector->recovery_count = block->journal->recovery_count;
	block->sector->entry_count = 0;
}

/**
 * vdo_initialize_recovery_block() - Initialize the next active
 *                                   recovery journal block.
 * @block: The journal block to initialize.
 */
void vdo_initialize_recovery_block(struct recovery_journal_block *block)
{
	struct recovery_journal *journal = block->journal;
	struct recovery_block_header unpacked = {
		.metadata_type = VDO_METADATA_RECOVERY_JOURNAL,
		.block_map_data_blocks = journal->block_map_data_blocks,
		.logical_blocks_used = journal->logical_blocks_used,
		.nonce = journal->nonce,
		.recovery_count = journal->recovery_count,
		.sequence_number = journal->tail,
		.check_byte = vdo_compute_recovery_journal_check_byte(journal,
								      journal->tail),
	};
	struct packed_journal_header *header = get_block_header(block);

	memset(block->block, 0x0, VDO_BLOCK_SIZE);
	block->sequence_number = journal->tail;
	block->entry_count = 0;
	block->uncommitted_entry_count = 0;

	block->block_number =
		vdo_get_recovery_journal_block_number(journal, journal->tail);

	vdo_pack_recovery_block_header(&unpacked, header);

	set_active_sector(block, vdo_get_journal_block_sector(header, 1));
}

/**
 * vdo_enqueue_recovery_block_entry() - Enqueue a data_vio to
 *                                      asynchronously encode and
 *                                      commit its next recovery
 *                                      journal entry in this block.
 * @block: The journal block in which to make an entry.
 * @data_vio: The data_vio to enqueue.
 *
 * The data_vio will not be continued until the entry is committed to
 * the on-disk journal. The caller is responsible for ensuring the
 * block is not already full.
 *
 * Return: VDO_SUCCESS or an error code if the data_vio could not be enqueued.
 */
int vdo_enqueue_recovery_block_entry(struct recovery_journal_block *block,
				     struct data_vio *data_vio)
{
	/*
	 * First queued entry indicates this is a journal block we've just
	 * opened or a committing block we're extending and will have to write
	 * again.
	 */
	bool new_batch = !has_waiters(&block->entry_waiters);

	/* Enqueue the data_vio to wait for its entry to commit. */
	int result = enqueue_data_vio(&block->entry_waiters, data_vio);

	if (result != VDO_SUCCESS) {
		return result;
	}

	block->entry_count++;
	block->uncommitted_entry_count++;

	/* Update stats to reflect the journal entry we're going to write. */
	if (new_batch) {
		block->journal->events.blocks.started++;
	}
	block->journal->events.entries.started++;

	return VDO_SUCCESS;
}

/**
 * is_sector_full() - * Check whether the current sector of a block is full.
 * @block: The block to check.
 *
 * Return: true if the sector is full.
 */
static bool __must_check
is_sector_full(const struct recovery_journal_block *block)
{
	return (block->sector->entry_count ==
		RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
}

/**
 * add_queued_recovery_entries() - Actually add entries from the queue to the
 *                                 given block.
 * @block: The journal block.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check
add_queued_recovery_entries(struct recovery_journal_block *block)
{
	while (has_waiters(&block->entry_waiters)) {
		struct data_vio *data_vio =
			waiter_as_data_vio(dequeue_next_waiter(&block->entry_waiters));
		struct tree_lock *lock = &data_vio->tree_lock;
		struct packed_recovery_journal_entry *packed_entry;
		struct recovery_journal_entry new_entry;
		int result;

		if (data_vio->operation.type == VDO_JOURNAL_DATA_INCREMENT) {
			/*
			 * In order to not lose an acknowledged write with the
			 * FUA flag, we must also set the FUA flag on the
			 * journal entry write.
			 */
			block->has_fua_entry =
				(block->has_fua_entry ||
				 data_vio_requires_fua(data_vio));
		}

		/* Compose and encode the entry. */
		packed_entry =
			&block->sector->entries[block->sector->entry_count++];
		new_entry = (struct recovery_journal_entry) {
			.mapping = {
					.pbn = data_vio->operation.pbn,
					.state = data_vio->operation.state,
				},
			.operation = data_vio->operation.type,
			.slot = lock->tree_slots[lock->height].block_map_slot,
		};
		*packed_entry = vdo_pack_recovery_journal_entry(&new_entry);

		if (vdo_is_journal_increment_operation(data_vio->operation.type)) {
			data_vio->recovery_sequence_number =
				block->sequence_number;
		}

		/* Enqueue the data_vio to wait for its entry to commit. */
		result = enqueue_data_vio(&block->commit_waiters, data_vio);
		if (result != VDO_SUCCESS) {
			continue_data_vio(data_vio, result);
			return result;
		}

		if (is_sector_full(block)) {
			set_active_sector(block, (char *) block->sector
						       + VDO_SECTOR_SIZE);
		}
	}

	return VDO_SUCCESS;
}

static int __must_check
get_recovery_block_pbn(struct recovery_journal_block *block,
		       physical_block_number_t *pbn_ptr)
{
	struct recovery_journal *journal = block->journal;
	int result = vdo_translate_to_pbn(journal->partition,
					  block->block_number,
					  pbn_ptr);
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result,
				       "Error translating recovery journal block number %llu",
				       (unsigned long long) block->block_number);
	}
	return result;
}

/**
 * vdo_can_commit_recovery_block() - Check whether a journal block can be
 *                                   committed.
 * @block: The journal block in question.
 *
 * Return: true if the block can be committed now.
 */
bool vdo_can_commit_recovery_block(struct recovery_journal_block *block)
{
	/*
	 * Cannot commit in read-only mode, if already committing the block,
	 * or if there are no entries to commit.
	 */
	return ((block != NULL)
		&& !block->committing
		&& has_waiters(&block->entry_waiters)
		&& !vdo_is_read_only(block->journal->read_only_notifier));
}

/**
 * vdo_commit_recovery_block() - Attempt to commit a block.
 * @block: The block to write.
 * @callback: The function to call when the write completes.
 * @error_handler: The handler for flush or write errors.
 *
 * If the block is not the oldest block with uncommitted entries or if it is
 * already being committed, nothing will be done.
 *
 * Return: VDO_SUCCESS, or an error if the write could not be launched.
 */
int vdo_commit_recovery_block(struct recovery_journal_block *block,
			      bio_end_io_t callback,
			      vdo_action *error_handler)
{
	int result;
	physical_block_number_t block_pbn;
	struct recovery_journal *journal = block->journal;
	struct packed_journal_header *header = get_block_header(block);
	unsigned int operation =
		REQ_OP_WRITE | REQ_PRIO | REQ_PREFLUSH | REQ_SYNC;

	result = ASSERT(vdo_can_commit_recovery_block(block),
			"block can commit in %s",
			__func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = get_recovery_block_pbn(block, &block_pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	block->entries_in_commit = count_waiters(&block->entry_waiters);
	result = add_queued_recovery_entries(block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/* Update stats to reflect the block and entries we're about to write. */
	journal->pending_write_count += 1;
	journal->events.blocks.written += 1;
	journal->events.entries.written += block->entries_in_commit;

	header->block_map_head = __cpu_to_le64(journal->block_map_head);
	header->slab_journal_head = __cpu_to_le64(journal->slab_journal_head);
	header->entry_count = __cpu_to_le16(block->entry_count);

	block->committing = true;

	/*
	 * We must issue a flush for every commit. For increments, it is
	 * necessary to ensure that the data being referenced is stable. For
	 * decrements, it is necessary to ensure that the preceding increment
	 * entry is stable before allowing overwrites of the lbn's previous
	 * data. For writes which had the FUA flag set, we must also set the
	 * FUA flag on the journal write.
	 */
	if (block->has_fua_entry) {
		block->has_fua_entry = false;
		operation |= REQ_FUA;
	}
	submit_metadata_vio(block->vio,
			    block_pbn,
			    callback,
			    error_handler,
			    operation);
	return VDO_SUCCESS;
}

/**
 * vdo_dump_recovery_block() - Dump the contents of the recovery block to the
 *                             log.
 * @block: The block to dump.
 */
void vdo_dump_recovery_block(const struct recovery_journal_block *block)
{
	uds_log_info("    sequence number %llu; entries %u; %s; %zu entry waiters; %zu commit waiters",
		     (unsigned long long) block->sequence_number,
		     block->entry_count,
		     (block->committing ? "committing" : "waiting"),
		     count_waiters(&block->entry_waiters),
		     count_waiters(&block->commit_waiters));
}
