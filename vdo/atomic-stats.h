/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef ATOMIC_STATS_H
#define ATOMIC_STATS_H

#include <linux/atomic.h>

#include "statistics.h"

/* Keep struct bio statistics atomically */
struct atomic_bio_stats {
	atomic64_t read; /* Number of not REQ_WRITE bios */
	atomic64_t write; /* Number of REQ_WRITE bios */
	atomic64_t discard; /* Number of REQ_DISCARD bios */
	atomic64_t flush; /* Number of REQ_FLUSH bios */
	atomic64_t empty_flush; /* Number of REQ_PREFLUSH bios without data */
	atomic64_t fua; /* Number of REQ_FUA bios */
};

/*
 * Counters are atomic since updates can arrive concurrently from arbitrary
 * threads.
 */
struct atomic_statistics {
	atomic64_t bios_submitted;
	atomic64_t bios_completed;
	atomic64_t dedupe_context_busy;
	atomic64_t flush_out;
	atomic64_t invalid_advice_pbn_count;
	atomic64_t no_space_error_count;
	atomic64_t read_only_error_count;
	struct atomic_bio_stats bios_in;
	struct atomic_bio_stats bios_in_partial;
	struct atomic_bio_stats bios_out;
	struct atomic_bio_stats bios_out_completed;
	struct atomic_bio_stats bios_acknowledged;
	struct atomic_bio_stats bios_acknowledged_partial;
	struct atomic_bio_stats bios_meta;
	struct atomic_bio_stats bios_meta_completed;
	struct atomic_bio_stats bios_journal;
	struct atomic_bio_stats bios_journal_completed;
	struct atomic_bio_stats bios_page_cache;
	struct atomic_bio_stats bios_page_cache_completed;
};

#endif /* ATOMIC_STATS_H */
