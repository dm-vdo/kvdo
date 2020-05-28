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
 * $Id: //eng/uds-releases/krusty/src/uds/cacheCounters.h#4 $
 */

#ifndef CACHE_COUNTERS_H
#define CACHE_COUNTERS_H

#include "typeDefs.h"

/**
 * Basic counts of hits and misses for a given type of cache probe.
 **/
struct cache_counts_by_kind {
	/** Number of hits */
	uint64_t hits;
	/** Number of misses */
	uint64_t misses;
	/** Number of probes for data already queued for read */
	uint64_t queued;
};

/**
 * The various types of cache probes we care about.
 **/
typedef enum {
	/** First attempt to look up an index page, for a given request. */
	CACHE_PROBE_INDEX_FIRST = 0,
	/** First attempt to look up a record page, for a given request. */
	CACHE_PROBE_RECORD_FIRST,
	/** Second or later attempt to look up an index page, for a given
	 * request.
	 */
	CACHE_PROBE_INDEX_RETRY,
	/** Second or later attempt to look up a record page, for a given
	 * request.
	 */
	CACHE_PROBE_RECORD_RETRY
} cache_probe_type_t;

enum {
	/** Flag bit to indicate that failures shouldn't be recorded.  */
	CACHE_PROBE_IGNORE_FAILURE = 128
};

/**
 * Result-type counts for both kinds of data pages in the page cache.
 **/
struct cache_counts_by_page_type {
	/** His/miss counts for index pages. */
	struct cache_counts_by_kind index_page;
	/** Hit/miss counts for record pages. */
	struct cache_counts_by_kind record_page;
};

/**
 * All the counters used for an entry cache.
 **/
struct cache_counters {
	// counters for the page cache
	/** Hit/miss counts for the first attempt per request */
	struct cache_counts_by_page_type first_time;
	/** Hit/miss counts when a second (or later) attempt is needed */
	struct cache_counts_by_page_type retried;

	/** Number of cache entry invalidations due to single-entry eviction */
	uint64_t evictions;
	/** Number of cache entry invalidations due to chapter expiration */
	uint64_t expirations;

	// counters for the sparse chapter index cache
	/** Hit/miss counts for the sparse cache chapter probes */
	struct cache_counts_by_kind sparse_chapters;
	/** Hit/miss counts for the sparce cache name searches */
	struct cache_counts_by_kind sparse_searches;
};

/**
 * Success/failure assessment of cache probe result.
 **/
typedef enum {
	/** The requested entry was found in the cache */
	CACHE_RESULT_HIT,
	/** The requested entry was not found in the cache */
	CACHE_RESULT_MISS,
	/** The requested entry wasn't found in the cache but is queued for
	  * read
	 */
	CACHE_RESULT_QUEUED
} cache_result_kind_t;

/**
 * Increment one of the cache counters.
 *
 * @param counters    pointer to the counters
 * @param probe_type  type of access done
 * @param kind        result of probe
 **/
void increment_cache_counter(struct cache_counters *counters,
			     int probe_type,
			     cache_result_kind_t kind);

#endif /* CACHE_COUNTERS_H */
