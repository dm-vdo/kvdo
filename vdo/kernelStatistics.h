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

#ifndef KERNEL_STATISTICS_H
#define KERNEL_STATISTICS_H

#include "header.h"
#include "types.h"

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
	/** Current number of dedupe queries that are in flight */
	uint32_t curr_dedupe_queries;
	/** Maximum number of dedupe queries that have been in flight */
	uint32_t max_dedupe_queries;
};

struct kernel_statistics {
	uint32_t version;
	uint32_t release_version;
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

#endif /* not KERNEL_STATISTICS_H */
