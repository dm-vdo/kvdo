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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabScrubber.c#10 $
 */

#include "slabScrubberInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "blockAllocator.h"
#include "constants.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "refCountsInternals.h"
#include "slab.h"
#include "slabJournalInternals.h"

/**
 * Allocate the buffer and extent used for reading the slab journal when
 * scrubbing a slab.
 *
 * @param scrubber         The slab scrubber for which to allocate
 * @param layer            The physical layer on which the scrubber resides
 * @param slabJournalSize  The size of a slab journal
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int allocateExtentAndBuffer(SlabScrubber  *scrubber,
                                   PhysicalLayer *layer,
                                   BlockCount     slabJournalSize)
{
  size_t bufferSize = VDO_BLOCK_SIZE * slabJournalSize;
  int result = ALLOCATE(bufferSize, char, __func__, &scrubber->journalData);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return createExtent(layer, VIO_TYPE_SLAB_JOURNAL, VIO_PRIORITY_METADATA,
                      slabJournalSize, scrubber->journalData,
                      &scrubber->extent);
}

/**********************************************************************/
int makeSlabScrubber(PhysicalLayer              *layer,
                     BlockCount                  slabJournalSize,
                     struct read_only_notifier  *readOnlyNotifier,
                     SlabScrubber              **scrubberPtr)
{
  SlabScrubber *scrubber;
  int result = ALLOCATE(1, SlabScrubber, __func__, &scrubber);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = allocateExtentAndBuffer(scrubber, layer, slabJournalSize);
  if (result != VDO_SUCCESS) {
    freeSlabScrubber(&scrubber);
    return result;
  }

  initializeCompletion(&scrubber->completion, SLAB_SCRUBBER_COMPLETION, layer);
  initializeRing(&scrubber->highPrioritySlabs);
  initializeRing(&scrubber->slabs);
  scrubber->readOnlyNotifier = readOnlyNotifier;
  scrubber->adminState.state = ADMIN_STATE_SUSPENDED;
  *scrubberPtr               = scrubber;
  return VDO_SUCCESS;
}

/**
 * Free the extent and buffer used for reading slab journals.
 *
 * @param scrubber  The scrubber
 **/
static void freeExtentAndBuffer(SlabScrubber *scrubber)
{
  freeExtent(&scrubber->extent);
  if (scrubber->journalData != NULL) {
    FREE(scrubber->journalData);
    scrubber->journalData = NULL;
  }
}

/**********************************************************************/
void freeSlabScrubber(SlabScrubber **scrubberPtr)
{
  if (*scrubberPtr == NULL) {
    return;
  }

  SlabScrubber *scrubber = *scrubberPtr;
  freeExtentAndBuffer(scrubber);
  FREE(scrubber);
  *scrubberPtr = NULL;
}

/**
 * Get the next slab to scrub.
 *
 * @param scrubber  The slab scrubber
 *
 * @return The next slab to scrub or <code>NULL</code> if there are none
 **/
static Slab *getNextSlab(SlabScrubber *scrubber)
{
  if (!isRingEmpty(&scrubber->highPrioritySlabs)) {
    return slabFromRingNode(scrubber->highPrioritySlabs.next);
  }

  if (!isRingEmpty(&scrubber->slabs)) {
    return slabFromRingNode(scrubber->slabs.next);
  }

  return NULL;
}

/**********************************************************************/
bool hasSlabsToScrub(SlabScrubber *scrubber)
{
  return (getNextSlab(scrubber) != NULL);
}

/**********************************************************************/
SlabCount getScrubberSlabCount(const SlabScrubber *scrubber)
{
  return relaxedLoad64(&scrubber->slabCount);
}

/**********************************************************************/
void registerSlabForScrubbing(SlabScrubber *scrubber,
                              Slab         *slab,
                              bool          highPriority)
{
  ASSERT_LOG_ONLY((slab->status != SLAB_REBUILT),
                  "slab to be scrubbed is unrecovered");

  if (slab->status != SLAB_REQUIRES_SCRUBBING) {
    return;
  }

  unspliceRingNode(&slab->ringNode);
  if (!slab->wasQueuedForScrubbing) {
    relaxedAdd64(&scrubber->slabCount, 1);
    slab->wasQueuedForScrubbing = true;
  }

  if (highPriority) {
    slab->status = SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING;
    pushRingNode(&scrubber->highPrioritySlabs, &slab->ringNode);
    return;
  }

  pushRingNode(&scrubber->slabs, &slab->ringNode);
}

/**
 * Stop scrubbing, either because there are no more slabs to scrub or because
 * there's been an error.
 *
 * @param scrubber  The scrubber
 **/
static void finishScrubbing(SlabScrubber *scrubber)
{
  if (!hasSlabsToScrub(scrubber)) {
    freeExtentAndBuffer(scrubber);
  }

  // Inform whoever is waiting that scrubbing has completed.
  completeCompletion(&scrubber->completion);

  bool notify = hasWaiters(&scrubber->waiters);

  // Note that the scrubber has stopped, and inform anyone who might be waiting
  // for that to happen.
  if (!finishDraining(&scrubber->adminState)) {
    scrubber->adminState.state = ADMIN_STATE_SUSPENDED;
  }

  /*
   * We can't notify waiters until after we've finished draining or they'll
   * just requeue. Fortunately if there were waiters, we can't have been freed
   * yet.
   */
  if (notify) {
    notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  }
}

/**********************************************************************/
static void scrubNextSlab(SlabScrubber *scrubber);

/**
 * Notify the scrubber that a slab has been scrubbed. This callback is
 * registered in applyJournalEntries().
 *
 * @param completion  The slab rebuild completion
 **/
static void slabScrubbed(VDOCompletion *completion)
{
  SlabScrubber *scrubber = completion->parent;
  finishScrubbingSlab(scrubber->slab);
  relaxedAdd64(&scrubber->slabCount, -1);
  scrubNextSlab(scrubber);
}

/**
 * Abort scrubbing due to an error.
 *
 * @param scrubber  The slab scrubber
 * @param result    The error
 **/
static void abortScrubbing(SlabScrubber *scrubber, int result)
{
  enterReadOnlyMode(scrubber->readOnlyNotifier, result);
  setCompletionResult(&scrubber->completion, result);
  scrubNextSlab(scrubber);
}

/**
 * Handle errors while rebuilding a slab.
 *
 * @param completion  The slab rebuild completion
 **/
static void handleScrubberError(VDOCompletion *completion)
{
  abortScrubbing(completion->parent, completion->result);
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
static int applyBlockEntries(struct packed_slab_journal_block *block,
                             JournalEntryCount                 entryCount,
                             SequenceNumber                    blockNumber,
                             Slab                             *slab)
{
  struct journal_point entryPoint = {
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
                                     " (%llu, %u) had invalid offset"
                                     " %u in slab (size %u blocks)",
                                     blockNumber, entryPoint.entryCount,
                                     entry.sbn, maxSBN);
    }

    int result = replayReferenceCountChange(slab->referenceCounts, &entryPoint,
                                            entry);
    if (result != VDO_SUCCESS) {
      logErrorWithStringError(result, "Slab journal entry (%llu, %u)"
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
  SlabScrubber      *scrubber        = completion->parent;
  Slab              *slab            = scrubber->slab;
  SlabJournal       *journal         = slab->journal;
  struct ref_counts *referenceCounts = slab->referenceCounts;

  // Find the boundaries of the useful part of the journal.
  SequenceNumber  tail     = journal->tail;
  TailBlockOffset endIndex = getSlabJournalBlockOffset(journal, tail - 1);
  char *endData = scrubber->journalData + (endIndex * VDO_BLOCK_SIZE);
  struct packed_slab_journal_block *endBlock
    = (struct packed_slab_journal_block *) endData;

  SequenceNumber  head      = getUInt64LE(endBlock->header.fields.head);
  TailBlockOffset headIndex = getSlabJournalBlockOffset(journal, head);
  BlockCount      index     = headIndex;

  struct journal_point refCountsPoint   = referenceCounts->slabJournalPoint;
  struct journal_point lastEntryApplied = refCountsPoint;
  for (SequenceNumber sequence = head; sequence < tail; sequence++) {
    char *blockData = scrubber->journalData + (index * VDO_BLOCK_SIZE);
    struct packed_slab_journal_block *block
      = (struct packed_slab_journal_block *) blockData;
    struct slab_journal_block_header header;
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
      abortScrubbing(scrubber, VDO_CORRUPT_JOURNAL);
      return;
    }

    int result = applyBlockEntries(block, header.entryCount, sequence, slab);
    if (result != VDO_SUCCESS) {
      abortScrubbing(scrubber, result);
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
    abortScrubbing(scrubber, result);
    return;
  }

  // Save out the rebuilt reference blocks.
  prepareCompletion(completion, slabScrubbed, handleScrubberError,
                    completion->callbackThreadID, scrubber);
  startSlabAction(slab, ADMIN_STATE_SAVE_FOR_SCRUBBING, completion);
}

/**
 * Read the current slab's journal from disk now that it has been flushed.
 * This callback is registered in scrubNextSlab().
 *
 * @param completion  The scrubber's extent completion
 **/
static void startScrubbing(VDOCompletion *completion)
{
  SlabScrubber *scrubber = completion->parent;
  Slab         *slab     = scrubber->slab;
  if (getSummarizedCleanliness(slab->allocator->summary, slab->slabNumber)) {
    slabScrubbed(completion);
    return;
  }

  prepareCompletion(&scrubber->extent->completion, applyJournalEntries,
                    handleScrubberError, completion->callbackThreadID,
                    completion->parent);
  readMetadataExtent(scrubber->extent, slab->journalOrigin);
}

/**
 * Scrub the next slab if there is one.
 *
 * @param scrubber  The scrubber
 **/
static void scrubNextSlab(SlabScrubber *scrubber)
{
  // Note: this notify call is always safe only because scrubbing can only
  // be started when the VDO is quiescent.
  notifyAllWaiters(&scrubber->waiters, NULL, NULL);
  if (isReadOnly(scrubber->readOnlyNotifier)) {
    setCompletionResult(&scrubber->completion, VDO_READ_ONLY);
    finishScrubbing(scrubber);
    return;
  }

  Slab *slab = getNextSlab(scrubber);
  if ((slab == NULL)
      || (scrubber->highPriorityOnly
          && isRingEmpty(&scrubber->highPrioritySlabs))) {
    scrubber->highPriorityOnly = false;
    finishScrubbing(scrubber);
    return;
  }

  if (finishDraining(&scrubber->adminState)) {
    return;
  }

  unspliceRingNode(&slab->ringNode);
  scrubber->slab = slab;
  VDOCompletion *completion = extentAsCompletion(scrubber->extent);
  prepareCompletion(completion, startScrubbing,
                    handleScrubberError, scrubber->completion.callbackThreadID,
                    scrubber);
  startSlabAction(slab, ADMIN_STATE_SCRUBBING, completion);
}

/**********************************************************************/
void scrubSlabs(SlabScrubber *scrubber,
                void         *parent,
                VDOAction    *callback,
                VDOAction    *errorHandler)
{
  resumeIfQuiescent(&scrubber->adminState);
  ThreadID threadID = getCallbackThreadID();
  prepareCompletion(&scrubber->completion, callback, errorHandler, threadID,
                    parent);
  if (!hasSlabsToScrub(scrubber)) {
    finishScrubbing(scrubber);
    return;
  }

  scrubNextSlab(scrubber);
}

/**********************************************************************/
void scrubHighPrioritySlabs(SlabScrubber  *scrubber,
                            bool           scrubAtLeastOne,
                            VDOCompletion *parent,
                            VDOAction     *callback,
                            VDOAction     *errorHandler)
{
  if (scrubAtLeastOne && isRingEmpty(&scrubber->highPrioritySlabs)) {
    Slab *slab = getNextSlab(scrubber);
    if (slab != NULL) {
      registerSlabForScrubbing(scrubber, slab, true);
    }
  }
  scrubber->highPriorityOnly = true;
  scrubSlabs(scrubber, parent, callback, errorHandler);
}

/**********************************************************************/
void stopScrubbing(SlabScrubber *scrubber, VDOCompletion *parent)
{
  if (isQuiescent(&scrubber->adminState)) {
    completeCompletion(parent);
  } else {
    startDraining(&scrubber->adminState, ADMIN_STATE_SUSPENDING, parent, NULL);
  }
}

/**********************************************************************/
void resumeScrubbing(SlabScrubber *scrubber, VDOCompletion *parent)
{
  if (!hasSlabsToScrub(scrubber)) {
    completeCompletion(parent);
    return;
  }

  int result = resumeIfQuiescent(&scrubber->adminState);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  scrubNextSlab(scrubber);
  completeCompletion(parent);
}

/**********************************************************************/
int enqueueCleanSlabWaiter(SlabScrubber *scrubber, struct waiter *waiter)
{
  if (isReadOnly(scrubber->readOnlyNotifier)) {
    return VDO_READ_ONLY;
  }

  if (isQuiescent(&scrubber->adminState)) {
    return VDO_NO_SPACE;
  }

  return enqueueWaiter(&scrubber->waiters, waiter);
}

/**********************************************************************/
void dumpSlabScrubber(const SlabScrubber *scrubber)
{
  logInfo("slabScrubber slabCount %u waiters %zu %s%s",
          getScrubberSlabCount(scrubber),
          countWaiters(&scrubber->waiters),
          getAdminStateName(&scrubber->adminState),
          scrubber->highPriorityOnly ? ", highPriorityOnly " : "");
}
