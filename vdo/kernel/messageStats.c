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

#include "commonStats.h"
#include "dedupeIndex.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "messageStats.h"
#include "statistics.h"
#include "threadDevice.h"
#include "vdo.h"

/**********************************************************************/
int write_uint64_t(char *prefix,
                   uint64_t value,
                   char *suffix,
                   char **buf,
                   unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%llu%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_uint32_t(char *prefix,
                   uint32_t value,
                   char *suffix,
                   char **buf,
                   unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%" PRIu32 "%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_BlockCount(char *prefix,
                     BlockCount value,
                     char *suffix,
                     char **buf,
                     unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%llu%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_string(char *prefix,
                 char *value,
                 char *suffix,
                 char **buf,
                 unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%s%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_bool(char *prefix,
               bool value,
               char *suffix,
               char **buf,
               unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%d%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_uint8_t(char *prefix,
                  uint8_t value,
                  char *suffix,
                  char **buf,
                  unsigned int *maxlen)
{
       int count = snprintf(*buf, *maxlen, "%s%u%s",
                            prefix == NULL ? "" : prefix,
                            value,
                            suffix == NULL ? "" : suffix);
       *buf += count;
       *maxlen -= count;
       if (count >= *maxlen) {
               return VDO_UNEXPECTED_EOF;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_block_allocator_statistics(char *prefix,
                                     struct block_allocator_statistics *stats,
                                     char *suffix,
                                     char **buf,
                                     unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The total number of slabs from which blocks may be allocated */
       result = write_uint64_t("slabCount : ",
                               stats->slabCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The total number of slabs from which blocks have ever been allocated */
       result = write_uint64_t("slabsOpened : ",
                               stats->slabsOpened,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The number of times since loading that a slab has been re-opened */
       result = write_uint64_t("slabsReopened : ",
                               stats->slabsReopened,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_commit_statistics(char *prefix,
                            struct commit_statistics *stats,
                            char *suffix,
                            char **buf,
                            unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The total number of items on which processing has started */
       result = write_uint64_t("started : ",
                               stats->started,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The total number of items for which a write operation has been issued */
       result = write_uint64_t("written : ",
                               stats->written,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The total number of items for which a write operation has completed */
       result = write_uint64_t("committed : ",
                               stats->committed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_recovery_journal_statistics(char *prefix,
                                      struct recovery_journal_statistics *stats,
                                      char *suffix,
                                      char **buf,
                                      unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the on-disk journal was full */
       result = write_uint64_t("diskFull : ",
                               stats->diskFull,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the recovery journal requested slab journal commits. */
       result = write_uint64_t("slabJournalCommitsRequested : ",
                               stats->slabJournalCommitsRequested,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Write/Commit totals for individual journal entries */
       result = write_commit_statistics("entries : ",
                                        &stats->entries,
                                        ", ",
                                        buf,
                                        maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Write/Commit totals for journal blocks */
       result = write_commit_statistics("blocks : ",
                                        &stats->blocks,
                                        ", ",
                                        buf,
                                        maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_packer_statistics(char *prefix,
                            struct packer_statistics *stats,
                            char *suffix,
                            char **buf,
                            unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of compressed data items written since startup */
       result = write_uint64_t("compressedFragmentsWritten : ",
                               stats->compressedFragmentsWritten,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of blocks containing compressed items written since startup */
       result = write_uint64_t("compressedBlocksWritten : ",
                               stats->compressedBlocksWritten,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of VIOs that are pending in the packer */
       result = write_uint64_t("compressedFragmentsInPacker : ",
                               stats->compressedFragmentsInPacker,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_slab_journal_statistics(char *prefix,
                                  struct slab_journal_statistics *stats,
                                  char *suffix,
                                  char **buf,
                                  unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the on-disk journal was full */
       result = write_uint64_t("diskFullCount : ",
                               stats->diskFullCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times an entry was added over the flush threshold */
       result = write_uint64_t("flushCount : ",
                               stats->flushCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times an entry was added over the block threshold */
       result = write_uint64_t("blockedCount : ",
                               stats->blockedCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times a tail block was written */
       result = write_uint64_t("blocksWritten : ",
                               stats->blocksWritten,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times we had to wait for the tail to write */
       result = write_uint64_t("tailBusyCount : ",
                               stats->tailBusyCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_slab_summary_statistics(char *prefix,
                                  struct slab_summary_statistics *stats,
                                  char *suffix,
                                  char **buf,
                                  unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of blocks written */
       result = write_uint64_t("blocksWritten : ",
                               stats->blocksWritten,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_ref_counts_statistics(char *prefix,
                                struct ref_counts_statistics *stats,
                                char *suffix,
                                char **buf,
                                unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of reference blocks written */
       result = write_uint64_t("blocksWritten : ",
                               stats->blocksWritten,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_block_map_statistics(char *prefix,
                               struct block_map_statistics *stats,
                               char *suffix,
                               char **buf,
                               unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of dirty (resident) pages */
       result = write_uint32_t("dirtyPages : ",
                               stats->dirtyPages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of clean (resident) pages */
       result = write_uint32_t("cleanPages : ",
                               stats->cleanPages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of free pages */
       result = write_uint32_t("freePages : ",
                               stats->freePages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of pages in failed state */
       result = write_uint32_t("failedPages : ",
                               stats->failedPages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of pages incoming */
       result = write_uint32_t("incomingPages : ",
                               stats->incomingPages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of pages outgoing */
       result = write_uint32_t("outgoingPages : ",
                               stats->outgoingPages,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** how many times free page not avail */
       result = write_uint32_t("cachePressure : ",
                               stats->cachePressure,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of getVDOPageAsync() for read */
       result = write_uint64_t("readCount : ",
                               stats->readCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number or getVDOPageAsync() for write */
       result = write_uint64_t("writeCount : ",
                               stats->writeCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of times pages failed to read */
       result = write_uint64_t("failedReads : ",
                               stats->failedReads,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of times pages failed to write */
       result = write_uint64_t("failedWrites : ",
                               stats->failedWrites,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets that are reclaimed */
       result = write_uint64_t("reclaimed : ",
                               stats->reclaimed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets for outgoing pages */
       result = write_uint64_t("readOutgoing : ",
                               stats->readOutgoing,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets that were already there */
       result = write_uint64_t("foundInCache : ",
                               stats->foundInCache,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets requiring discard */
       result = write_uint64_t("discardRequired : ",
                               stats->discardRequired,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets enqueued for their page */
       result = write_uint64_t("waitForPage : ",
                               stats->waitForPage,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of gets that have to fetch */
       result = write_uint64_t("fetchRequired : ",
                               stats->fetchRequired,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of page fetches */
       result = write_uint64_t("pagesLoaded : ",
                               stats->pagesLoaded,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of page saves */
       result = write_uint64_t("pagesSaved : ",
                               stats->pagesSaved,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** the number of flushes issued */
       result = write_uint64_t("flushCount : ",
                               stats->flushCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_HashLockStatistics(char *prefix,
                             HashLockStatistics *stats,
                             char *suffix,
                             char **buf,
                             unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the UDS advice proved correct */
       result = write_uint64_t("dedupeAdviceValid : ",
                               stats->dedupeAdviceValid,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the UDS advice proved incorrect */
       result = write_uint64_t("dedupeAdviceStale : ",
                               stats->dedupeAdviceStale,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of writes with the same data as another in-flight write */
       result = write_uint64_t("concurrentDataMatches : ",
                               stats->concurrentDataMatches,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of writes whose hash collided with an in-flight write */
       result = write_uint64_t("concurrentHashCollisions : ",
                               stats->concurrentHashCollisions,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_ErrorStatistics(char *prefix,
                          ErrorStatistics *stats,
                          char *suffix,
                          char **buf,
                          unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of times VDO got an invalid dedupe advice PBN from UDS */
       result = write_uint64_t("invalidAdvicePBNCount : ",
                               stats->invalidAdvicePBNCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of times a VIO completed with a VDO_NO_SPACE error */
       result = write_uint64_t("noSpaceErrorCount : ",
                               stats->noSpaceErrorCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of times a VIO completed with a VDO_READ_ONLY error */
       result = write_uint64_t("readOnlyErrorCount : ",
                               stats->readOnlyErrorCount,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_vdoStatistics(char *prefix,
                        struct vdoStatistics *stats,
                        char *suffix,
                        char **buf,
                        unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_uint32_t("version : ",
                               stats->version,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_uint32_t("releaseVersion : ",
                               stats->releaseVersion,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of blocks used for data */
       result = write_uint64_t("dataBlocksUsed : ",
                               stats->dataBlocksUsed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of blocks used for VDO metadata */
       result = write_uint64_t("overheadBlocksUsed : ",
                               stats->overheadBlocksUsed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of logical blocks that are currently mapped to physical blocks */
       result = write_uint64_t("logicalBlocksUsed : ",
                               stats->logicalBlocksUsed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of physical blocks */
       result = write_BlockCount("physicalBlocks : ",
                                 stats->physicalBlocks,
                                 ", ",
                                 buf,
                                 maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** number of logical blocks */
       result = write_BlockCount("logicalBlocks : ",
                                 stats->logicalBlocks,
                                 ", ",
                                 buf,
                                 maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Size of the block map page cache, in bytes */
       result = write_uint64_t("blockMapCacheSize : ",
                               stats->blockMapCacheSize,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** String describing the active write policy of the VDO */
       result = write_string("writePolicy : ",
                             stats->writePolicy,
                             ", ",
                             buf,
                             maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The physical block size */
       result = write_uint64_t("blockSize : ",
                               stats->blockSize,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the VDO has successfully recovered */
       result = write_uint64_t("completeRecoveries : ",
                               stats->completeRecoveries,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the VDO has recovered from read-only mode */
       result = write_uint64_t("readOnlyRecoveries : ",
                               stats->readOnlyRecoveries,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** String describing the operating mode of the VDO */
       result = write_string("mode : ",
                             stats->mode,
                             ", ",
                             buf,
                             maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Whether the VDO is in recovery mode */
       result = write_bool("inRecoveryMode : ",
                           stats->inRecoveryMode,
                           ", ",
                           buf,
                           maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** What percentage of recovery mode work has been completed */
       result = write_uint8_t("recoveryPercentage : ",
                              stats->recoveryPercentage,
                              ", ",
                              buf,
                              maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the compressed block packer */
       result = write_packer_statistics("packer : ",
                                        &stats->packer,
                                        ", ",
                                        buf,
                                        maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Counters for events in the block allocator */
       result = write_block_allocator_statistics("allocator : ",
                                                 &stats->allocator,
                                                 ", ",
                                                 buf,
                                                 maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Counters for events in the recovery journal */
       result = write_recovery_journal_statistics("journal : ",
                                                  &stats->journal,
                                                  ", ",
                                                  buf,
                                                  maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the slab journals */
       result = write_slab_journal_statistics("slabJournal : ",
                                              &stats->slabJournal,
                                              ", ",
                                              buf,
                                              maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the slab summary */
       result = write_slab_summary_statistics("slabSummary : ",
                                              &stats->slabSummary,
                                              ", ",
                                              buf,
                                              maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the reference counts */
       result = write_ref_counts_statistics("refCounts : ",
                                            &stats->refCounts,
                                            ", ",
                                            buf,
                                            maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the block map */
       result = write_block_map_statistics("blockMap : ",
                                           &stats->blockMap,
                                           ", ",
                                           buf,
                                           maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The dedupe statistics from hash locks */
       result = write_HashLockStatistics("hashLock : ",
                                         &stats->hashLock,
                                         ", ",
                                         buf,
                                         maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Counts of error conditions */
       result = write_ErrorStatistics("errors : ",
                                      &stats->errors,
                                      ", ",
                                      buf,
                                      maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_vdo_stats(struct kernel_layer *layer,
                     char *buf,
                     unsigned int maxlen)
{
       struct vdoStatistics *stats;
       int result = ALLOCATE(1, struct vdoStatistics, __func__, &stats);
       if (result != VDO_SUCCESS) {
              return result;
       }

       get_kvdo_statistics(&layer->kvdo, stats);
       result = write_vdoStatistics(NULL, stats, NULL, &buf, &maxlen);
       FREE(stats);
       return result;
}

/**********************************************************************/
int write_bio_stats(char *prefix,
                    struct bio_stats *stats,
                    char *suffix,
                    char **buf,
                    unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of not REQ_WRITE bios */
       result = write_uint64_t("read : ",
                               stats->read,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of REQ_WRITE bios */
       result = write_uint64_t("write : ",
                               stats->write,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of REQ_DISCARD bios */
       result = write_uint64_t("discard : ",
                               stats->discard,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of REQ_FLUSH bios */
       result = write_uint64_t("flush : ",
                               stats->flush,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of REQ_FUA bios */
       result = write_uint64_t("fua : ",
                               stats->fua,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_memory_usage(char *prefix,
                       struct memory_usage *stats,
                       char *suffix,
                       char **buf,
                       unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Tracked bytes currently allocated. */
       result = write_uint64_t("bytesUsed : ",
                               stats->bytesUsed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Maximum tracked bytes allocated. */
       result = write_uint64_t("peakBytesUsed : ",
                               stats->peakBytesUsed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_index_statistics(char *prefix,
                           struct index_statistics *stats,
                           char *suffix,
                           char **buf,
                           unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of chunk names stored in the index */
       result = write_uint64_t("entriesIndexed : ",
                               stats->entriesIndexed,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of post calls that found an existing entry */
       result = write_uint64_t("postsFound : ",
                               stats->postsFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of post calls that added a new entry */
       result = write_uint64_t("postsNotFound : ",
                               stats->postsNotFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of query calls that found an existing entry */
       result = write_uint64_t("queriesFound : ",
                               stats->queriesFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of query calls that added a new entry */
       result = write_uint64_t("queriesNotFound : ",
                               stats->queriesNotFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of update calls that found an existing entry */
       result = write_uint64_t("updatesFound : ",
                               stats->updatesFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of update calls that added a new entry */
       result = write_uint64_t("updatesNotFound : ",
                               stats->updatesNotFound,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Current number of dedupe queries that are in flight */
       result = write_uint32_t("currDedupeQueries : ",
                               stats->currDedupeQueries,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Maximum number of dedupe queries that have been in flight */
       result = write_uint32_t("maxDedupeQueries : ",
                               stats->maxDedupeQueries,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_kernel_statistics(char *prefix,
                            struct kernel_statistics *stats,
                            char *suffix,
                            char **buf,
                            unsigned int *maxlen)
{
       int result = write_string(prefix, "{ ", NULL, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_uint32_t("version : ",
                               stats->version,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_uint32_t("releaseVersion : ",
                               stats->releaseVersion,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The VDO instance */
       result = write_uint32_t("instance : ",
                               stats->instance,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Current number of active VIOs */
       result = write_uint32_t("currentVIOsInProgress : ",
                               stats->currentVIOsInProgress,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Maximum number of active VIOs */
       result = write_uint32_t("maxVIOs : ",
                               stats->maxVIOs,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of times the UDS index was too slow in responding */
       result = write_uint64_t("dedupeAdviceTimeouts : ",
                               stats->dedupeAdviceTimeouts,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Number of flush requests submitted to the storage device */
       result = write_uint64_t("flushOut : ",
                               stats->flushOut,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Logical block size */
       result = write_uint64_t("logicalBlockSize : ",
                               stats->logicalBlockSize,
                               ", ",
                               buf,
                               maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Bios submitted into VDO from above */
       result = write_bio_stats("biosIn : ",
                                &stats->biosIn,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosInPartial : ",
                                &stats->biosInPartial,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Bios submitted onward for user data */
       result = write_bio_stats("biosOut : ",
                                &stats->biosOut,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Bios submitted onward for metadata */
       result = write_bio_stats("biosMeta : ",
                                &stats->biosMeta,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosJournal : ",
                                &stats->biosJournal,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosPageCache : ",
                                &stats->biosPageCache,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosOutCompleted : ",
                                &stats->biosOutCompleted,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosMetaCompleted : ",
                                &stats->biosMetaCompleted,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosJournalCompleted : ",
                                &stats->biosJournalCompleted,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosPageCacheCompleted : ",
                                &stats->biosPageCacheCompleted,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosAcknowledged : ",
                                &stats->biosAcknowledged,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_bio_stats("biosAcknowledgedPartial : ",
                                &stats->biosAcknowledgedPartial,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Current number of bios in progress */
       result = write_bio_stats("biosInProgress : ",
                                &stats->biosInProgress,
                                ", ",
                                buf,
                                maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** Memory usage stats. */
       result = write_memory_usage("memoryUsage : ",
                                   &stats->memoryUsage,
                                   ", ",
                                   buf,
                                   maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       /** The statistics for the UDS index */
       result = write_index_statistics("index : ",
                                       &stats->index,
                                       ", ",
                                       buf,
                                       maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       result = write_string(NULL, "}", suffix, buf, maxlen);
       if (result != VDO_SUCCESS) {
              return result;
       }
       return VDO_SUCCESS;
}

/**********************************************************************/
int write_kernel_stats(struct kernel_layer *layer,
                        char *buf,
                        unsigned int maxlen)
{
       struct kernel_statistics *stats;
       int result = ALLOCATE(1, struct kernel_statistics, __func__, &stats);
       if (result != VDO_SUCCESS) {
              return result;
       }

       get_kernel_statistics(layer, stats);
       result = write_kernel_statistics(NULL, stats, NULL, &buf, &maxlen);
       FREE(stats);
       return result;
}
