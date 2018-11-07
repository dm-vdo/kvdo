/*
 * Copyright (c) 2018 Red Hat, Inc.
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
  STATISTICS_VERSION = 30,
};

typedef struct {
  /** The total number of slabs from which blocks may be allocated */
  uint64_t slabCount;
  /** The total number of slabs from which blocks have ever been allocated */
  uint64_t slabsOpened;
  /** The number of times since loading that a slab has been re-opened */
  uint64_t slabsReopened;
} BlockAllocatorStatistics;

/**
 * Counters for tracking the number of items written (blocks, requests, etc.)
 * that keep track of totals at steps in the write pipeline. Three counters
 * allow the number of buffered, in-memory items and the number of in-flight,
 * unacknowledged writes to be derived, while still tracking totals for
 * reporting purposes
 **/
typedef struct {
  /** The total number of items on which processing has started */
  uint64_t started;
  /** The total number of items for which a write operation has been issued */
  uint64_t written;
  /** The total number of items for which a write operation has completed */
  uint64_t committed;
} CommitStatistics;

/** Counters for events in the recovery journal */
typedef struct {
  /** Number of times the on-disk journal was full */
  uint64_t diskFull;
  /** Number of times the recovery journal requested slab journal commits. */
  uint64_t slabJournalCommitsRequested;
  /** Write/Commit totals for individual journal entries */
  CommitStatistics entries;
  /** Write/Commit totals for journal blocks */
  CommitStatistics blocks;
} RecoveryJournalStatistics;

/** The statistics for the compressed block packer. */
typedef struct {
  /** Number of compressed data items written since startup */
  uint64_t compressedFragmentsWritten;
  /** Number of blocks containing compressed items written since startup */
  uint64_t compressedBlocksWritten;
  /** Number of VIOs that are pending in the packer */
  uint64_t compressedFragmentsInPacker;
} PackerStatistics;

/** The statistics for the slab journals. */
typedef struct {
  /** Number of times the on-disk journal was full */
  uint64_t diskFullCount;
  /** Number of times an entry was added over the flush threshold */
  uint64_t flushCount;
  /** Number of times an entry was added over the block threshold */
  uint64_t blockedCount;
  /** Number of times a tail block was written */
  uint64_t blocksWritten;
  /** Number of times we had to wait for the tail to write */
  uint64_t tailBusyCount;
} SlabJournalStatistics;

/** The statistics for the slab summary. */
typedef struct {
  /** Number of blocks written */
  uint64_t blocksWritten;
} SlabSummaryStatistics;

/** The statistics for the reference counts. */
typedef struct {
  /** Number of reference blocks written */
  uint64_t blocksWritten;
} RefCountsStatistics;

/** The statistics for the block map. */
typedef struct {
  /** number of dirty (resident) pages */
  uint32_t dirtyPages;
  /** number of clean (resident) pages */
  uint32_t cleanPages;
  /** number of free pages */
  uint32_t freePages;
  /** number of pages in failed state */
  uint32_t failedPages;
  /** number of pages incoming */
  uint32_t incomingPages;
  /** number of pages outgoing */
  uint32_t outgoingPages;
  /** how many times free page not avail */
  uint32_t cachePressure;
  /** number of getVDOPageAsync() for read */
  uint64_t readCount;
  /** number or getVDOPageAsync() for write */
  uint64_t writeCount;
  /** number of times pages failed to read */
  uint64_t failedReads;
  /** number of times pages failed to write */
  uint64_t failedWrites;
  /** number of gets that are reclaimed */
  uint64_t reclaimed;
  /** number of gets for outgoing pages */
  uint64_t readOutgoing;
  /** number of gets that were already there */
  uint64_t foundInCache;
  /** number of gets requiring discard */
  uint64_t discardRequired;
  /** number of gets enqueued for their page */
  uint64_t waitForPage;
  /** number of gets that have to fetch */
  uint64_t fetchRequired;
  /** number of page fetches */
  uint64_t pagesLoaded;
  /** number of page saves */
  uint64_t pagesSaved;
  /** the number of flushes issued */
  uint64_t flushCount;
} BlockMapStatistics;

/** The dedupe statistics from hash locks */
typedef struct {
  /** Number of times the UDS advice proved correct */
  uint64_t dedupeAdviceValid;
  /** Number of times the UDS advice proved incorrect */
  uint64_t dedupeAdviceStale;
  /** Number of writes with the same data as another in-flight write */
  uint64_t concurrentDataMatches;
  /** Number of writes whose hash collided with an in-flight write */
  uint64_t concurrentHashCollisions;
} HashLockStatistics;

/** Counts of error conditions in VDO. */
typedef struct {
  /** number of times VDO got an invalid dedupe advice PBN from UDS */
  uint64_t invalidAdvicePBNCount;
  /** number of times a VIO completed with a VDO_NO_SPACE error */
  uint64_t noSpaceErrorCount;
  /** number of times a VIO completed with a VDO_READ_ONLY error */
  uint64_t readOnlyErrorCount;
} ErrorStatistics;

/** The statistics of the vdo service. */
struct vdoStatistics {
  uint32_t version;
  uint32_t releaseVersion;
  /** Number of blocks used for data */
  uint64_t dataBlocksUsed;
  /** Number of blocks used for VDO metadata */
  uint64_t overheadBlocksUsed;
  /** Number of logical blocks that are currently mapped to physical blocks */
  uint64_t logicalBlocksUsed;
  /** number of physical blocks */
  BlockCount physicalBlocks;
  /** number of logical blocks */
  BlockCount logicalBlocks;
  /** Size of the block map page cache, in bytes */
  uint64_t blockMapCacheSize;
  /** String describing the active write policy of the VDO */
  char writePolicy[15];
  /** The physical block size */
  uint64_t blockSize;
  /** Number of times the VDO has successfully recovered */
  uint64_t completeRecoveries;
  /** Number of times the VDO has recovered from read-only mode */
  uint64_t readOnlyRecoveries;
  /** String describing the operating mode of the VDO */
  char mode[15];
  /** Whether the VDO is in recovery mode */
  bool inRecoveryMode;
  /** What percentage of recovery mode work has been completed */
  uint8_t recoveryPercentage;
  /** The statistics for the compressed block packer */
  PackerStatistics packer;
  /** Counters for events in the block allocator */
  BlockAllocatorStatistics allocator;
  /** Counters for events in the recovery journal */
  RecoveryJournalStatistics journal;
  /** The statistics for the slab journals */
  SlabJournalStatistics slabJournal;
  /** The statistics for the slab summary */
  SlabSummaryStatistics slabSummary;
  /** The statistics for the reference counts */
  RefCountsStatistics refCounts;
  /** The statistics for the block map */
  BlockMapStatistics blockMap;
  /** The dedupe statistics from hash locks */
  HashLockStatistics hashLock;
  /** Counts of error conditions */
  ErrorStatistics errors;
};

/**
 * Get the proc file path for reading VDOStatistics.
 *
 * @return The proc file path
 **/
static inline const char *getVDOStatisticsProcFile(void) {
  return "dedupe_stats";
}

#endif /* not STATISTICS_H */
