/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_JOURNAL_H
#define SLAB_JOURNAL_H

#include <linux/list.h>

#include "numeric.h"

#include "completion.h"
#include "journal-point.h"
#include "types.h"

#include "block-allocator.h"
#include "block-map-entry.h"
#include "journal-point.h"
#include "slab.h"
#include "slab-journal-format.h"
#include "slab-summary.h"
#include "statistics.h"
#include "wait-queue.h"

struct journal_lock {
	uint16_t count;
	sequence_number_t recovery_start;
};

struct slab_journal {
	/* A waiter object for getting a VIO pool entry */
	struct waiter resource_waiter;
	/* A waiter object for updating the slab summary */
	struct waiter slab_summary_waiter;
	/* A waiter object for getting a vio with which to flush */
	struct waiter flush_waiter;
	/* The queue of VIOs waiting to make an entry */
	struct wait_queue entry_waiters;
	/* The parent slab reference of this journal */
	struct vdo_slab *slab;

	/* Whether a tail block commit is pending */
	bool waiting_to_commit;
	/* Whether the journal is updating the slab summary */
	bool updating_slab_summary;
	/* Whether the journal is adding entries from the entry_waiters queue */
	bool adding_entries;
	/* Whether a partial write is in progress */
	bool partial_write_in_progress;

	/* The oldest block in the journal on disk */
	sequence_number_t head;
	/* The oldest block in the journal which may not be reaped */
	sequence_number_t unreapable;
	/* The end of the half-open interval of the active journal */
	sequence_number_t tail;
	/* The next journal block to be committed */
	sequence_number_t next_commit;
	/* The tail sequence number that is written in the slab summary */
	sequence_number_t summarized;
	/* The tail sequence number that was last summarized in slab summary */
	sequence_number_t last_summarized;

	/* The sequence number of the recovery journal lock */
	sequence_number_t recovery_lock;

	/*
	 * The number of entries which fit in a single block. Can't use the
	 * constant because unit tests change this number.
	 */
	journal_entry_count_t entries_per_block;
	/*
	 * The number of full entries which fit in a single block. Can't use
	 * the constant because unit tests change this number.
	 */
	journal_entry_count_t full_entries_per_block;

	/* The recovery journal of the VDO (slab journal holds locks on it) */
	struct recovery_journal *recovery_journal;

	/* The slab summary to update tail block location */
	struct slab_summary_zone *summary;
	/* The statistics shared by all slab journals in our physical zone */
	struct slab_journal_statistics *events;
	/*
	 * A list of the VIO pool entries for outstanding journal block writes
	 */
	struct list_head uncommitted_blocks;

	/*
	 * The current tail block header state. This will be packed into
	 * the block just before it is written.
	 */
	struct slab_journal_block_header tail_header;
	/* A pointer to a block-sized buffer holding the packed block data */
	struct packed_slab_journal_block *block;

	/* The number of blocks in the on-disk journal */
	block_count_t size;
	/* The number of blocks at which to start pushing reference blocks */
	block_count_t flushing_threshold;
	/*
	 * The number of blocks at which all reference blocks should be writing
	 */
	block_count_t flushing_deadline;
	/*
	 * The number of blocks at which to wait for reference blocks to write
	 */
	block_count_t blocking_threshold;
	/*
	 * The number of blocks at which to scrub the slab before coming online
	 */
	block_count_t scrubbing_threshold;

	/*
	 * This list entry is for block_allocator to keep a queue of dirty
	 * journals
	 */
	struct list_head dirty_entry;

	/* The lock for the oldest unreaped block of the journal */
	struct journal_lock *reap_lock;
	/* The locks for each on disk block */
	struct journal_lock locks[];
};

/**
 * vdo_pack_slab_journal_entry() - Generate the packed encoding of a
 *                                 slab journal entry.
 * @packed: The entry into which to pack the values.
 * @sbn: The slab block number of the entry to encode.
 * @is_increment: The increment flag.
 */
static inline void vdo_pack_slab_journal_entry(packed_slab_journal_entry *packed,
					       slab_block_number sbn,
					       bool is_increment)
{
	packed->offset_low8 = (sbn & 0x0000FF);
	packed->offset_mid8 = (sbn & 0x00FF00) >> 8;
	packed->offset_high7 = (sbn & 0x7F0000) >> 16;
	packed->increment = is_increment ? 1 : 0;
}

/**
 * vdo_unpack_slab_journal_block_header() - Decode the packed
 *                                          representation of a slab
 *                                          block header.
 * @packed: The packed header to decode.
 * @header: The header into which to unpack the values.
 */
static inline void
vdo_unpack_slab_journal_block_header(
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
	vdo_unpack_journal_point(&packed->recovery_point,
				 &header->recovery_point);
}

struct slab_journal * __must_check
vdo_slab_journal_from_dirty_entry(struct list_head *entry);

int __must_check vdo_make_slab_journal(struct block_allocator *allocator,
				       struct vdo_slab *slab,
				       struct recovery_journal *recovery_journal,
				       struct slab_journal **journal_ptr);

void vdo_free_slab_journal(struct slab_journal *journal);

bool __must_check vdo_is_slab_journal_blank(const struct slab_journal *journal);

bool __must_check vdo_is_slab_journal_active(struct slab_journal *journal);

void vdo_abort_slab_journal_waiters(struct slab_journal *journal);

void vdo_reopen_slab_journal(struct slab_journal *journal);

bool __must_check
vdo_attempt_replay_into_slab_journal(struct slab_journal *journal,
				     physical_block_number_t pbn,
				     enum journal_operation operation,
				     struct journal_point *recovery_point,
				     struct vdo_completion *parent);

void vdo_add_slab_journal_entry(struct slab_journal *journal,
				struct data_vio *data_vio);

void vdo_adjust_slab_journal_block_reference(struct slab_journal *journal,
					     sequence_number_t sequence_number,
					     int adjustment);

bool __must_check
vdo_release_recovery_journal_lock(struct slab_journal *journal,
				  sequence_number_t recovery_lock);

void vdo_drain_slab_journal(struct slab_journal *journal);

void vdo_decode_slab_journal(struct slab_journal *journal);

bool __must_check
vdo_slab_journal_requires_scrubbing(const struct slab_journal *journal);

/**
 * vdo_get_slab_journal_block_offset() - Get the slab journal block offset of
 *                                       the given sequence number.
 * @journal: The slab journal.
 * @sequence: The sequence number.
 *
 * Return: The offset corresponding to the sequence number.
 */
static inline tail_block_offset_t __must_check
vdo_get_slab_journal_block_offset(struct slab_journal *journal,
				  sequence_number_t sequence)
{
	return (sequence % journal->size);
}

void vdo_resume_slab_journal(struct slab_journal *journal);

void vdo_dump_slab_journal(const struct slab_journal *journal);

#endif /* SLAB_JOURNAL_H */
