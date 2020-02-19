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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalBlock.c#15 $
 */

#include "recoveryJournalBlock.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "dataVIO.h"
#include "fixedLayout.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
#include "ringNode.h"
#include "vio.h"
#include "waitQueue.h"

/**********************************************************************/
int make_recovery_block(PhysicalLayer *layer, struct recovery_journal *journal,
			struct recovery_journal_block **block_ptr)
{
	// Ensure that a block is large enough to store
	// RECOVERY_JOURNAL_ENTRIES_PER_BLOCK entries.
	STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
		      <= ((VDO_BLOCK_SIZE - sizeof(PackedJournalHeader))
			  / sizeof(packed_recovery_journal_entry)));

	struct recovery_journal_block *block;
	int result =
		ALLOCATE(1, struct recovery_journal_block, __func__, &block);
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

	result = createVIO(layer, VIO_TYPE_RECOVERY_JOURNAL, VIO_PRIORITY_HIGH,
			   block, block->block, &block->vio);
	if (result != VDO_SUCCESS) {
		free_recovery_block(&block);
		return result;
	}

	block->vio->completion.callbackThreadID = journal->thread_id;
	initializeRing(&block->ring_node);
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
	freeVIO(&block->vio);
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
static inline PackedJournalHeader *
get_block_header(const struct recovery_journal_block *block)
{
	return (PackedJournalHeader *) block->block;
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
	block->sector->checkByte = get_block_header(block)->fields.checkByte;
	block->sector->recoveryCount = block->journal->recovery_count;
	block->sector->entryCount = 0;
}

/**********************************************************************/
void initialize_recovery_block(struct recovery_journal_block *block)
{
	memset(block->block, 0x0, VDO_BLOCK_SIZE);

	struct recovery_journal *journal = block->journal;
	block->sequence_number = journal->tail;
	block->entry_count = 0;
	block->uncommitted_entry_count = 0;

	block->block_number =
		get_recovery_journal_block_number(journal, journal->tail);

	struct recovery_block_header unpacked = {
		.metadataType = VDO_METADATA_RECOVERY_JOURNAL,
		.blockMapDataBlocks = journal->block_map_data_blocks,
		.logicalBlocksUsed = journal->logical_blocks_used,
		.nonce = journal->nonce,
		.recoveryCount = journal->recovery_count,
		.sequenceNumber = journal->tail,
		.checkByte = compute_recovery_check_byte(journal,
							 journal->tail),
	};
	PackedJournalHeader *header = get_block_header(block);
	packRecoveryBlockHeader(&unpacked, header);

	set_active_sector(block, getJournalBlockSector(header, 1));
}

/**********************************************************************/
int enqueue_recovery_block_entry(struct recovery_journal_block *block,
				 struct data_vio *data_vio)
{
	// First queued entry indicates this is a journal block we've just
	// opened or a committing block we're extending and will have to write
	// again.
	bool new_batch = !hasWaiters(&block->entry_waiters);

	// Enqueue the data_vio to wait for its entry to commit.
	int result = enqueueDataVIO(&block->entry_waiters, data_vio,
				    THIS_LOCATION("$F($j-$js)"));
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
__attribute__((warn_unused_result)) static bool
is_sector_full(const struct recovery_journal_block *block)
{
	return (block->sector->entryCount
		== RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
}

/**
 * Actually add entries from the queue to the given block.
 *
 * @param block  The journal block
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result)) static int
add_queued_recovery_entries(struct recovery_journal_block *block)
{
	while (hasWaiters(&block->entry_waiters)) {
		struct data_vio *data_vio = waiterAsDataVIO(dequeueNextWaiter(&block->entry_waiters));
		if (data_vio->operation.type == DATA_INCREMENT) {
			// In order to not lose committed sectors of this
			// partial write, we must flush before the partial write
			// entries are committed.
			block->has_partial_write_entry =
				(block->has_partial_write_entry
				 || data_vio->isPartialWrite);
			/*
			 * In order to not lose acknowledged writes with the FUA
			 * flag set, we must issue a flush to cover the data
			 * write and also all previous journal writes, and we
			 * must issue a FUA on the journal write.
			 */
			block->has_fua_entry = (block->has_fua_entry
						|| vioRequiresFlushAfter(dataVIOAsVIO(data_vio)));
		}

		// Compose and encode the entry.
		packed_recovery_journal_entry *packed_entry =
			&block->sector->entries[block->sector->entryCount++];
		struct tree_lock *lock = &data_vio->treeLock;
		struct recovery_journal_entry new_entry = {
			.mapping =
				{
					.pbn = data_vio->operation.pbn,
					.state = data_vio->operation.state,
				},
			.operation = data_vio->operation.type,
			.slot = lock->treeSlots[lock->height].blockMapSlot,
		};
		*packed_entry = pack_recovery_journal_entry(&new_entry);

		if (is_increment_operation(data_vio->operation.type)) {
			data_vio->recoverySequenceNumber =
				block->sequence_number;
		}

		// Enqueue the data_vio to wait for its entry to commit.
		int result = enqueueDataVIO(&block->commit_waiters, data_vio,
					    THIS_LOCATION("$F($j-$js)"));
		if (result != VDO_SUCCESS) {
			continueDataVIO(data_vio, result);
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
__attribute__((warn_unused_result)) static int
get_recovery_block_pbn(struct recovery_journal_block *block,
		       PhysicalBlockNumber *pbn_ptr)
{
	struct recovery_journal *journal = block->journal;
	int result = translate_to_pbn(journal->partition, block->block_number,
				      pbn_ptr);
	if (result != VDO_SUCCESS) {
		logErrorWithStringError(result,
					"Error translating recovery journal block "
					"number %llu",
					block->block_number);
	}
	return result;
}

/**
 * Check whether a journal block should be committed.
 *
 * @param block  The journal block in question
 *
 * @return <code>true</code> if the block should be committed now
 **/
static bool should_commit(struct recovery_journal_block *block)
{
	// Never commit in read-only mode, if already committing the block, or
	// if there are no entries to commit.
	if ((block == NULL) || block->committing
	    || !hasWaiters(&block->entry_waiters)
	    || is_read_only(block->journal->read_only_notifier)) {
		return false;
	}

	// Always commit filled journal blocks.
	if (is_recovery_block_full(block)) {
		return true;
	}

	/*
	 * We want to commit any journal blocks that have VIOs waiting on them,
	 * but we'd also like to accumulate entries instead of always writing a
	 * journal block immediately after the first entry is added. If there
	 * are any pending journal writes, we can safely defer committing this
	 * partial journal block until the last pending write completes, using
	 * the last write's completion as a flush/wake-up.
	 */
	return (block->journal->pending_write_count == 0);
}

/**********************************************************************/
int commit_recovery_block(struct recovery_journal_block *block,
			  VDOAction *callback, VDOAction *error_handler)
{
	if (!should_commit(block)) {
		return VDO_SUCCESS;
	}

	PhysicalBlockNumber block_pbn;
	int result = get_recovery_block_pbn(block, &block_pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	block->entries_in_commit = countWaiters(&block->entry_waiters);
	result = add_queued_recovery_entries(block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct recovery_journal *journal = block->journal;
	PackedJournalHeader *header = get_block_header(block);

	// Update stats to reflect the block and entries we're about to write.
	journal->pending_write_count += 1;
	journal->events.blocks.written += 1;
	journal->events.entries.written += block->entries_in_commit;

	storeUInt64LE(header->fields.blockMapHead, journal->block_map_head);
	storeUInt64LE(header->fields.slabJournalHead,
		      journal->slab_journal_head);
	storeUInt16LE(header->fields.entryCount, block->entry_count);

	block->committing = true;

	/*
	 * In sync mode, when we are writing an increment entry for a request
	 * with FUA, or when making the increment entry for a partial write, we
	 * need to make sure all the data being mapped to by this block is
	 * stable on disk and also that the recovery journal is stable up to the
	 * current block, so we must flush before writing.
	 *
	 * In sync mode, and for FUA, we also need to make sure that the write
	 * we are doing is stable, so we issue the write with FUA.
	 */
	PhysicalLayer *layer = vioAsCompletion(block->vio)->layer;
	bool sync = !layer->isFlushRequired(layer);
	bool fua = sync || block->has_fua_entry;
	bool flushBefore = fua || block->has_partial_write_entry;
	block->has_fua_entry = false;
	block->has_partial_write_entry = false;
	launchWriteMetadataVIOWithFlush(block->vio, block_pbn, callback,
					error_handler, flushBefore, fua);

	return VDO_SUCCESS;
}

/**********************************************************************/
void dump_recovery_block(const struct recovery_journal_block *block)
{
	logInfo("    sequence number %llu; entries %" PRIu16
		"; %s; %zu entry waiters; %zu commit waiters",
		block->sequence_number, block->entry_count,
		(block->committing ? "committing" : "waiting"),
		countWaiters(&block->entry_waiters),
		countWaiters(&block->commit_waiters));
}
