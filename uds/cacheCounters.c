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

#include "cacheCounters.h"

#include <linux/atomic.h>

#include "compiler.h"
#include "errors.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds.h"

/**********************************************************************/
void increment_cache_counter(struct cache_counters *counters,
			     int probe_type,
			     enum cache_result_kind kind)
{
	struct cache_counts_by_kind *kind_counts;
	uint64_t *my_counter;
	enum cache_probe_type basic_probe_type =
		probe_type & ~CACHE_PROBE_IGNORE_FAILURE;
	int result = ASSERT(basic_probe_type <= CACHE_PROBE_RECORD_RETRY,
			    "invalid cache probe type %#x",
			    probe_type);
	if (result != UDS_SUCCESS) {
		return;
	}
	result = ASSERT(kind <= CACHE_RESULT_QUEUED,
			"invalid cache probe result type %#x",
			kind);
	if (result != UDS_SUCCESS) {
		return;
	}

	if (((probe_type & CACHE_PROBE_IGNORE_FAILURE) != 0) &&
	    (kind != CACHE_RESULT_HIT)) {
		return;
	}

	switch (basic_probe_type) {
	case CACHE_PROBE_INDEX_FIRST:
		kind_counts = &counters->first_time.index_page;
		break;
	case CACHE_PROBE_RECORD_FIRST:
		kind_counts = &counters->first_time.record_page;
		break;
	case CACHE_PROBE_INDEX_RETRY:
		kind_counts = &counters->retried.index_page;
		break;
	case CACHE_PROBE_RECORD_RETRY:
		kind_counts = &counters->retried.record_page;
		break;
	default:
		// Never used but the compiler hasn't figured that out.
		return;
	}

	switch (kind) {
	case CACHE_RESULT_MISS:
		my_counter = &kind_counts->misses;
		break;
	case CACHE_RESULT_QUEUED:
		my_counter = &kind_counts->queued;
		break;
	case CACHE_RESULT_HIT:
		my_counter = &kind_counts->hits;
		break;
	default:
		// Never used but the compiler hasn't figured that out.
		return;
	}
	// XXX Vile case makes many assumptions.  Counters should be declared
	// atomic.
	atomic64_inc((atomic64_t *) my_counter);
}
