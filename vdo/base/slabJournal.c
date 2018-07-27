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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabJournal.c#4 $
 */

#include "slabJournalInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "blockAllocatorInternals.h"
#include "dataVIO.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "slabSummary.h"

/**********************************************************************/
SlabJournal *asSlabJournal(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(SlabJournal, completion) == 0);
  assertCompletionType(completion->type, SLAB_JOURNAL_COMPLETION);
  return (SlabJournal *) completion;
}

/**
 * Return the slab journal from the resource waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline SlabJournal *slabJournalFromResourceWaiter(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (SlabJournal *)
    ((uintptr_t) waiter - offsetof(SlabJournal, resourceWaiter));
}

/**
 * Return the slab journal from the flush waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline SlabJournal *slabJournalFromFlushWaiter(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (SlabJournal *)
    ((uintptr_t) waiter - offsetof(SlabJournal, flushWaiter));
}

/**********************************************************************/
SlabJournal *slabJournalFromDirtyNode(RingNode *node)
{
  if (node == NULL) {
    return NULL;
  }
  return (SlabJournal *) ((uintptr_t) node - offsetof(SlabJournal, dirtyNode));
}

/**
 * Return the slab journal from the slab summary waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline SlabJournal *slabJournalFromSlabSummaryWaiter(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (SlabJournal *)
    ((uintptr_t) waiter - offsetof(SlabJournal, slabSummaryWaiter));
}

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return the block number corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline PhysicalBlockNumber getBlockNumber(SlabJournal    *journal,
                                                 SequenceNumber  sequence)
{
  TailBlockOffset offset = getSlabJournalBlockOffset(journal, sequence);
  return (journal->slab->journalOrigin + offset);
}

/**
 * Get the lock object for a slab journal block by sequence number.
 *
 * @param journal         Slab journal to retrieve from
 * @param sequenceNumber  Sequence number of the block
 *
 * @return the lock object for the given sequence number
 **/
__attribute__((warn_unused_result))
static inline JournalLock *getLock(SlabJournal    *journal,
                                   SequenceNumber  sequenceNumber)
{
  TailBlockOffset offset = getSlabJournalBlockOffset(journal, sequenceNumber);
  return &journal->locks[offset];
}

/**
 * Check whether the VDO is in read-only mode.
 *
 * @param journal  The journal whose owning VDO should be checked
 *
 * @return <code>true</code> if the VDO is in read-only mode
 **/
__attribute__((warn_unused_result))
static inline bool isVDOReadOnly(SlabJournal *journal)
{
  return isReadOnly(journal->slab->allocator->readOnlyContext);
}

/**
 * Check whether there are entry waiters which should delay a flush.
 *
 * @param journal  The journal to check
 *
 * @return <code>true</code> if there are no entry waiters, or if the slab
 *         is unrecovered
 **/
__attribute__((warn_unused_result))
static inline bool mustMakeEntriesToFlush(SlabJournal *journal)
{
  return (!slabIsRebuilding(journal->slab)
          && hasWaiters(&journal->entryWaiters));
}

/**
 * Check whether a reap is currently in progress.
 *
 * @param journal  The journal which may be reaping
 *
 * @return <code>true</code> if the journal is reaping
 **/
__attribute__((warn_unused_result))
static inline bool isReaping(SlabJournal *journal)
{
  return (journal->head != journal->unreapable);
}

/**
 * Check whether the journal was requested to flush, has flushed, and has no
 * IO outstanding; if so, notify the completion waiting for the flush.
 *
 * @param journal  The journal which may have just flushed
 **/
static inline void checkForFlushComplete(SlabJournal *journal)
{
  if ((journal->flushState != FLUSH_INITIATED)
      || mustMakeEntriesToFlush(journal)
      || isReaping(journal)
      || journal->waitingToCommit
      || !isRingEmpty(&journal->uncommittedBlocks)
      || journal->updatingSlabSummary) {
    return;
  }

  journal->flushState = NOT_FLUSHING;
  finishCompletion(&journal->completion,
                   (isVDOReadOnly(journal) ? VDO_READ_ONLY : VDO_SUCCESS));
  return;
}

/**
 * Initialize tail block as a new block.
 *
 * @param journal  The journal whose tail block is being initialized
 **/
static void initializeTailBlock(SlabJournal *journal)
{
  SlabJournalBlockHeader *header = &journal->tailHeader;
  header->sequenceNumber         = journal->tail;
  header->entryCount             = 0;
  header->hasBlockMapIncrements  = false;
}

/**
 * Set all journal fields appropriately to start journaling.
 *
 * @param journal  The journal to be reset, based on its tail sequence number
 **/
static void initializeJournalState(SlabJournal *journal)
{
  journal->unreapable = journal->head;
  journal->reapLock   = getLock(journal, journal->unreapable);
  journal->nextCommit = journal->tail;
  journal->summarized = journal->lastSummarized = journal->tail;
  initializeTailBlock(journal);
}

/**
 * Check whether a journal block is full.
 *
 * @param journal The slab journal for the block
 *
 * @return <code>true</code> if the tail block is full
 **/
__attribute__((warn_unused_result))
static bool blockIsFull(SlabJournal *journal)
{
  JournalEntryCount count = journal->tailHeader.entryCount;
  return (journal->tailHeader.hasBlockMapIncrements
          ? (journal->fullEntriesPerBlock == count)
          : (journal->entriesPerBlock == count));
}

/**********************************************************************/
static void addEntries(SlabJournal *journal);
static void updateTailBlockLocation(SlabJournal *journal);
static void releaseJournalLocks(Waiter *waiter, void *context);

/**********************************************************************/
int makeSlabJournal(BlockAllocator  *allocator,
                    Slab            *slab,
                    PhysicalLayer   *layer,
                    RecoveryJournal *recoveryJournal,
                    SlabJournal    **journalPtr)
{
  SlabJournal *journal;
  const SlabConfig *slabConfig = getSlabConfig(allocator->depot);
  int result = ALLOCATE_EXTENDED(SlabJournal, slabConfig->slabJournalBlocks,
                                 JournalLock, __func__, &journal);
  if (result != VDO_SUCCESS) {
    return result;
  }

  journal->slab                = slab;
  journal->size                = slabConfig->slabJournalBlocks;
  journal->flushingThreshold   = slabConfig->slabJournalFlushingThreshold;
  journal->blockingThreshold   = slabConfig->slabJournalBlockingThreshold;
  journal->scrubbingThreshold  = slabConfig->slabJournalScrubbingThreshold;
  journal->entriesPerBlock     = SLAB_JOURNAL_ENTRIES_PER_BLOCK;
  journal->fullEntriesPerBlock = SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK;
  journal->events              = &allocator->slabJournalStatistics;
  journal->recoveryJournal     = recoveryJournal;
  journal->summary             = getSlabSummaryZone(allocator);
  journal->tail                = 1;
  journal->head                = 1;

  journal->flushingDeadline = journal->flushingThreshold;
  // Set there to be some time between the deadline and the blocking threshold,
  // so that hopefully all are done before blocking.
  if ((journal->blockingThreshold - journal->flushingThreshold) > 5) {
    journal->flushingDeadline = journal->blockingThreshold - 5;
  }

  journal->slabSummaryWaiter.callback = releaseJournalLocks;

  result = ALLOCATE(VDO_BLOCK_SIZE, char, "PackedSlabJournalBlock",
                    (char **) &journal->block);
  if (result != VDO_SUCCESS) {
    freeSlabJournal(&journal);
    return result;
  }

  result = initializeEnqueueableCompletion(&journal->completion,
                                           SLAB_JOURNAL_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    freeSlabJournal(&journal);
    return result;
  }

  initializeRing(&journal->dirtyNode);
  initializeRing(&journal->uncommittedBlocks);

  journal->tailHeader.nonce        = slab->allocator->nonce;
  journal->tailHeader.metadataType = VDO_METADATA_SLAB_JOURNAL;
  initializeJournalState(journal);

  *journalPtr = journal;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeSlabJournal(SlabJournal **journalPtr)
{
  SlabJournal *journal = *journalPtr;
  if (journal == NULL) {
    return;
  }

  destroyEnqueueable(&journal->completion);
  FREE(journal->block);
  FREE(journal);
  *journalPtr = NULL;
}

/**********************************************************************/
bool isSlabJournalBlank(const SlabJournal *journal)
{
  return ((journal != NULL)
          && (journal->tail == 1)
          && (journal->tailHeader.entryCount == 0));
}

/**********************************************************************/
bool isSlabJournalDirty(const SlabJournal *journal)
{
  return (journal->recoveryLock != 0);
}

/**
 * Put a slab journal on the dirty ring of its allocator in the correct order.
 *
 * @param journal  The journal to be marked dirty
 * @param lock     The recovery journal lock held by the slab journal
 **/
static void markSlabJournalDirty(SlabJournal *journal, SequenceNumber lock)
{
  ASSERT_LOG_ONLY(!isSlabJournalDirty(journal), "slab journal was clean");

  journal->recoveryLock = lock;
  RingNode *dirtyRing   = &journal->slab->allocator->dirtySlabJournals;
  RingNode *node        = dirtyRing->prev;
  while (node != dirtyRing) {
    SlabJournal *dirtyJournal = slabJournalFromDirtyNode(node);
    if (dirtyJournal->recoveryLock <= journal->recoveryLock) {
      break;
    }

    node = node->prev;
  }

  pushRingNode(node->next, &journal->dirtyNode);
}

/**********************************************************************/
static void markSlabJournalClean(SlabJournal *journal)
{
  journal->recoveryLock = 0;
  unspliceRingNode(&journal->dirtyNode);
}

/**
 * Implements WaiterCallback. This callback is invoked on all VIOs waiting
 * to make slab journal entries after the VDO has gone into read-only mode.
 **/
static void abortWaiter(Waiter *waiter,
                        void   *context __attribute__((unused)))
{
  continueDataVIO(waiterAsDataVIO(waiter), VDO_READ_ONLY);
}

/**********************************************************************/
void abortSlabJournalWaiters(SlabJournal *journal)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == journal->slab->allocator->threadID),
                  "abortSlabJournalWaiters() called on correct thread");
  notifyAllWaiters(&journal->entryWaiters, abortWaiter, journal);
  checkForFlushComplete(journal);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All VIOs waiting for to make entries
 * will be awakened with an error. All flushes will complete as soon as all
 * pending IO is done.
 *
 * @param journal    The journal which has failed
 * @param errorCode  The error result triggering this call
 **/
static void enterJournalReadOnlyMode(SlabJournal *journal, int errorCode)
{
  enterReadOnlyMode(journal->slab->allocator->readOnlyContext, errorCode);
  abortSlabJournalWaiters(journal);
}

/**
 * Actually advance the head of the journal now that any necessary flushes
 * are complete.
 *
 * @param journal  The journal to be reaped
 **/
static void finishReaping(SlabJournal *journal)
{
  journal->head = journal->unreapable;
  addEntries(journal);
  checkForFlushComplete(journal);
}

/**********************************************************************/
static void reapSlabJournal(SlabJournal *journal);

/**
 * Finish reaping now that we have flushed the lower layer and then try
 * reaping again in case we deferred reaping due to an outstanding VIO.
 *
 * @param completion  The flush VIO
 **/
static void completeReaping(VDOCompletion *completion)
{
  VIOPoolEntry *entry   = completion->parent;
  SlabJournal  *journal = entry->parent;
  returnVIO(journal->slab->allocator, entry);
  finishReaping(journal);
  reapSlabJournal(journal);
}

/**
 * Handle an error flushing the lower layer.
 *
 * @param completion  The flush VIO
 **/
static void handleFlushError(VDOCompletion *completion)
{
  SlabJournal *journal = ((VIOPoolEntry *) completion->parent)->parent;
  enterJournalReadOnlyMode(journal, completion->result);
  completeReaping(completion);
}

/**
 * Waiter callback for getting a VIO with which to flush the lower layer prior
 * to reaping.
 *
 * @param waiter      The journal as a flush waiter
 * @param vioContext  The newly acquired flush VIO
 **/
static void flushForReaping(Waiter *waiter, void *vioContext)
{
  SlabJournal  *journal = slabJournalFromFlushWaiter(waiter);
  VIOPoolEntry *entry   = vioContext;
  VIO          *vio     = entry->vio;

  entry->parent                    = journal;
  vio->completion.callbackThreadID = journal->slab->allocator->threadID;
  launchFlush(vio, completeReaping, handleFlushError);
}

/**
 * Conduct a reap on a slab journal to reclaim unreferenced blocks.
 *
 * @param journal  The slab journal
 **/
static void reapSlabJournal(SlabJournal *journal)
{
  if (isUnrecoveredSlab(journal->slab) || journal->closeRequested
      || isVDOReadOnly(journal)) {
    // We must not reap while we are unrecovered, or when closing or closed,
    // and might as well not if the VDO is in read-only mode.
    return;
  }

  if (isReaping(journal)) {
    // We already have a reap in progress so wait for it to finish.
    return;
  }

  /*
   * Start reclaiming blocks only when the journal head has no references. Then
   * stop when a block is referenced or reap reaches the most recently written
   * block, referenced by the slab summary, which has the sequence number just
   * before the tail.
   */
  bool reaped = false;
  while ((journal->unreapable < journal->tail)
         && (journal->reapLock->count == 0)) {
    reaped = true;
    journal->unreapable++;
    journal->reapLock++;
    if (journal->reapLock == &journal->locks[journal->size]) {
      journal->reapLock = &journal->locks[0];
    }
  }

  if (!reaped) {
    return;
  }

  PhysicalLayer *layer = journal->completion.layer;
  if (!layer->isFlushRequired(layer)) {
    finishReaping(journal);
    return;
  }

  /*
   * In async mode, it is never safe to reap a slab journal block without first
   * issuing a flush, regardless of whether a user flush has been received or
   * not. In the absence of the flush, the reference block write which released
   * the locks allowing the slab journal to reap may not be persisted. Although
   * slab summary writes will eventually issue flushes, multiple slab journal
   * block writes can be issued while previous slab summary updates have not
   * yet been made. Even though those slab journal block writes will be ignored
   * if the slab summary update is not persisted, they may still overwrite the
   * to-be-reaped slab journal block resulting in a loss of reference count
   * updates (VDO-2912).
   */
  journal->flushWaiter.callback = flushForReaping;
  int result = acquireVIO(journal->slab->allocator, &journal->flushWaiter);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }
}

/**
 * This is the callback invoked after a slab summary update completes. It
 * is registered in the constructor on behalf of updateTailBlockLocation().
 *
 * Implements WaiterCallback.
 *
 * @param waiter        The slab summary waiter that has just been notified
 * @param context       The result code of the update
 **/
static void releaseJournalLocks(Waiter *waiter, void *context)
{
  SlabJournal *journal = slabJournalFromSlabSummaryWaiter(waiter);
  int          result  = *((int *) context);
  if (result != VDO_SUCCESS) {
    journal->updatingSlabSummary = false;
    logErrorWithStringError(result, "failed slab summary updater %" PRIu64,
                            journal->summarized);
    enterJournalReadOnlyMode(journal, result);
    return;
  }

  if (journal->partialWriteInProgress
      && (journal->summarized == journal->tail)) {
    journal->partialWriteInProgress = false;
    addEntries(journal);
  }

  SequenceNumber first    = journal->lastSummarized;
  journal->lastSummarized = journal->summarized;
  for (SequenceNumber i = journal->summarized - 1; i >= first; i--) {
    // Release the lock the summarized block held on the recovery journal.
    // (During replay, recoveryStart will always be 0.)
    if (journal->recoveryJournal != NULL) {
      ZoneCount zoneNumber = journal->slab->allocator->zoneNumber;
      releaseRecoveryJournalBlockReference(journal->recoveryJournal,
                                           getLock(journal, i)->recoveryStart,
                                           ZONE_TYPE_PHYSICAL,
                                           zoneNumber);

    }

    // Release our own lock against reaping for blocks that are committed.
    // (This function will not change locks during replay.)
    adjustSlabJournalBlockReference(journal, i, -1);
  }

  journal->updatingSlabSummary = false;

  reapSlabJournal(journal);

  // Check if the slab summary needs to be updated again.
  updateTailBlockLocation(journal);
}

/**
 * Update the tail block location in the slab summary, if necessary.
 *
 * @param journal  The slab journal that is updating its tail block location
 **/
static void updateTailBlockLocation(SlabJournal *journal)
{
  if (journal->updatingSlabSummary || isVDOReadOnly(journal)
      || (journal->lastSummarized >= journal->nextCommit)) {
    checkForFlushComplete(journal);
    return;
  }

  BlockCount freeBlockCount;
  if (isUnrecoveredSlab(journal->slab)) {
    freeBlockCount = getSummarizedFreeBlockCount(journal->summary,
                                                 journal->slab->slabNumber);
  } else {
    freeBlockCount = getSlabFreeBlockCount(journal->slab);
  }

  journal->summarized          = journal->nextCommit;
  journal->updatingSlabSummary = true;

  /*
   * Update slab summary as dirty.
   * Slab journal can only reap past sequence number 1 when all the refCounts
   * for this slab have been written to the layer. Therefore, indicate that the
   * refCounts must be loaded when the journal head has reaped past sequence
   * number 1.
   */
  TailBlockOffset blockOffset
    = getSlabJournalBlockOffset(journal, journal->summarized);
  updateSlabSummaryEntry(journal->summary, &journal->slabSummaryWaiter,
                         journal->slab->slabNumber, blockOffset,
                         (journal->head > 1), false, freeBlockCount);
}

/**********************************************************************/
void reopenSlabJournal(SlabJournal *journal)
{
  ASSERT_LOG_ONLY(journal->tailHeader.entryCount == 0,
                  "Slab journal's active block empty before reopening");
  journal->head       = journal->tail;
  initializeJournalState(journal);

  // Ensure no locks are spuriously held on an empty journal.
  for (SequenceNumber block = 1; block <= journal->size; block++) {
    ASSERT_LOG_ONLY((getLock(journal, block)->count == 0),
                    "Scrubbed journal's block %" PRIu64 " is not locked",
                    block);
  }

  addEntries(journal);
}

/**********************************************************************/
static SequenceNumber getCommittingSequenceNumber(const VIOPoolEntry *entry)
{
  const PackedSlabJournalBlock *block = entry->buffer;
  return getUInt64LE(block->header.fields.sequenceNumber);
}

/**
 * Handle post-commit processing. This is the callback registered by
 * writeSlabJournalBlock().
 *
 * @param completion  The write VIO as a completion
 **/
static void completeWrite(VDOCompletion *completion)
{
  int           writeResult = completion->result;
  VIOPoolEntry *entry       = completion->parent;
  SlabJournal  *journal     = entry->parent;

  SequenceNumber committed = getCommittingSequenceNumber(entry);
  unspliceRingNode(&entry->node);
  returnVIO(journal->slab->allocator, entry);

  if (writeResult != VDO_SUCCESS) {
    logErrorWithStringError(writeResult,
                            "cannot write slab journal block %" PRIu64,
                            committed);
    enterJournalReadOnlyMode(journal, writeResult);
    return;
  }

  relaxedAdd64(&journal->events->blocksWritten, 1);

  if (isRingEmpty(&journal->uncommittedBlocks)) {
    // If no blocks are outstanding, then the commit point is at the tail.
    journal->nextCommit = journal->tail;
  } else {
    // The commit point is always the beginning of the oldest incomplete block.
    VIOPoolEntry *oldest = asVIOPoolEntry(journal->uncommittedBlocks.next);
    journal->nextCommit = getCommittingSequenceNumber(oldest);
  }

  updateTailBlockLocation(journal);
}

/**
 * Callback from acquireVIO() registered in commitSlabJournalTail().
 *
 * @param waiter      The VIO pool waiter which was just notified
 * @param vioContext  The VIO pool entry for the write
 **/
static void writeSlabJournalBlock(Waiter *waiter, void *vioContext)
{
  SlabJournal            *journal = slabJournalFromResourceWaiter(waiter);
  VIOPoolEntry           *entry   = vioContext;
  SlabJournalBlockHeader *header  = &journal->tailHeader;

  header->head = journal->head;
  pushRingNode(&journal->uncommittedBlocks, &entry->node);
  packSlabJournalBlockHeader(header, &journal->block->header);

  // Copy the tail block into the VIO.
  memcpy(entry->buffer, journal->block, VDO_BLOCK_SIZE);

  int unusedEntries = journal->entriesPerBlock - header->entryCount;
  ASSERT_LOG_ONLY(unusedEntries >= 0, "Slab journal block is not overfull");
  if (unusedEntries > 0) {
    // Release the per-entry locks for any unused entries in the block we are
    // about to write.
    adjustSlabJournalBlockReference(journal, header->sequenceNumber,
                                    -unusedEntries);
    journal->partialWriteInProgress = !blockIsFull(journal);
  }

  PhysicalBlockNumber blockNumber
    = getBlockNumber(journal, header->sequenceNumber);

  entry->parent = journal;
  entry->vio->completion.callbackThreadID = journal->slab->allocator->threadID;
  /*
   * This block won't be read in recovery until the slab summary is updated
   * to refer to it. The slab summary update does a flush which is sufficient
   * to protect us from VDO-2331.
   */
  launchWriteMetadataVIO(entry->vio, blockNumber, completeWrite,
                         completeWrite);

  // Since the write is submitted, the tail block structure can be reused.
  journal->tail++;
  initializeTailBlock(journal);
  journal->waitingToCommit = false;
  if (journal->waitingForSpace) {
    journal->waitingForSpace = false;
    completeCompletion(&journal->completion);
  }

  addEntries(journal);
}

/**********************************************************************/
static void initiateFlush(SlabJournal *journal)
{
  if (journal->flushState == FLUSH_REQUESTED) {
    journal->flushState = FLUSH_INITIATED;
  }
}

/**********************************************************************/
void commitSlabJournalTail(SlabJournal *journal)
{
  if ((journal->tailHeader.entryCount == 0)
      && mustMakeEntriesToFlush(journal)) {
    // There are no entries at the moment, but there are some waiters, so defer
    // initiating the flush until those entries are ready to write.
    return;
  }

  initiateFlush(journal);

  if (isVDOReadOnly(journal)
      || journal->waitingToCommit
      || (journal->tailHeader.entryCount == 0)) {
    // There is nothing to do since the tail block is empty, or writing, or
    // the journal is in read-only mode.
    return;
  }

  /*
   * Since we are about to commit the tail block, this journal no longer
   * needs to be on the ring of journals which the recovery journal might
   * ask to commit.
   */
  markSlabJournalClean(journal);

  journal->waitingToCommit = true;

  journal->resourceWaiter.callback = writeSlabJournalBlock;
  int result = acquireVIO(journal->slab->allocator, &journal->resourceWaiter);
  if (result != VDO_SUCCESS) {
    journal->waitingToCommit = false;
    enterJournalReadOnlyMode(journal, result);
    return;
  }
}

/**********************************************************************/
void encodeSlabJournalEntry(SlabJournalBlockHeader *tailHeader,
                            SlabJournalPayload     *payload,
                            SlabBlockNumber         sbn,
                            JournalOperation        operation)
{
  JournalEntryCount entryNumber = tailHeader->entryCount++;
  if (operation == BLOCK_MAP_INCREMENT) {
    if (!tailHeader->hasBlockMapIncrements) {
      memset(payload->fullEntries.entryTypes, 0,
             SLAB_JOURNAL_ENTRY_TYPES_SIZE);
      tailHeader->hasBlockMapIncrements = true;
    }

    payload->fullEntries.entryTypes[entryNumber / 8]
      |= ((byte) 1 << (entryNumber % 8));
  }

  packSlabJournalEntry(&payload->entries[entryNumber], sbn,
                       isIncrementOperation(operation));
}

/**********************************************************************/
SlabJournalEntry decodeSlabJournalEntry(PackedSlabJournalBlock *block,
                                        JournalEntryCount       entryCount)
{
  SlabJournalEntry entry
    = unpackSlabJournalEntry(&block->payload.entries[entryCount]);
  if (block->header.fields.hasBlockMapIncrements
      && ((block->payload.fullEntries.entryTypes[entryCount / 8]
           & ((byte) 1 << (entryCount % 8))) != 0)) {
    entry.operation = BLOCK_MAP_INCREMENT;
  }
  return entry;
}

/**
 * Actually add an entry to the slab journal, potentially firing off a write
 * if a block becomes full. This function is synchronous.
 *
 * @param journal        The slab journal to append to
 * @param pbn            The pbn being adjusted
 * @param operation      The type of entry to make
 * @param recoveryPoint  The recovery journal point for this entry
 **/
static void addEntry(SlabJournal         *journal,
                     PhysicalBlockNumber  pbn,
                     JournalOperation     operation,
                     const JournalPoint  *recoveryPoint)
{
  int result = ASSERT(beforeJournalPoint(&journal->tailHeader.recoveryPoint,
                                         recoveryPoint),
                      "recovery journal point is monotonically increasing, "
                      "recovery point: %" PRIu64 ".%u, "
                      "block recovery point: %" PRIu64 ".%u",
                      recoveryPoint->sequenceNumber, recoveryPoint->entryCount,
                      journal->tailHeader.recoveryPoint.sequenceNumber,
                      journal->tailHeader.recoveryPoint.entryCount);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }

  PackedSlabJournalBlock *block = journal->block;
  if (operation == BLOCK_MAP_INCREMENT) {
    result = ASSERT_LOG_ONLY((journal->tailHeader.entryCount
                              < journal->fullEntriesPerBlock),
                             "block has room for full entries");
    if (result != VDO_SUCCESS) {
      enterJournalReadOnlyMode(journal, result);
      return;
    }
  }

  encodeSlabJournalEntry(&journal->tailHeader, &block->payload,
                         pbn - journal->slab->start, operation);
  journal->tailHeader.recoveryPoint = *recoveryPoint;
  if (blockIsFull(journal)) {
    commitSlabJournalTail(journal);
  }
}

/**********************************************************************/
bool mayAddSlabJournalEntry(SlabJournal      *journal,
                            JournalOperation  operation,
                            VDOCompletion    *parent)
{
  if (!journal->waitingToCommit) {
    SlabJournalBlockHeader *header = &journal->tailHeader;
    if (header->entryCount < journal->fullEntriesPerBlock) {
      return true;
    }

    if (!header->hasBlockMapIncrements && (operation != BLOCK_MAP_INCREMENT)) {
      return true;
    }

    // The tail block does not have room for a block map increment, so commit
    // it now.
    commitSlabJournalTail(journal);
    if (!journal->waitingToCommit) {
      return true;
    }
  }

  journal->waitingForSpace = true;
  prepareCompletion(&journal->completion, finishParentCallback,
                    finishParentCallback, parent->callbackThreadID, parent);
  return false;
}

/**********************************************************************/
void addSlabJournalEntryForRebuild(SlabJournal         *journal,
                                   PhysicalBlockNumber  pbn,
                                   JournalOperation     operation,
                                   JournalPoint        *recoveryPoint)
{
  // Only accept entries after the current recovery point.
  if (!beforeJournalPoint(&journal->tailHeader.recoveryPoint, recoveryPoint)) {
    return;
  }

  if ((journal->tail - journal->head) >= journal->size) {
    /*
     * We must have reaped the current head before the crash, since
     * the blocked threshold keeps us from having more entries than
     * fit in a slab journal; hence we can just advance the head
     * (and unreapable block), as needed.
     */
    journal->head++;
    journal->unreapable++;
  }

  markSlabReplaying(journal->slab);
  addEntry(journal, pbn, operation, recoveryPoint);
}

/**
 * Check whether the journal should be saving reference blocks out.
 *
 * @param journal       The journal to check
 *
 * @return true if the journal should be requesting reference block writes
 **/
static bool requiresFlushing(const SlabJournal *journal)
{
  BlockCount journalLength = (journal->tail - journal->head);
  return (journalLength >= journal->flushingThreshold);
}

/**
 * Check whether the journal must be reaped before adding new entries.
 *
 * @param journal       The journal to check
 *
 * @return true if the journal must be reaped
 **/
static bool requiresReaping(const SlabJournal *journal)
{
  BlockCount journalLength = (journal->tail - journal->head);
  return (journalLength >= journal->blockingThreshold);
}

/**********************************************************************/
bool requiresScrubbing(const SlabJournal *journal)
{
  BlockCount journalLength = (journal->tail - journal->head);
  return (journalLength >= journal->scrubbingThreshold);
}

/**
 * Implements WaiterCallback. This callback is invoked by addEntries() once
 * it has determined that we are ready to make another entry in the slab
 * journal.
 *
 * @param waiter        The VIO which should make an entry now
 * @param context       The slab journal to make an entry in
 **/
static void addEntryFromWaiter(Waiter *waiter, void *context)
{
  DataVIO     *dataVIO = waiterAsDataVIO(waiter);
  SlabJournal *journal = (SlabJournal *) context;
  SlabJournalBlockHeader *header = &journal->tailHeader;
  SequenceNumber recoveryBlock = dataVIO->recoveryJournalPoint.sequenceNumber;

  if (header->entryCount == 0) {
    /*
     * This is the first entry in the current tail block, so get a lock
     * on the recovery journal which we will hold until this tail block is
     * committed.
     */
    getLock(journal, header->sequenceNumber)->recoveryStart = recoveryBlock;
    if (journal->recoveryJournal != NULL) {
      ZoneCount zoneNumber = journal->slab->allocator->zoneNumber;
      acquireRecoveryJournalBlockReference(journal->recoveryJournal,
                                           recoveryBlock, ZONE_TYPE_PHYSICAL,
                                           zoneNumber);
    }
    markSlabJournalDirty(journal, recoveryBlock);

    // If the slab journal is over the first threshold, tell the refCounts to
    // write some reference blocks, but proceed apace.
    if (requiresFlushing(journal)) {
      relaxedAdd64(&journal->events->flushCount, 1);
      BlockCount journalLength = (journal->tail - journal->head);
      BlockCount blocksToDeadline = 0;
      if (journalLength <= journal->flushingDeadline) {
        blocksToDeadline = journal->flushingDeadline - journalLength;
      }
      saveSeveralReferenceBlocks(journal->slab->referenceCounts,
                                 blocksToDeadline + 1);
    }
  }

  JournalPoint slabJournalPoint = {
    .sequenceNumber = header->sequenceNumber,
    .entryCount     = header->entryCount,
  };

  addEntry(journal, dataVIO->operation.pbn, dataVIO->operation.type,
           &dataVIO->recoveryJournalPoint);

  // Now that an entry has been made in the slab journal, update the
  // reference counts.
  int result = modifySlabReferenceCount(journal->slab, &slabJournalPoint,
                                        dataVIO->operation);
  continueDataVIO(dataVIO, result);
}

/**
 * Check whether the next entry to be made is a block map increment.
 *
 * @param journal  The journal
 *
 * @return <code>true</code> if the first entry waiter's operation is a block
 *         map increment
 **/
static inline bool isNextEntryABlockMapIncrement(SlabJournal *journal)
{
  DataVIO *dataVIO = waiterAsDataVIO(getFirstWaiter(&journal->entryWaiters));
  return (dataVIO->operation.type == BLOCK_MAP_INCREMENT);
}

/**
 * Add as many entries as possible from the queue of VIOs waiting to make
 * entries. By processing the queue in order, we ensure that slab journal
 * entries are made in the same order as recovery journal entries for the
 * same increment or decrement.
 *
 * @param journal  The journal to which entries may be added
 **/
static void addEntries(SlabJournal *journal)
{
  if (journal->addingEntries) {
    // Protect against re-entrancy.
    return;
  }

  journal->addingEntries = true;
  while (hasWaiters(&journal->entryWaiters)) {
    if (journal->partialWriteInProgress || slabIsRebuilding(journal->slab)) {
      // Don't add entries while rebuilding or while a partial write is
      // outstanding (VDO-2399).
      break;
    }

    SlabJournalBlockHeader *header = &journal->tailHeader;
    if (journal->waitingToCommit) {
      // If we are waiting for resources to write the tail block, and the
      // tail block is full, we can't make another entry.
      relaxedAdd64(&journal->events->tailBusyCount, 1);
      break;
    } else if (isNextEntryABlockMapIncrement(journal)
               && (header->entryCount >= journal->fullEntriesPerBlock)) {
      // The tail block does not have room for a block map increment, so
      // commit it now.
      commitSlabJournalTail(journal);
      if (journal->waitingToCommit) {
        relaxedAdd64(&journal->events->tailBusyCount, 1);
        break;
      }
    }

    // If the slab is over the blocking threshold, make the VIO wait.
    if (requiresReaping(journal)) {
      relaxedAdd64(&journal->events->blockedCount, 1);
      saveDirtyReferenceBlocks(journal->slab->referenceCounts);
      break;
    }

    if (header->entryCount == 0) {
      JournalLock *lock = getLock(journal, header->sequenceNumber);
      // Check if the on disk slab journal is full. Because of the
      // blocking and scrubbing thresholds, this should never happen.
      if (lock->count > 0) {
        ASSERT_LOG_ONLY((journal->head + journal->size) == journal->tail,
                        "New block has locks, but journal is not full");

        /*
         * The blocking threshold must let the journal fill up if the new
         * block has locks; if the blocking threshold is smaller than the
         * journal size, the new block cannot possibly have locks already.
         */
        ASSERT_LOG_ONLY((journal->blockingThreshold >= journal->size),
                        "New block can have locks already iff blocking"
                        "threshold is at the end of the journal");

        relaxedAdd64(&journal->events->diskFullCount, 1);
        saveDirtyReferenceBlocks(journal->slab->referenceCounts);
        break;
      }

      /*
       * Don't allow the new block to be reaped until all of the reference
       * count blocks are written and the journal block has been
       * fully committed as well.
       */
      lock->count = journal->entriesPerBlock + 1;

      if (header->sequenceNumber == 1) {
        /*
         * This is the first entry in this slab journal, ever. Dirty all of
         * the reference count blocks. Each will acquire a lock on the
         * tail block so that the journal won't be reaped until the
         * reference counts are initialized. The lock acquisition must
         * be done by the RefCounts since here we don't know how many
         * reference blocks the RefCounts has.
         */
        Slab *slab = journal->slab;
        int result = acquireDirtyBlockLocks(slab->referenceCounts);
        if (result != VDO_SUCCESS) {
          enterJournalReadOnlyMode(journal, result);
          break;
        }
      }
    }

    notifyNextWaiter(&journal->entryWaiters, addEntryFromWaiter, journal);
  }

  journal->addingEntries = false;

  if ((journal->flushState == FLUSH_REQUESTED)
      && !hasWaiters(&journal->entryWaiters)) {
    commitSlabJournalTail(journal);
  }
}

/**********************************************************************/
void addSlabJournalEntry(SlabJournal *journal, DataVIO *dataVIO)
{
  if (journal->closeRequested) {
    continueDataVIO(dataVIO, VDO_SHUTTING_DOWN);
    return;
  }

  if (isVDOReadOnly(journal)) {
    continueDataVIO(dataVIO, VDO_READ_ONLY);
    return;
  }

  int result = enqueueDataVIO(&journal->entryWaiters, dataVIO,
                              THIS_LOCATION("$F($j-$js)"));
  if (result != VDO_SUCCESS) {
    continueDataVIO(dataVIO, result);
    return;
  }

  if (isUnrecoveredSlab(journal->slab) && requiresReaping(journal)) {
    increaseScrubbingPriority(journal->slab);
  }

  addEntries(journal);
}

/**********************************************************************/
void adjustSlabJournalBlockReference(SlabJournal    *journal,
                                     SequenceNumber  sequenceNumber,
                                     int             adjustment)
{
  if (sequenceNumber == 0) {
    return;
  }

  if (isReplayingSlab(journal->slab)) {
    // Locks should not be used during offline replay.
    return;
  }

  ASSERT_LOG_ONLY((adjustment != 0), "adjustment must be non-zero");
  JournalLock *lock = getLock(journal, sequenceNumber);
  if (adjustment < 0) {
    ASSERT_LOG_ONLY((-adjustment <= lock->count),
                    "decrement of lock count for slab journal block %" PRIu64
                    " must not underflow", sequenceNumber);
  }

  lock->count += adjustment;
  if (lock->count == 0) {
    reapSlabJournal(journal);
  }
}

/**********************************************************************/
bool releaseRecoveryJournalLock(SlabJournal    *journal,
                                SequenceNumber  recoveryLock)
{
  if (recoveryLock > journal->recoveryLock) {
    ASSERT_LOG_ONLY((recoveryLock < journal->recoveryLock),
                    "slab journal recovery lock is not older than the recovery"
                    " journal head");
    return false;
  }

  if ((recoveryLock < journal->recoveryLock) || isVDOReadOnly(journal)) {
    return false;
  }

  // All locks are held by the block which is in progress; write it.
  commitSlabJournalTail(journal);
  return true;
}

/**********************************************************************/
void closeSlabJournal(SlabJournal   *journal,
                      VDOCompletion *parent,
                      VDOAction     *callback,
                      VDOAction     *errorHandler,
                      ThreadID       threadID)
{
  ASSERT_LOG_ONLY((!(slabIsRebuilding(journal->slab)
                     && hasWaiters(&journal->entryWaiters))),
                  "slab is recovered or has no waiters");
  journal->closeRequested = true;
  flushSlabJournal(journal, parent, callback, errorHandler, threadID);
}

/**********************************************************************/
void flushSlabJournal(SlabJournal   *journal,
                      VDOCompletion *parent,
                      VDOAction     *callback,
                      VDOAction     *errorHandler,
                      ThreadID       threadID)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == journal->slab->allocator->threadID),
                  "flushSlabJournal() called on correct thread");
  int result = ASSERT((journal->flushState == NOT_FLUSHING),
                      "slab journal not flushing");
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  journal->flushState = FLUSH_REQUESTED;
  prepareCompletion(&journal->completion, callback, errorHandler, threadID,
                    parent);
  commitSlabJournalTail(journal);
  checkForFlushComplete(journal);
}

/**
 * Finish doing manual IO on the slab journal by returning the VIO and
 * finishing the slab journal completion.
 *
 * @param completion  The VIO as a completion
 **/
static void finishManualIO(VDOCompletion *completion)
{
  int           result  = completion->result;
  VIOPoolEntry *entry   = completion->parent;
  SlabJournal  *journal = entry->parent;
  returnVIO(journal->slab->allocator, entry);
  finishCompletion(&journal->completion, result);
}

/**
 * Set up the in-memory journal state to the state which was written to disk.
 * This is the callback registered in readSlabJournalTail().
 *
 * @param completion  The VIO which was used to read the journal tail
 **/
static void setDecodedState(VDOCompletion *completion)
{
  VIOPoolEntry           *entry   = completion->parent;
  SlabJournal            *journal = entry->parent;
  PackedSlabJournalBlock *block   = entry->buffer;

  SlabJournalBlockHeader header;
  unpackSlabJournalBlockHeader(&block->header, &header);

  if ((header.metadataType != VDO_METADATA_SLAB_JOURNAL)
      || (header.nonce != journal->slab->allocator->nonce)) {
    finishManualIO(completion);
    return;
  }

  journal->tail = header.sequenceNumber + 1;

  // If the slab is clean, this implies the slab journal is empty, so advance
  // the head appropriately.
  if (getSummarizedCleanliness(journal->summary, journal->slab->slabNumber)) {
    journal->head = journal->tail;
  } else {
    journal->head = header.head;
  }

  journal->tailHeader = header;
  initializeJournalState(journal);
  finishManualIO(completion);
}

/**
 * This reads the slab journal tail block by using a VIO acquired from the VIO
 * pool. This is the success callback from acquireVIOFromPool() when decoding
 * the slab journal.
 *
 * @param waiter      The VIO pool waiter which has just been notified
 * @param vioContext  The VIO pool entry given to the waiter
 **/
static void readSlabJournalTail(Waiter *waiter, void *vioContext)
{
  SlabJournal  *journal = slabJournalFromResourceWaiter(waiter);
  VIOPoolEntry *entry   = vioContext;
  TailBlockOffset lastCommitPoint
    = getSummarizedTailBlockOffset(journal->summary,
                                   journal->slab->slabNumber);
  entry->parent = journal;

  if ((lastCommitPoint == 0)
      && !mustLoadRefCounts(journal->summary, journal->slab->slabNumber)) {
    /*
     * This slab claims that it has a tail block at (journal->size - 1), but
     * a head of 1. This is impossible, due to the scrubbing threshold, on
     * a real system, so don't bother reading the (bogus) data off disk.
     */
    ASSERT_LOG_ONLY(((journal->size < 16)
                     || (journal->scrubbingThreshold < (journal->size - 1))),
                    "Scrubbing threshold protects against reads of unwritten"
                    "slab journal blocks");
    VDOCompletion *completion = vioAsCompletion(entry->vio);
    resetCompletion(completion);
    finishManualIO(completion);
    return;
  }

  // Slab summary keeps the commit point offset, so the tail block is the
  // block before that. Calculation supports small journals in unit tests.
  entry->vio->completion.callbackThreadID
    = journal->completion.callbackThreadID;
  TailBlockOffset tailBlock = ((lastCommitPoint == 0)
                               ? (TailBlockOffset) (journal->size - 1)
                               : (lastCommitPoint - 1));
  launchReadMetadataVIO(entry->vio, journal->slab->journalOrigin + tailBlock,
                        setDecodedState, finishManualIO);
}

/**********************************************************************/
void decodeSlabJournal(SlabJournal   *journal,
                       VDOCompletion *parent,
                       VDOAction     *callback,
                       VDOAction     *errorHandler,
                       ThreadID       threadID)
{
  prepareCompletion(&journal->completion, callback, errorHandler, threadID,
                    parent);
  journal->resourceWaiter.callback = readSlabJournalTail;
  int result = acquireVIO(journal->slab->allocator, &journal->resourceWaiter);
  if (result != VDO_SUCCESS) {
    finishCompletion(&journal->completion, result);
  }
}

/**********************************************************************/
void dumpSlabJournal(const SlabJournal *journal)
{
  logInfo("  slab journal: entryWaiters=%zu waitingToCommit=%s"
          " updatingSlabSummary=%s head=%" PRIu64 " unreapable=%" PRIu64
          " tail=%" PRIu64 " nextCommit=%" PRIu64 " summarized=%" PRIu64
          " lastSummarized=%" PRIu64 " recoveryJournalLock=%" PRIu64
          " dirty=%s", countWaiters(&journal->entryWaiters),
          boolToString(journal->waitingToCommit),
          boolToString(journal->updatingSlabSummary),
          journal->head, journal->unreapable, journal->tail,
          journal->nextCommit, journal->summarized, journal->lastSummarized,
          journal->recoveryLock,
          boolToString(isSlabJournalDirty(journal)));
  // Given the frequency with which the locks are just a tiny bit off, it
  // might be worth dumping all the locks, but that might be too much logging.
}
