/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/src/uds/cacheCounters.c#1 $
 */

#include "cacheCounters.h"

#include "atomicDefs.h"
#include "compiler.h"
#include "errors.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds.h"

/**********************************************************************/
void incrementCacheCounter(CacheCounters   *counters,
                           int              probeType,
                           CacheResultKind  kind)
{
  CacheProbeType basicProbeType = probeType & ~CACHE_PROBE_IGNORE_FAILURE;
  int result = ASSERT(basicProbeType <= CACHE_PROBE_RECORD_RETRY,
                      "invalid cache probe type %#x", probeType);
  if (result != UDS_SUCCESS) {
    return;
  }
  result = ASSERT(kind <= CACHE_RESULT_QUEUED,
                  "invalid cache probe result type %#x", kind);
  if (result != UDS_SUCCESS) {
    return;
  }

  if (((probeType & CACHE_PROBE_IGNORE_FAILURE) != 0)
      && (kind != CACHE_RESULT_HIT)) {
    return;
  }

  CacheCountsByKind *kindCounts;
  switch (basicProbeType) {
  case CACHE_PROBE_INDEX_FIRST:
    kindCounts = &counters->firstTime.indexPage;
    break;
  case CACHE_PROBE_RECORD_FIRST:
    kindCounts = &counters->firstTime.recordPage;
    break;
  case CACHE_PROBE_INDEX_RETRY:
    kindCounts = &counters->retried.indexPage;
    break;
  case CACHE_PROBE_RECORD_RETRY:
    kindCounts = &counters->retried.recordPage;
    break;
  default:
    // Never used but the compiler hasn't figured that out.
    return;
  }

  uint64_t *myCounter;
  switch (kind) {
  case CACHE_RESULT_MISS:
    myCounter = &kindCounts->misses;
    break;
  case CACHE_RESULT_QUEUED:
    myCounter = &kindCounts->queued;
    break;
  case CACHE_RESULT_HIT:
    myCounter = &kindCounts->hits;
    break;
  default:
    // Never used but the compiler hasn't figured that out.
    return;
  }
  // XXX Vile case makes many assumptions.  Counters should be declared atomic.
  atomic64_inc((atomic64_t *) myCounter);
}
