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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/slabJournalInternals.h#2 $
 */

#ifndef SLAB_JOURNAL_INTERNALS_H
#define SLAB_JOURNAL_INTERNALS_H

#include "slabJournal.h"

#include "numeric.h"

#include "blockAllocatorInternals.h"
#include "blockMapEntry.h"
#include "journalPoint.h"
#include "slab.h"
#include "slabJournalFormat.h"
#include "slabSummary.h"
#include "statistics.h"
#include "waitQueue.h"

struct journal_lock {
	uint16_t count;
	sequence_number_t recovery_start;
};

struct slab_journal {
	/** A waiter object for getting a VIO pool entry */
	struct waiter resource_waiter;
	/** A waiter object for updating the slab summary */
	struct waiter slab_summary_waiter;
	/** A waiter object for getting an extent with which to flush */
	struct waiter flush_waiter;
	/** The queue of VIOs waiting to make an entry */
	struct wait_queue entry_waiters;
	/** The parent slab reference of this journal */
	struct vdo_slab *slab;

	/** Whether a tail block commit is pending */
	bool waiting_to_commit;
	/** Whether the journal is updating the slab summary */
	bool updating_slab_summary;
	/** Whether the journal is adding entries from the entry_waiters queue */
	bool adding_entries;
	/** Whether a partial write is in progress */
	bool partial_write_in_progress;

	/** The oldest block in the journal on disk */
	sequence_number_t head;
	/** The oldest block in the journal which may not be reaped */
	sequence_number_t unreapable;
	/** The end of the half-open interval of the active journal */
	sequence_number_t tail;
	/** The next journal block to be committed */
	sequence_number_t next_commit;
	/** The tail sequence number that is written in the slab summary */
	sequence_number_t summarized;
	/** The tail sequence number that was last summarized in slab summary */
	sequence_number_t last_summarized;

	/** The sequence number of the recovery journal lock */
	sequence_number_t recovery_lock;

	/**
	 * The number of entries which fit in a single block. Can't use the
	 * constant because unit tests change this number.
	 **/
	journal_entry_count_t entries_per_block;
	/**
	 * The number of full entries which fit in a single block. Can't use
	 * the constant because unit tests change this number.
	 **/
	journal_entry_count_t full_entries_per_block;

	/** The recovery journal of the VDO (slab journal holds locks on it) */
	struct recovery_journal *recovery_journal;

	/** The slab summary to update tail block location */
	struct slab_summary_zone *summary;
	/** The statistics shared by all slab journals in our physical zone */
	struct slab_journal_statistics *events;
	/**
	 * A list of the VIO pool entries for outstanding journal block writes
	 */
	struct list_head uncommitted_blocks;

	/**
	 * The current tail block header state. This will be packed into
	 * the block just before it is written.
	 **/
	struct slab_journal_block_header tail_header;
	/** A pointer to a block-sized buffer holding the packed block data */
	struct packed_slab_journal_block *block;

	/** The number of blocks in the on-disk journal */
	block_count_t size;
	/** The number of blocks at which to start pushing reference blocks */
	block_count_t flushing_threshold;
	/** The number of blocks at which all reference blocks should be writing
	 */
	block_count_t flushing_deadline;
	/**
	 * The number of blocks at which to wait for reference blocks to write
	 */
	block_count_t blocking_threshold;
	/**
	 * The number of blocks at which to scrub the slab before coming online
	 */
	block_count_t scrubbing_threshold;

	/**
	 * This list entry is for block_allocator to keep a queue of dirty
	 * journals
	 */
	struct list_head dirty_entry;

	/** The lock for the oldest unreaped block of the journal */
	struct journal_lock *reap_lock;
	/** The locks for each on disk block */
	struct journal_lock locks[];
};

/**
 * Get the slab journal block offset of the given sequence number.
 *
 * @param journal   The slab journal
 * @param sequence  The sequence number
 *
 * @return the offset corresponding to the sequence number
 **/
static inline tail_block_offset_t __must_check
get_vdo_slab_journal_block_offset(struct slab_journal *journal,
				  sequence_number_t sequence)
{
	return (sequence % journal->size);
}

/**
 * Encode a slab journal entry (exposed for unit tests).
 *
 * @param tail_header  The unpacked header for the block
 * @param payload      The journal block payload to hold the entry
 * @param sbn          The slab block number of the entry to encode
 * @param operation    The type of the entry
 **/
void encode_vdo_slab_journal_entry(struct slab_journal_block_header *tail_header,
				   slab_journal_payload *payload,
				   slab_block_number sbn,
				   enum journal_operation operation);

/**
 * Generate the packed encoding of a slab journal entry.
 *
 * @param packed        The entry into which to pack the values
 * @param sbn           The slab block number of the entry to encode
 * @param is_increment  The increment flag
 **/
static inline void pack_vdo_slab_journal_entry(packed_slab_journal_entry *packed,
					       slab_block_number sbn,
					       bool is_increment)
{
	packed->offset_low8 = (sbn & 0x0000FF);
	packed->offset_mid8 = (sbn & 0x00FF00) >> 8;
	packed->offset_high7 = (sbn & 0x7F0000) >> 16;
	packed->increment = is_increment ? 1 : 0;
}

/**
 * Decode the packed representation of a slab block header.
 *
 * @param packed  The packed header to decode
 * @param header  The header into which to unpack the values
 **/
static inline void
unpack_vdo_slab_journal_block_header(
	const struct packed_slab_journal_block_header *packed,
	struct slab_journal_block_header *header)
{
	*header = (struct slab_journal_block_header) {
		.head = __le64_to_cpu(packed->head),
		.sequence_number = __le64_to_cpu(packed->sequence_number),
		.nonce = __le64_to_cpu(packed->nonce),
		.entry_count = __le16_to_cpu(packed->entry_count),
		.metadata_type = packed->metadata_type,
		.has_block_map_increments = packed->has_block_map_increments,
	};
	unpack_vdo_journal_point(&packed->recovery_point,
				 &header->recovery_point);
}

#endif // SLAB_JOURNAL_INTERNALS_H
