/* SPDX-License-Identifier: GPL-2.0-only */
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
 */

#ifndef STATISTICS_H
#define STATISTICS_H

#include "header.h"
#include "types.h"

enum {
	STATISTICS_VERSION = 35,
};

struct block_allocator_statistics {
	/** The total number of slabs from which blocks may be allocated */
	uint64_t slab_count;
	/** The total number of slabs from which blocks have ever been allocated */
	uint64_t slabs_opened;
	/** The number of times since loading that a slab has been re-opened */
	uint64_t slabs_reopened;
};

/**
 * Counters for tracking the number of items written (blocks, requests, etc.)
 * that keep track of totals at steps in the write pipeline. Three counters
 * allow the number of buffered, in-memory items and the number of in-flight,
 * unacknowledged writes to be derived, while still tracking totals for
 * reporting purposes
 **/
struct commit_statistics {
	/** The total number of items on which processing has started */
	uint64_t started;
	/** The total number of items for which a write operation has been issued */
	uint64_t written;
	/** The total number of items for which a write operation has completed */
	uint64_t committed;
};

/** Counters for events in the recovery journal */
struct recovery_journal_statistics {
	/** Number of times the on-disk journal was full */
	uint64_t disk_full;
	/** Number of times the recovery journal requested slab journal commits. */
	uint64_t slab_journal_commits_requested;
	/** Write/Commit totals for individual journal entries */
	struct commit_statistics entries;
	/** Write/Commit totals for journal blocks */
	struct commit_statistics blocks;
};

/** The statistics for the compressed block packer. */
struct packer_statistics {
	/** Number of compressed data items written since startup */
	uint64_t compressed_fragments_written;
	/** Number of blocks containing compressed items written since startup */
	uint64_t compressed_blocks_written;
	/** Number of VIOs that are pending in the packer */
	uint64_t compressed_fragments_in_packer;
};

/** The statistics for the slab journals. */
struct slab_journal_statistics {
	/** Number of times the on-disk journal was full */
	uint64_t disk_full_count;
	/** Number of times an entry was added over the flush threshold */
	uint64_t flush_count;
	/** Number of times an entry was added over the block threshold */
	uint64_t blocked_count;
	/** Number of times a tail block was written */
	uint64_t blocks_written;
	/** Number of times we had to wait for the tail to write */
	uint64_t tail_busy_count;
};

/** The statistics for the slab summary. */
struct slab_summary_statistics {
	/** Number of blocks written */
	uint64_t blocks_written;
};

/** The statistics for the reference counts. */
struct ref_counts_statistics {
	/** Number of reference blocks written */
	uint64_t blocks_written;
};

/** The statistics for the block map. */
struct block_map_statistics {
	/** number of dirty (resident) pages */
	uint32_t dirty_pages;
	/** number of clean (resident) pages */
	uint32_t clean_pages;
	/** number of free pages */
	uint32_t free_pages;
	/** number of pages in failed state */
	uint32_t failed_pages;
	/** number of pages incoming */
	uint32_t incoming_pages;
	/** number of pages outgoing */
	uint32_t outgoing_pages;
	/** how many times free page not avail */
	uint32_t cache_pressure;
	/** number of get_vdo_page() calls for read */
	uint64_t read_count;
	/** number of get_vdo_page() calls for write */
	uint64_t write_count;
	/** number of times pages failed to read */
	uint64_t failed_reads;
	/** number of times pages failed to write */
	uint64_t failed_writes;
	/** number of gets that are reclaimed */
	uint64_t reclaimed;
	/** number of gets for outgoing pages */
	uint64_t read_outgoing;
	/** number of gets that were already there */
	uint64_t found_in_cache;
	/** number of gets requiring discard */
	uint64_t discard_required;
	/** number of gets enqueued for their page */
	uint64_t wait_for_page;
	/** number of gets that have to fetch */
	uint64_t fetch_required;
	/** number of page fetches */
	uint64_t pages_loaded;
	/** number of page saves */
	uint64_t pages_saved;
	/** the number of flushes issued */
	uint64_t flush_count;
};

/** The dedupe statistics from hash locks */
struct hash_lock_statistics {
	/** Number of times the UDS advice proved correct */
	uint64_t dedupe_advice_valid;
	/** Number of times the UDS advice proved incorrect */
	uint64_t dedupe_advice_stale;
	/** Number of writes with the same data as another in-flight write */
	uint64_t concurrent_data_matches;
	/** Number of writes whose hash collided with an in-flight write */
	uint64_t concurrent_hash_collisions;
	/** Current number of dedupe queries that are in flight */
	uint32_t curr_dedupe_queries;
};

/** Counts of error conditions in VDO. */
struct error_statistics {
	/** number of times VDO got an invalid dedupe advice PBN from UDS */
	uint64_t invalid_advice_pbn_count;
	/** number of times a VIO completed with a VDO_NO_SPACE error */
	uint64_t no_space_error_count;
	/** number of times a VIO completed with a VDO_READ_ONLY error */
	uint64_t read_only_error_count;
};

struct bio_stats {
	/** Number of REQ_OP_READ bios */
	uint64_t read;
	/** Number of REQ_OP_WRITE bios with data */
	uint64_t write;
	/** Number of bios tagged with REQ_PREFLUSH and containing no data */
	uint64_t empty_flush;
	/** Number of REQ_OP_DISCARD bios */
	uint64_t discard;
	/** Number of bios tagged with REQ_PREFLUSH */
	uint64_t flush;
	/** Number of bios tagged with REQ_FUA */
	uint64_t fua;
};

struct memory_usage {
	/** Tracked bytes currently allocated. */
	uint64_t bytes_used;
	/** Maximum tracked bytes allocated. */
	uint64_t peak_bytes_used;
};

/** UDS index statistics */
struct index_statistics {
	/** Number of chunk names stored in the index */
	uint64_t entries_indexed;
	/** Number of post calls that found an existing entry */
	uint64_t posts_found;
	/** Number of post calls that added a new entry */
	uint64_t posts_not_found;
	/** Number of query calls that found an existing entry */
	uint64_t queries_found;
	/** Number of query calls that added a new entry */
	uint64_t queries_not_found;
	/** Number of update calls that found an existing entry */
	uint64_t updates_found;
	/** Number of update calls that added a new entry */
	uint64_t updates_not_found;
};

/** The statistics of the vdo service. */
struct vdo_statistics {
	uint32_t version;
	uint32_t release_version;
	/** Number of blocks used for data */
	uint64_t data_blocks_used;
	/** Number of blocks used for VDO metadata */
	uint64_t overhead_blocks_used;
	/** Number of logical blocks that are currently mapped to physical blocks */
	uint64_t logical_blocks_used;
	/** number of physical blocks */
	block_count_t physical_blocks;
	/** number of logical blocks */
	block_count_t logical_blocks;
	/** Size of the block map page cache, in bytes */
	uint64_t block_map_cache_size;
	/** The physical block size */
	uint64_t block_size;
	/** Number of times the VDO has successfully recovered */
	uint64_t complete_recoveries;
	/** Number of times the VDO has recovered from read-only mode */
	uint64_t read_only_recoveries;
	/** String describing the operating mode of the VDO */
	char mode[15];
	/** Whether the VDO is in recovery mode */
	bool in_recovery_mode;
	/** What percentage of recovery mode work has been completed */
	uint8_t recovery_percentage;
	/** The statistics for the compressed block packer */
	struct packer_statistics packer;
	/** Counters for events in the block allocator */
	struct block_allocator_statistics allocator;
	/** Counters for events in the recovery journal */
	struct recovery_journal_statistics journal;
	/** The statistics for the slab journals */
	struct slab_journal_statistics slab_journal;
	/** The statistics for the slab summary */
	struct slab_summary_statistics slab_summary;
	/** The statistics for the reference counts */
	struct ref_counts_statistics ref_counts;
	/** The statistics for the block map */
	struct block_map_statistics block_map;
	/** The dedupe statistics from hash locks */
	struct hash_lock_statistics hash_lock;
	/** Counts of error conditions */
	struct error_statistics errors;
	/** The VDO instance */
	uint32_t instance;
	/** Current number of active VIOs */
	uint32_t current_vios_in_progress;
	/** Maximum number of active VIOs */
	uint32_t max_vios;
	/** Number of times the UDS index was too slow in responding */
	uint64_t dedupe_advice_timeouts;
	/** Number of flush requests submitted to the storage device */
	uint64_t flush_out;
	/** Logical block size */
	uint64_t logical_block_size;
	/** Bios submitted into VDO from above */
	struct bio_stats bios_in;
	struct bio_stats bios_in_partial;
	/** Bios submitted onward for user data */
	struct bio_stats bios_out;
	/** Bios submitted onward for metadata */
	struct bio_stats bios_meta;
	struct bio_stats bios_journal;
	struct bio_stats bios_page_cache;
	struct bio_stats bios_out_completed;
	struct bio_stats bios_meta_completed;
	struct bio_stats bios_journal_completed;
	struct bio_stats bios_page_cache_completed;
	struct bio_stats bios_acknowledged;
	struct bio_stats bios_acknowledged_partial;
	/** Current number of bios in progress */
	struct bio_stats bios_in_progress;
	/** Memory usage stats. */
	struct memory_usage memory_usage;
	/** The statistics for the UDS index */
	struct index_statistics index;
};

#endif /* not STATISTICS_H */
