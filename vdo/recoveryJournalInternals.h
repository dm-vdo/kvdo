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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/recoveryJournalInternals.h#1 $
 */

#ifndef RECOVERY_JOURNAL_INTERNALS_H
#define RECOVERY_JOURNAL_INTERNALS_H

#include <linux/list.h>

#include "numeric.h"

#include "adminState.h"
#include "fixedLayout.h"
#include "journalPoint.h"
#include "lockCounter.h"
#include "recoveryJournal.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

struct recovery_journal_block;

struct recovery_journal {
	/** The thread ID of the journal zone */
	thread_id_t thread_id;
	/** The slab depot which can hold locks on this journal */
	struct slab_depot *depot;
	/** The block map which can hold locks on this journal */
	struct block_map *block_map;
	/** The queue of vios waiting to make increment entries */
	struct wait_queue increment_waiters;
	/** The queue of vios waiting to make decrement entries */
	struct wait_queue decrement_waiters;
	/** The number of free entries in the journal */
	uint64_t available_space;
	/** The number of decrement entries which need to be made */
	vio_count_t pending_decrement_count;
	/**
	 * Whether the journal is adding entries from the increment or
	 * decrement waiters queues
	 **/
	bool adding_entries;
	/** The notifier for read-only mode */
	struct read_only_notifier *read_only_notifier;
	/** The administrative state of the journal */
	struct admin_state state;
	/** Whether a reap is in progress */
	bool reaping;
	/** The partition which holds the journal on disk */
	struct partition *partition;
	/** The oldest active block in the journal on disk for block map rebuild
	 */
	sequence_number_t block_map_head;
	/** The oldest active block in the journal on disk for slab journal
	 * replay */
	sequence_number_t slab_journal_head;
	/** The newest block in the journal on disk to which a write has
	 * finished */
	sequence_number_t last_write_acknowledged;
	/** The end of the half-open interval of the active journal */
	sequence_number_t tail;
	/** The point at which the last entry will have been added */
	struct journal_point append_point;
	/** The journal point of the vio most recently released from the journal
	 */
	struct journal_point commit_point;
	/** The nonce of the VDO */
	nonce_t nonce;
	/** The number of recoveries completed by the VDO */
	uint8_t recovery_count;
	/** The number of entries which fit in a single block */
	journal_entry_count_t entries_per_block;
	/** Unused in-memory journal blocks */
	struct list_head free_tail_blocks;
	/** In-memory journal blocks with records */
	struct list_head active_tail_blocks;
	/** A pointer to the active block (the one we are adding entries to now)
	 */
	struct recovery_journal_block *active_block;
	/** Journal blocks that need writing */
	struct wait_queue pending_writes;
	/** The new block map reap head after reaping */
	sequence_number_t block_map_reap_head;
	/** The head block number for the block map rebuild range */
	block_count_t block_map_head_block_number;
	/** The new slab journal reap head after reaping */
	sequence_number_t slab_journal_reap_head;
	/** The head block number for the slab journal replay range */
	block_count_t slab_journal_head_block_number;
	/** The data-less vio, usable only for flushing */
	struct vio *flush_vio;
	/** The number of blocks in the on-disk journal */
	block_count_t size;
	/** The number of logical blocks that are in-use */
	block_count_t logical_blocks_used;
	/** The number of block map pages that are allocated */
	block_count_t block_map_data_blocks;
	/** The number of journal blocks written but not yet acknowledged */
	block_count_t pending_write_count;
	/** The threshold at which slab journal tail blocks will be written out
	 */
	block_count_t slab_journal_commit_threshold;
	/** Counters for events in the journal that are reported as statistics
	 */
	struct recovery_journal_statistics events;
	/** The locks for each on-disk block */
	struct lock_counter *lock_counter;
};

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return The block number corresponding to the sequence number
 **/
static inline physical_block_number_t __must_check
get_recovery_journal_block_number(const struct recovery_journal *journal,
				  sequence_number_t sequence)
{
	// Since journal size is a power of two, the block number modulus can
	// just be extracted from the low-order bits of the sequence.
	return compute_recovery_journal_block_number(journal->size, sequence);
}

/**
 * Compute the check byte for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number
 *
 * @return The check byte corresponding to the sequence number
 **/
static inline uint8_t __must_check
compute_recovery_check_byte(const struct recovery_journal *journal,
			    sequence_number_t sequence)
{
	// The check byte must change with each trip around the journal.
	return (((sequence / journal->size) & 0x7F) | 0x80);
}

#endif // RECOVERY_JOURNAL_INTERNALS_H
