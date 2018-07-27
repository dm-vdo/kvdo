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
 *
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabRebuild.c#2 $
 */

#include "slabRebuild.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockAllocatorInternals.h"
#include "extent.h"
#include "journalPoint.h"
#include "recoveryJournal.h"
#include "refCountsInternals.h"
#include "refCounts.h"
#include "slabCompletion.h"
#include "slabJournalInternals.h"
#include "slabSummary.h"

typedef struct {
  /** A completion for rebuilding a slab */
  VDOCompletion    completion;
  /** The slab to rebuild */
  Slab            *slab;
  /** The extent to load the slab journal blocks */
  VDOExtent       *extent;
  /** A buffer to store the slab journal blocks */
  char            *journalData;
} SlabRebuildCompletion;

/**
 * Convert a generic VDO completion to a SlabRebuildCompletion.
 *
 * @param completion  the completion to convert
 *
 * @return  the completion as a SlabRebuildCompletion
 **/
__attribute__((warn_unused_result))
static inline SlabRebuildCompletion *
asSlabRebuildCompletion(VDOCompletion *completion)
{
  if (completion == NULL) {
    return NULL;
  }
  STATIC_ASSERT(offsetof(SlabRebuildCompletion, completion) == 0);
  assertCompletionType(completion->type, SLAB_REBUILD_COMPLETION);
  return (SlabRebuildCompletion *) completion;
}

/**********************************************************************/
int makeSlabRebuildCompletion(PhysicalLayer  *layer,
                              BlockCount      slabJournalSize,
                              VDOCompletion **completionPtr)
{
  SlabRebuildCompletion *rebuild;
  int result = ALLOCATE(1, SlabRebuildCompletion, __func__, &rebuild);
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDOCompletion *completion = &rebuild->completion;
  initializeCompletion(completion, SLAB_REBUILD_COMPLETION, layer);

  size_t bufferSize = VDO_BLOCK_SIZE * slabJournalSize;
  result = ALLOCATE(bufferSize, char, __func__, &rebuild->journalData);
  if (result != VDO_SUCCESS) {
    freeSlabRebuildCompletion(&completion);
    return result;
  }

  result = createExtent(layer, VIO_TYPE_SLAB_JOURNAL, VIO_PRIORITY_METADATA,
                        slabJournalSize, rebuild->journalData,
                        &rebuild->extent);
  if (result != VDO_SUCCESS) {
    freeSlabRebuildCompletion(&completion);
    return result;
  }

  *completionPtr = completion;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabRebuildCompletion(VDOCompletion **completionPtr)
{
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(*completionPtr);
  if (rebuild == NULL) {
    return;
  }

  freeExtent(&rebuild->extent);
  FREE(rebuild->journalData);
  FREE(rebuild);
  *completionPtr = NULL;
}

/**
 * Fix up the slab's state and release it.
 *
 * @param rebuild  The slab rebuild completion
 **/
static void finishSlabRebuild(SlabRebuildCompletion *rebuild)
{
  Slab *slab = rebuild->slab;
  slab->status = SLAB_REBUILT;
  queueSlab(slab);
  reopenSlabJournal(slab->journal);
  finishCompletion(&rebuild->completion, VDO_SUCCESS);
}

/**
 * A callback to fix up the slab's state and release it. This is a callback
 * registered in applyJournalEntries().
 *
 * @param completion  The reference counts completion
 **/
static void finishSavingReferenceCountsCallback(VDOCompletion *completion)
{
  // The slab is now completely scrubbed.
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(completion->parent);
  finishSlabRebuild(rebuild);
}

/**
 * Apply all the entries in a block to the reference counts.
 *
 * @param block        A block with entries to apply
 * @param entryCount   The number of entries to apply
 * @param blockNumber  The sequence number of the block
 * @param slab         The slab to apply the entries to
 *
 * @return VDO_SUCCESS or an error code
 **/
static int applyBlockEntries(PackedSlabJournalBlock *block,
                             JournalEntryCount       entryCount,
                             SequenceNumber          blockNumber,
                             Slab                   *slab)
{
  JournalPoint entryPoint = {
    .sequenceNumber = blockNumber,
    .entryCount     = 0,
  };

  SlabBlockNumber maxSBN = slab->end - slab->start;
  while (entryPoint.entryCount < entryCount) {
    SlabJournalEntry entry = decodeSlabJournalEntry(block,
                                                    entryPoint.entryCount);
    if (entry.sbn > maxSBN) {
      // This entry is out of bounds.
      return logErrorWithStringError(VDO_CORRUPT_JOURNAL, "Slab journal entry"
                                     " (%" PRIu64 ", %u) had invalid offset"
                                     " %u in slab (size %u blocks)",
                                     blockNumber, entryPoint.entryCount,
                                     entry.sbn, maxSBN);
    }

    int result = replayReferenceCountChange(slab->referenceCounts, &entryPoint,
                                            entry);
    if (result != VDO_SUCCESS) {
      logErrorWithStringError(result, "Slab journal entry (%" PRIu64 ", %u)"
                              " (%s of offset %" PRIu32 ") could not be"
                              " applied in slab %u",
                              blockNumber, entryPoint.entryCount,
                              getJournalOperationName(entry.operation),
                              entry.sbn, slab->slabNumber);
      return result;
    }
    entryPoint.entryCount++;
  }

  return VDO_SUCCESS;
}

/**
 * Find the relevant extent of the slab journal and apply all valid entries.
 * This is a callback registered in startScrubbing().
 *
 * @param completion  The metadata read extent completion
 **/
static void applyJournalEntries(VDOCompletion *completion)
{
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(completion->parent);
  Slab        *slab            = rebuild->slab;
  SlabJournal *journal         = slab->journal;
  RefCounts   *referenceCounts = slab->referenceCounts;

  // Find the boundaries of the useful part of the journal.
  SequenceNumber  tail     = journal->tail;
  TailBlockOffset endIndex = getSlabJournalBlockOffset(journal, tail - 1);
  char *endData = rebuild->journalData + (endIndex * VDO_BLOCK_SIZE);
  PackedSlabJournalBlock *endBlock = (PackedSlabJournalBlock *) endData;

  SequenceNumber  head      = getUInt64LE(endBlock->header.fields.head);
  TailBlockOffset headIndex = getSlabJournalBlockOffset(journal, head);
  BlockCount      index     = headIndex;

  JournalPoint refCountsPoint   = referenceCounts->slabJournalPoint;
  JournalPoint lastEntryApplied = refCountsPoint;
  for (SequenceNumber sequence = head; sequence < tail; sequence++) {
    char *blockData = rebuild->journalData + (index * VDO_BLOCK_SIZE);
    PackedSlabJournalBlock *block  = (PackedSlabJournalBlock *) blockData;
    SlabJournalBlockHeader header;
    unpackSlabJournalBlockHeader(&block->header, &header);

    if ((header.nonce != slab->allocator->nonce)
        || (header.metadataType != VDO_METADATA_SLAB_JOURNAL)
        || (header.sequenceNumber != sequence)
        || (header.entryCount > journal->entriesPerBlock)
        || (header.hasBlockMapIncrements
            && (header.entryCount > journal->fullEntriesPerBlock))) {
      // The block is not what we expect it to be.
      logError("Slab journal block for slab %u was invalid",
               slab->slabNumber);
      finishCompletion(&rebuild->completion, VDO_CORRUPT_JOURNAL);
      return;
    }

    int result = applyBlockEntries(block, header.entryCount, sequence, slab);
    if (result != VDO_SUCCESS) {
      finishCompletion(&rebuild->completion, result);
      return;
    }

    lastEntryApplied.sequenceNumber = sequence;
    lastEntryApplied.entryCount     = header.entryCount - 1;
    index++;
    if (index == journal->size) {
      index = 0;
    }
  }

  // At the end of rebuild, the refCounts should be accurate to the end
  // of the journal we just applied.
  int result = ASSERT(!beforeJournalPoint(&lastEntryApplied, &refCountsPoint),
                      "Refcounts are not more accurate than the slab journal");
  if (result != VDO_SUCCESS) {
    finishCompletion(&rebuild->completion, result);
    return;
  }

  // The refCounts were not written out before, so mark them all dirty.
  if (!mustLoadRefCounts(journal->summary, slab->slabNumber)) {
    result = dirtyAllReferenceBlocks(referenceCounts);
    if (result != VDO_SUCCESS) {
      finishCompletion(&rebuild->completion, result);
      return;
    }
  }

  // Save all the reference count blocks to the layer.
  saveRebuiltSlab(slab, &rebuild->completion,
                  finishSavingReferenceCountsCallback,
                  finishParentCallback);
}

/**
 * Start the scrubbing by reading the journal blocks from disk if necessary.
 * This is a callback registered in flushSlabJournalForRebuild().
 *
 * @param completion  The slab journal completion from flushing
 **/
static void startScrubbing(VDOCompletion *completion)
{
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(completion->parent);
  Slab *slab = rebuild->slab;
  if (getSummarizedCleanliness(slab->allocator->summary, slab->slabNumber)) {
    finishSlabRebuild(rebuild);
    return;
  }

  prepareCompletion(&rebuild->extent->completion, applyJournalEntries,
                    finishParentCallback, completion->callbackThreadID,
                    completion->parent);
  readMetadataExtent(rebuild->extent, rebuild->slab->journalOrigin);
}

/**
 * Flush the slab journal.
 *
 * @param rebuild  The slab rebuild completion
 **/
static void flushSlabJournalForRebuild(SlabRebuildCompletion *rebuild)
{
  flushSlabJournal(rebuild->slab->journal, &rebuild->completion,
                   startScrubbing, finishParentCallback,
                   rebuild->completion.callbackThreadID);
}

/**
 * Start scrubbing the slab. This is a callback registered in scrubSlab().
 *
 * @param completion  The slab journal completion from flushing
 **/
static void finishLoadingReferenceBlocks(VDOCompletion *completion)
{
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(completion->parent);
  flushSlabJournalForRebuild(rebuild);
}

/**********************************************************************/
void scrubSlab(Slab *slab, VDOCompletion *completion)
{
  SlabRebuildCompletion *rebuild = asSlabRebuildCompletion(completion);
  rebuild->slab                  = slab;
  slab->status                   = SLAB_REBUILDING;

  if (mustLoadRefCounts(slab->allocator->summary, slab->slabNumber)) {
    loadReferenceBlocks(slab->referenceCounts, &rebuild->completion,
                        finishLoadingReferenceBlocks, finishParentCallback,
                        rebuild->completion.callbackThreadID);
    return;
  }

  flushSlabJournalForRebuild(rebuild);
}
