/*
 * Copyright (c) 2017 Red Hat, Inc.
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

typedef struct {
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
} BioStats;

/** The statistics for the read cache. */
typedef struct {
  /** Number of times the read cache was asked for a specific pbn. */
  uint64_t accesses;
  /** Number of times the read cache found the requested pbn. */
  uint64_t hits;
  /** Number of times the found requested pbn had valid data. */
  uint64_t dataHits;
} ReadCacheStats;

typedef struct {
  /** Tracked bytes currently allocated. */
  uint64_t bytesUsed;
  /** Maximum tracked bytes allocated. */
  uint64_t peakBytesUsed;
  /** Bio structures currently allocated (size not tracked). */
  uint64_t biosUsed;
  /** Maximum number of bios allocated. */
  uint64_t peakBioCount;
} MemoryUsage;

typedef struct {
  uint32_t version;
  uint32_t releaseVersion;
  /** The VDO instance */
  uint32_t instance;
  /** Current number of active VIOs */
  uint32_t currentVIOsInProgress;
  /** Maximum number of active VIOs */
  uint32_t maxVIOs;
  /** Current number of Dedupe queries that are in flight */
  uint32_t currDedupeQueries;
  /** Maximum number of Dedupe queries that have been in flight */
  uint32_t maxDedupeQueries;
  /** Number of times the Albireo advice proved correct */
  uint64_t dedupeAdviceValid;
  /** Number of times the Albireo advice proved incorrect */
  uint64_t dedupeAdviceStale;
  /** Number of times the Albireo server was too slow in responding */
  uint64_t dedupeAdviceTimeouts;
  /** Number of flush requests submitted to the storage device */
  uint64_t flushOut;
  /** Logical block size */
  uint64_t logicalBlockSize;
  /** Bios submitted into VDO from above */
  BioStats biosIn;
  BioStats biosInPartial;
  /** Bios submitted onward for user data */
  BioStats biosOut;
  /** Bios submitted onward for metadata */
  BioStats biosMeta;
  BioStats biosJournal;
  BioStats biosPageCache;
  BioStats biosOutCompleted;
  BioStats biosMetaCompleted;
  BioStats biosJournalCompleted;
  BioStats biosPageCacheCompleted;
  BioStats biosAcknowledged;
  BioStats biosAcknowledgedPartial;
  /** Current number of bios in progress */
  BioStats biosInProgress;
  /** The read cache stats. */
  ReadCacheStats readCache;
  /** Memory usage stats. */
  MemoryUsage memoryUsage;
} KernelStatistics;

/**
 * Get the root for all stats proc files.
 *
 * @return The proc root
 **/
static inline const char *getProcRoot(void) {
  return "vdo";
}

/**
 * Get the proc file path for reading KernelStatistics.
 *
 * @return The proc file path
 **/
static inline const char *getKernelStatisticsProcFile(void) {
  return "kernel_stats";
}

#endif /* not KERNEL_STATISTICS_H */
