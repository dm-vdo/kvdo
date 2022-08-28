/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RECOVERY_JOURNAL_BLOCK_H
#define RECOVERY_JOURNAL_BLOCK_H

#include "permassert.h"

#include <linux/bio.h>

#include "packed-recovery-journal-block.h"
#include "recovery-journal.h"
#include "types.h"
#include "wait-queue.h"

struct recovery_journal_block {
	/* The doubly linked pointers for the free or active lists */
	struct list_head list_node;
	/* The waiter for the pending full block list */
	struct waiter write_waiter;
	/* The journal to which this block belongs */
	struct recovery_journal *journal;
	/* A pointer to a block-sized buffer holding the packed block data */
	char *block;
	/* A pointer to the current sector in the packed block buffer */
	struct packed_journal_sector *sector;
	/* The vio for writing this block */
	struct vio *vio;
	/* The sequence number for this block */
	sequence_number_t sequence_number;
	/* The location of this block in the on-disk journal */
	physical_block_number_t block_number;
	/* Whether this block is being committed */
	bool committing;
	/*
	 * Whether this block has an uncommitted increment for a write with FUA
	 */
	bool has_fua_entry;
	/* The total number of entries in this block */
	journal_entry_count_t entry_count;
	/* The total number of uncommitted entries (queued or committing) */
	journal_entry_count_t uncommitted_entry_count;
	/* The number of new entries in the current commit */
	journal_entry_count_t entries_in_commit;
	/* The queue of vios which will make entries for the next commit */
	struct wait_queue entry_waiters;
	/* The queue of vios waiting for the current commit */
	struct wait_queue commit_waiters;
};

/**
 * vdo_recovery_block_from_list_entry() - Return the block associated with a
 *                                        list entry.
 * @entry: The list entry to recast as a block.
 *
 * Return: The block.
 **/
static inline struct recovery_journal_block *
vdo_recovery_block_from_list_entry(struct list_head *entry)
{
	return list_entry(entry, struct recovery_journal_block, list_node);
}

/**
 * vdo_is_recovery_block_dirty() - Check whether a recovery block is dirty.
 * @block: The block to check.
 *
 * Indicates it has any uncommitted entries, which includes both entries not
 * written and entries written but not yet acknowledged.
 *
 * Return: true if the block has any uncommitted entries.
 **/
static inline bool __must_check
vdo_is_recovery_block_dirty(const struct recovery_journal_block *block)
{
	return (block->uncommitted_entry_count > 0);
}

/**
 * vdo_is_recovery_block_empty() - Check whether a journal block is empty.
 * @block: The block to check.
 *
 * Return: true if the block has no entries.
 **/
static inline bool __must_check
vdo_is_recovery_block_empty(const struct recovery_journal_block *block)
{
	return (block->entry_count == 0);
}

/**
 * vdo_is_recovery_block_full() - Check whether a journal block is full.
 * @block: The block to check.
 *
 * Return: true if the block is full.
 **/
static inline bool __must_check
vdo_is_recovery_block_full(const struct recovery_journal_block *block)
{
	return ((block == NULL)
		|| (block->journal->entries_per_block == block->entry_count));
}

int __must_check
vdo_make_recovery_block(struct vdo *vdo,
			struct recovery_journal *journal,
			struct recovery_journal_block **block_ptr);

void vdo_free_recovery_block(struct recovery_journal_block *block);

void vdo_initialize_recovery_block(struct recovery_journal_block *block);

int __must_check
vdo_enqueue_recovery_block_entry(struct recovery_journal_block *block,
				 struct data_vio *data_vio);

int __must_check vdo_commit_recovery_block(struct recovery_journal_block *block,
					   bio_end_io_t callback,
					   vdo_action *error_handler);

void vdo_dump_recovery_block(const struct recovery_journal_block *block);

bool __must_check
vdo_can_commit_recovery_block(struct recovery_journal_block *block);

#endif /* RECOVERY_JOURNAL_BLOCK_H */
