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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryJournalInternals.h#16 $
 */

#ifndef RECOVERY_JOURNAL_INTERNALS_H
#define RECOVERY_JOURNAL_INTERNALS_H

#include "numeric.h"

#include "adminState.h"
#include "fixedLayout.h"
#include "journalPoint.h"
#include "lockCounter.h"
#include "recoveryJournal.h"
#include "ringNode.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

struct recovery_journal_block;

struct recovery_journal {
	/** The thread ID of the journal zone */
	ThreadID thread_id;
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
	VIOCount pending_decrement_count;
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
	SequenceNumber block_map_head;
	/** The oldest active block in the journal on disk for slab journal
	 * replay */
	SequenceNumber slab_journal_head;
	/** The newest block in the journal on disk to which a write has
	 * finished */
	SequenceNumber last_write_acknowledged;
	/** The end of the half-open interval of the active journal */
	SequenceNumber tail;
	/** The point at which the last entry will have been added */
	struct journal_point append_point;
	/** The journal point of the vio most recently released from the journal
	 */
	struct journal_point commit_point;
	/** The nonce of the VDO */
	Nonce nonce;
	/** The number of recoveries completed by the VDO */
	uint8_t recovery_count;
	/** The number of entries which fit in a single block */
	JournalEntryCount entries_per_block;
	/** Unused in-memory journal blocks */
	RingNode free_tail_blocks;
	/** In-memory journal blocks with records */
	RingNode active_tail_blocks;
	/** A pointer to the active block (the one we are adding entries to now)
	 */
	struct recovery_journal_block *active_block;
	/** Journal blocks that need writing */
	struct wait_queue pending_writes;
	/** The new block map reap head after reaping */
	SequenceNumber block_map_reap_head;
	/** The head block number for the block map rebuild range */
	BlockCount block_map_head_block_number;
	/** The new slab journal reap head after reaping */
	SequenceNumber slab_journal_reap_head;
	/** The head block number for the slab journal replay range */
	BlockCount slab_journal_head_block_number;
	/** The vio on which we can call flush (less ick, but still ick) */
	struct vio *flush_vio;
	/** The data block which must live in the vio in the flush extent */
	char *unused_flush_vio_data;
	/** The number of blocks in the on-disk journal */
	BlockCount size;
	/** The number of logical blocks that are in-use */
	BlockCount logical_blocks_used;
	/** The number of block map pages that are allocated */
	BlockCount block_map_data_blocks;
	/** The number of journal blocks written but not yet acknowledged */
	BlockCount pending_write_count;
	/** The threshold at which slab journal tail blocks will be written out
	 */
	BlockCount slab_journal_commit_threshold;
	/** Counters for events in the journal that are reported as statistics
	 */
	RecoveryJournalStatistics events;
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
__attribute__((warn_unused_result)) static inline PhysicalBlockNumber
get_recovery_journal_block_number(const struct recovery_journal *journal,
				  SequenceNumber sequence)
{
	// Since journal size is a power of two, the block number modulus can
	// just be extracted from the low-order bits of the sequence.
	return (sequence & (journal->size - 1));
}

/**
 * Compute the check byte for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number
 *
 * @return The check byte corresponding to the sequence number
 **/
__attribute__((warn_unused_result)) static inline uint8_t
compute_recovery_check_byte(const struct recovery_journal *journal,
			    SequenceNumber sequence)
{
	// The check byte must change with each trip around the journal.
	return (((sequence / journal->size) & 0x7F) | 0x80);
}

#endif // RECOVERY_JOURNAL_INTERNALS_H
