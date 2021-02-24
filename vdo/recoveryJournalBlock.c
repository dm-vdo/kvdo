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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalBlock.c#48 $
 */

#include "recoveryJournalBlock.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "dataVIO.h"
#include "fixedLayout.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
#include "vio.h"
#include "waitQueue.h"

/**********************************************************************/
int make_recovery_block(struct vdo *vdo,
			struct recovery_journal *journal,
			struct recovery_journal_block **block_ptr)
{
	struct recovery_journal_block *block;
	int result;

	// Ensure that a block is large enough to store
	// RECOVERY_JOURNAL_ENTRIES_PER_BLOCK entries.
	STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
		      <= ((VDO_BLOCK_SIZE
		      	    - sizeof(struct packed_journal_header))
			  / sizeof(struct packed_recovery_journal_entry)));

	result = ALLOCATE(1, struct recovery_journal_block, __func__, &block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Allocate a full block for the journal block even though not all of
	// the space is used since the VIO needs to write a full disk block.
	result = ALLOCATE(VDO_BLOCK_SIZE, char, "PackedJournalBlock",
			  &block->block);
	if (result != VDO_SUCCESS) {
		free_recovery_block(&block);
		return result;
	}

	result = create_metadata_vio(vdo,
				     VIO_TYPE_RECOVERY_JOURNAL,
				     VIO_PRIORITY_HIGH,
				     block,
				     block->block,
				     &block->vio);
	if (result != VDO_SUCCESS) {
		free_recovery_block(&block);
		return result;
	}

	block->vio->completion.callback_thread_id = journal->thread_id;
	INIT_LIST_HEAD(&block->list_node);
	block->journal = journal;

	*block_ptr = block;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_recovery_block(struct recovery_journal_block **block_ptr)
{
	struct recovery_journal_block *block = *block_ptr;
	if (block == NULL) {
		return;
	}

	FREE(block->block);
	free_vio(&block->vio);
	FREE(block);
	*block_ptr = NULL;
}

/**
 * Get a pointer to the packed journal block header in the block buffer.
 *
 * @param block  The recovery block
 *
 * @return The block's header
 **/
static inline struct packed_journal_header *
get_block_header(const struct recovery_journal_block *block)
{
	return (struct packed_journal_header *) block->block;
}

/**
 * Set the current sector of the current block and initialize it.
 *
 * @param block  The block to update
 * @param sector A pointer to the first byte of the new sector
 **/
static void set_active_sector(struct recovery_journal_block *block,
			      void *sector)
{
	block->sector = (struct packed_journal_sector *) sector;
	block->sector->check_byte = get_block_header(block)->check_byte;
	block->sector->recovery_count = block->journal->recovery_count;
	block->sector->entry_count = 0;
}

/**********************************************************************/
void initialize_recovery_block(struct recovery_journal_block *block)
{
	struct recovery_journal *journal = block->journal;
	struct recovery_block_header unpacked = {
		.metadata_type = VDO_METADATA_RECOVERY_JOURNAL,
		.block_map_data_blocks = journal->block_map_data_blocks,
		.logical_blocks_used = journal->logical_blocks_used,
		.nonce = journal->nonce,
		.recovery_count = journal->recovery_count,
		.sequence_number = journal->tail,
		.check_byte = compute_recovery_check_byte(journal,
							  journal->tail),
	};
	struct packed_journal_header *header = get_block_header(block);

	memset(block->block, 0x0, VDO_BLOCK_SIZE);
	block->sequence_number = journal->tail;
	block->entry_count = 0;
	block->uncommitted_entry_count = 0;

	block->block_number =
		get_recovery_journal_block_number(journal, journal->tail);

	pack_recovery_block_header(&unpacked, header);

	set_active_sector(block, get_journal_block_sector(header, 1));
}

/**********************************************************************/
int enqueue_recovery_block_entry(struct recovery_journal_block *block,
				 struct data_vio *data_vio)
{
	// First queued entry indicates this is a journal block we've just
	// opened or a committing block we're extending and will have to write
	// again.
	bool new_batch = !has_waiters(&block->entry_waiters);

	// Enqueue the data_vio to wait for its entry to commit.
	int result = enqueue_data_vio(&block->entry_waiters, data_vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	block->entry_count++;
	block->uncommitted_entry_count++;

	// Update stats to reflect the journal entry we're going to write.
	if (new_batch) {
		block->journal->events.blocks.started++;
	}
	block->journal->events.entries.started++;

	return VDO_SUCCESS;
}

/**
 * Check whether the current sector of a block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the sector is full
 **/
static bool __must_check
is_sector_full(const struct recovery_journal_block *block)
{
	return (block->sector->entry_count ==
		RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
}

/**
 * Actually add entries from the queue to the given block.
 *
 * @param block  The journal block
 *
 * @return VDO_SUCCESS or an error code
 **/
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

		if (data_vio->operation.type == DATA_INCREMENT) {
			// In order to not lose committed sectors of this
			// partial write, we must flush before the partial write
			// entries are committed.
			block->has_partial_write_entry =
				(block->has_partial_write_entry
				 || data_vio->is_partial_write);
			/*
			 * In order to not lose acknowledged writes with the FUA
			 * flag set, we must issue a flush to cover the data
			 * write and also all previous journal writes, and we
			 * must issue a FUA on the journal write.
			 */
			block->has_fua_entry =
				(block->has_fua_entry ||
				  vio_requires_flush_after(data_vio_as_vio(data_vio)));
		}

		// Compose and encode the entry.
		packed_entry =
			&block->sector->entries[block->sector->entry_count++];
		new_entry = (struct recovery_journal_entry) {
			.mapping =
				{
					.pbn = data_vio->operation.pbn,
					.state = data_vio->operation.state,
				},
			.operation = data_vio->operation.type,
			.slot = lock->tree_slots[lock->height].block_map_slot,
		};
		*packed_entry = pack_recovery_journal_entry(&new_entry);

		if (is_increment_operation(data_vio->operation.type)) {
			data_vio->recovery_sequence_number =
				block->sequence_number;
		}

		// Enqueue the data_vio to wait for its entry to commit.
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

/**********************************************************************/
static int __must_check
get_recovery_block_pbn(struct recovery_journal_block *block,
		       physical_block_number_t *pbn_ptr)
{
	struct recovery_journal *journal = block->journal;
	int result = translate_to_pbn(journal->partition, block->block_number,
				      pbn_ptr);
	if (result != VDO_SUCCESS) {
		log_error_strerror(result,
				   "Error translating recovery journal block number %llu",
				   block->block_number);
	}
	return result;
}

/**********************************************************************/
bool can_commit_recovery_block(struct recovery_journal_block *block)
{
	// Cannot commit in read-only mode, if already committing the block,
	// or if there are no entries to commit.
	return ((block != NULL)
		&& !block->committing
		&& has_waiters(&block->entry_waiters)
		&& !is_read_only(block->journal->read_only_notifier));
}

/**********************************************************************/
int commit_recovery_block(struct recovery_journal_block *block,
			  vdo_action *callback,
			  vdo_action *error_handler)
{
	struct recovery_journal *journal = block->journal;
	struct packed_journal_header *header = get_block_header(block);
	physical_block_number_t block_pbn;
	int result = ASSERT(can_commit_recovery_block(block),
			    "should never call %s when the block can't be committed",
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

	// Update stats to reflect the block and entries we're about to write.
	journal->pending_write_count += 1;
	journal->events.blocks.written += 1;
	journal->events.entries.written += block->entries_in_commit;

	header->block_map_head = __cpu_to_le64(journal->block_map_head);
	header->slab_journal_head = __cpu_to_le64(journal->slab_journal_head);
	header->entry_count = __cpu_to_le16(block->entry_count);

	block->committing = true;

	/*
	 * When we are writing an increment entry for a request with FUA, or
	 * when making the increment entry for a partial write, we need to make
	 * sure all the data being mapped to by this block is stable on disk
	 * and also that the recovery journal is stable up to the current
	 * block, so we must flush before writing.
	 *
	 * For FUA, we also need to make sure that the write we are doing is
	 * stable, so we issue the write with FUA.
	 */
	block->has_fua_entry = false;
	block->has_partial_write_entry = false;
	launch_write_metadata_vio_with_flush(block->vio,
					     block_pbn,
					     callback,
					     error_handler,
					     true,
					     block->has_fua_entry);
	return VDO_SUCCESS;
}

/**********************************************************************/
void dump_recovery_block(const struct recovery_journal_block *block)
{
	log_info("    sequence number %llu; entries %u; %s; %zu entry waiters; %zu commit waiters",
		 block->sequence_number, block->entry_count,
		 (block->committing ? "committing" : "waiting"),
		 count_waiters(&block->entry_waiters),
		 count_waiters(&block->commit_waiters));
}
