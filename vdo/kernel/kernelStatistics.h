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
 */

#ifndef KERNEL_STATISTICS_H
#define KERNEL_STATISTICS_H

#include "header.h"
#include "types.h"

struct bio_stats {
  /** Number of not REQ_WRITE bios */
  uint64_t read;
  /** Number of REQ_WRITE bios */
  uint64_t write;
  /** Number of REQ_DISCARD bios */
  uint64_t discard;
  /** Number of REQ_FLUSH bios */
  uint64_t flush;
  /** Number of REQ_FUA bios */
  uint64_t fua;
};

typedef struct {
  /** Tracked bytes currently allocated. */
  uint64_t bytesUsed;
  /** Maximum tracked bytes allocated. */
  uint64_t peakBytesUsed;
} MemoryUsage;

/** UDS index statistics */
typedef struct {
  /** Number of chunk names stored in the index */
  uint64_t entriesIndexed;
  /** Number of post calls that found an existing entry */
  uint64_t postsFound;
  /** Number of post calls that added a new entry */
  uint64_t postsNotFound;
  /** Number of query calls that found an existing entry */
  uint64_t queriesFound;
  /** Number of query calls that added a new entry */
  uint64_t queriesNotFound;
  /** Number of update calls that found an existing entry */
  uint64_t updatesFound;
  /** Number of update calls that added a new entry */
  uint64_t updatesNotFound;
  /** Current number of dedupe queries that are in flight */
  uint32_t currDedupeQueries;
  /** Maximum number of dedupe queries that have been in flight */
  uint32_t maxDedupeQueries;
} IndexStatistics;

typedef struct {
  uint32_t version;
  uint32_t releaseVersion;
  /** The VDO instance */
  uint32_t instance;
  /** Current number of active VIOs */
  uint32_t currentVIOsInProgress;
  /** Maximum number of active VIOs */
  uint32_t maxVIOs;
  /** Number of times the UDS index was too slow in responding */
  uint64_t dedupeAdviceTimeouts;
  /** Number of flush requests submitted to the storage device */
  uint64_t flushOut;
  /** Logical block size */
  uint64_t logicalBlockSize;
  /** Bios submitted into VDO from above */
  struct bio_stats biosIn;
  struct bio_stats biosInPartial;
  /** Bios submitted onward for user data */
  struct bio_stats biosOut;
  /** Bios submitted onward for metadata */
  struct bio_stats biosMeta;
  struct bio_stats biosJournal;
  struct bio_stats biosPageCache;
  struct bio_stats biosOutCompleted;
  struct bio_stats biosMetaCompleted;
  struct bio_stats biosJournalCompleted;
  struct bio_stats biosPageCacheCompleted;
  struct bio_stats biosAcknowledged;
  struct bio_stats biosAcknowledgedPartial;
  /** Current number of bios in progress */
  struct bio_stats biosInProgress;
  /** Memory usage stats. */
  MemoryUsage memoryUsage;
  /** The statistics for the UDS index */
  IndexStatistics index;
} KernelStatistics;

#endif /* not KERNEL_STATISTICS_H */
