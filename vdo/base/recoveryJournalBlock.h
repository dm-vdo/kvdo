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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalBlock.h#19 $
 */

#ifndef RECOVERY_JOURNAL_BLOCK_H
#define RECOVERY_JOURNAL_BLOCK_H

#include "permassert.h"

#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalInternals.h"
#include "types.h"
#include "waitQueue.h"

struct recovery_journal_block {
	/** The doubly linked pointers for the free or active lists */
	struct list_head list_entry;
	/** The waiter for the pending full block list */
	struct waiter write_waiter;
	/** The journal to which this block belongs */
	struct recovery_journal *journal;
	/** A pointer to a block-sized buffer holding the packed block data */
	char *block;
	/** A pointer to the current sector in the packed block buffer */
	struct packed_journal_sector *sector;
	/** The vio for writing this block */
	struct vio *vio;
	/** The sequence number for this block */
	sequence_number_t sequence_number;
	/** The location of this block in the on-disk journal */
	physical_block_number_t block_number;
	/** Whether this block is being committed */
	bool committing;
	/**
	 * Whether this block has an uncommitted increment for a partial write
	 */
	bool has_partial_write_entry;
	/**
	 * Whether this block has an uncommitted increment for a write with FUA
	 */
	bool has_fua_entry;
	/** The total number of entries in this block */
	JournalEntryCount entry_count;
	/** The total number of uncommitted entries (queued or committing) */
	JournalEntryCount uncommitted_entry_count;
	/** The number of new entries in the current commit */
	JournalEntryCount entries_in_commit;
	/** The queue of vios which will make entries for the next commit */
	struct wait_queue entry_waiters;
	/** The queue of vios waiting for the current commit */
	struct wait_queue commit_waiters;
};

/**
 * Return the block associated with a list entry.
 *
 * @param entry    The list entry to recast as a block
 *
 * @return The block
 **/
static inline struct recovery_journal_block *
block_from_list_entry(struct list_head *entry)
{
	return container_of(entry, struct recovery_journal_block, list_entry);
}

/**
 * Check whether a recovery block is dirty, indicating it has any uncommitted
 * entries, which includes both entries not written and entries written but
 * not yet acknowledged.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the block has any uncommitted entries
 **/
static inline bool __must_check
is_recovery_block_dirty(const struct recovery_journal_block *block)
{
	return (block->uncommitted_entry_count > 0);
}

/**
 * Check whether a journal block is empty.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the block has no entries
 **/
static inline bool __must_check
is_recovery_block_empty(const struct recovery_journal_block *block)
{
	return (block->entry_count == 0);
}

/**
 * Check whether a journal block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the the block is full
 **/
static inline bool __must_check
is_recovery_block_full(const struct recovery_journal_block *block)
{
	return ((block == NULL)
		|| (block->journal->entries_per_block == block->entry_count));
}

/**
 * Construct a journal block.
 *
 * @param [in]  layer      The layer from which to construct vios
 * @param [in]  journal    The journal to which the block will belong
 * @param [out] block_ptr  A pointer to receive the new block
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_recovery_block(PhysicalLayer *layer,
		    struct recovery_journal *journal,
		    struct recovery_journal_block **block_ptr);

/**
 * Free a tail block and null out the reference to it.
 *
 * @param block_ptr  The reference to the tail block to free
 **/
void free_recovery_block(struct recovery_journal_block **block_ptr);

/**
 * Initialize the next active recovery journal block.
 *
 * @param block  The journal block to initialize
 **/
void initialize_recovery_block(struct recovery_journal_block *block);

/**
 * Enqueue a data_vio to asynchronously encode and commit its next recovery
 * journal entry in this block. The data_vio will not be continued until the
 * entry is committed to the on-disk journal. The caller is responsible for
 * ensuring the block is not already full.
 *
 * @param block     The journal block in which to make an entry
 * @param data_vio  The data_vio to enqueue
 *
 * @return VDO_SUCCESS or an error code if the data_vio could not be enqueued
 **/
int __must_check
enqueue_recovery_block_entry(struct recovery_journal_block *block,
			     struct data_vio *data_vio);

/**
 * Attempt to commit a block. If the block is not the oldest block with
 * uncommitted entries or if it is already being committed, nothing will be
 * done.
 *
 * @param block          The block to write
 * @param callback       The function to call when the write completes
 * @param error_handler  The handler for flush or write errors
 *
 * @return VDO_SUCCESS, or an error if the write could not be launched
 **/
int __must_check commit_recovery_block(struct recovery_journal_block *block,
				       vdo_action *callback,
				       vdo_action *error_handler);

/**
 * Dump the contents of the recovery block to the log.
 *
 * @param block  The block to dump
 **/
void dump_recovery_block(const struct recovery_journal_block *block);

/**
 * Check whether a journal block can be committed.
 *
 * @param block  The journal block in question
 *
 * @return <code>true</code> if the block can be committed now
 **/
bool __must_check
can_commit_recovery_block(struct recovery_journal_block *block);

#endif // RECOVERY_JOURNAL_BLOCK_H
