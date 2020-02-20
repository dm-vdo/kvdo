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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabJournal.c#33 $
 */

#include "slabJournalInternals.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "adminState.h"
#include "blockAllocatorInternals.h"
#include "dataVIO.h"
#include "recoveryJournal.h"
#include "refCounts.h"
#include "slabDepot.h"
#include "slabSummary.h"

/**
 * Return the slab journal from the resource waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline struct slab_journal *
slabJournalFromResourceWaiter(struct waiter *waiter)
{
  STATIC_ASSERT(offsetof(struct slab_journal, resourceWaiter) == 0);
  return (struct slab_journal *) waiter;
}

/**
 * Return the slab journal from the flush waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline struct slab_journal *
slabJournalFromFlushWaiter(struct waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (struct slab_journal *)
    ((uintptr_t) waiter - offsetof(struct slab_journal, flushWaiter));
}

/**********************************************************************/
struct slab_journal *slabJournalFromDirtyNode(RingNode *node)
{
  if (node == NULL) {
    return NULL;
  }
  return (struct slab_journal *) ((uintptr_t) node
                                  - offsetof(struct slab_journal, dirtyNode));
}

/**
 * Return the slab journal from the slab summary waiter.
 *
 * @param waiter  The waiter
 *
 * @return The slab journal
 **/
__attribute__((warn_unused_result))
static inline struct slab_journal *
slabJournalFromSlabSummaryWaiter(struct waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return (struct slab_journal *)
    ((uintptr_t) waiter - offsetof(struct slab_journal, slabSummaryWaiter));
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
static inline PhysicalBlockNumber
getBlockNumber(struct slab_journal *journal,
               SequenceNumber       sequence)
{
  TailBlockOffset offset = getSlabJournalBlockOffset(journal, sequence);
  return (journal->slab->journalOrigin + offset);
}

/**
 * Get the lock object for a slab journal block by sequence number.
 *
 * @param journal         vdo_slab journal to retrieve from
 * @param sequenceNumber  Sequence number of the block
 *
 * @return the lock object for the given sequence number
 **/
__attribute__((warn_unused_result))
static inline struct journal_lock *getLock(struct slab_journal *journal,
                                           SequenceNumber       sequenceNumber)
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
static inline bool isVDOReadOnly(struct slab_journal *journal)
{
  return is_read_only(journal->slab->allocator->read_only_notifier);
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
static inline bool mustMakeEntriesToFlush(struct slab_journal *journal)
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
static inline bool isReaping(struct slab_journal *journal)
{
  return (journal->head != journal->unreapable);
}

/**********************************************************************/
bool isSlabJournalActive(struct slab_journal *journal)
{
  return (mustMakeEntriesToFlush(journal)
          || isReaping(journal)
          || journal->waitingToCommit
          || !isRingEmpty(&journal->uncommittedBlocks)
          || journal->updatingSlabSummary);
}

/**
 * Initialize tail block as a new block.
 *
 * @param journal  The journal whose tail block is being initialized
 **/
static void initializeTailBlock(struct slab_journal *journal)
{
  struct slab_journal_block_header *header = &journal->tailHeader;
  header->sequenceNumber                   = journal->tail;
  header->entryCount                       = 0;
  header->hasBlockMapIncrements            = false;
}

/**
 * Set all journal fields appropriately to start journaling.
 *
 * @param journal  The journal to be reset, based on its tail sequence number
 **/
static void initializeJournalState(struct slab_journal *journal)
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
static bool blockIsFull(struct slab_journal *journal)
{
  JournalEntryCount count = journal->tailHeader.entryCount;
  return (journal->tailHeader.hasBlockMapIncrements
          ? (journal->fullEntriesPerBlock == count)
          : (journal->entriesPerBlock == count));
}

/**********************************************************************/
static void addEntries(struct slab_journal *journal);
static void updateTailBlockLocation(struct slab_journal *journal);
static void releaseJournalLocks(struct waiter *waiter, void *context);

/**********************************************************************/
int makeSlabJournal(struct block_allocator   *allocator,
                    struct vdo_slab          *slab,
                    struct recovery_journal  *recoveryJournal,
                    struct slab_journal     **journalPtr)
{
  struct slab_journal *journal;
  const SlabConfig *slabConfig = get_slab_config(allocator->depot);
  int result = ALLOCATE_EXTENDED(struct slab_journal,
                                 slabConfig->slabJournalBlocks,
                                 struct journal_lock, __func__, &journal);
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
  journal->events              = &allocator->slab_journal_statistics;
  journal->recoveryJournal     = recoveryJournal;
  journal->summary             = get_slab_summary_zone(allocator);
  journal->tail                = 1;
  journal->head                = 1;

  journal->flushingDeadline = journal->flushingThreshold;
  // Set there to be some time between the deadline and the blocking threshold,
  // so that hopefully all are done before blocking.
  if ((journal->blockingThreshold - journal->flushingThreshold) > 5) {
    journal->flushingDeadline = journal->blockingThreshold - 5;
  }

  journal->slabSummaryWaiter.callback = releaseJournalLocks;

  result = ALLOCATE(VDO_BLOCK_SIZE, char, "struct packed_slab_journal_block",
                    (char **) &journal->block);
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
void freeSlabJournal(struct slab_journal **journalPtr)
{
  struct slab_journal *journal = *journalPtr;
  if (journal == NULL) {
    return;
  }

  FREE(journal->block);
  FREE(journal);
  *journalPtr = NULL;
}

/**********************************************************************/
bool isSlabJournalBlank(const struct slab_journal *journal)
{
  return ((journal != NULL)
          && (journal->tail == 1)
          && (journal->tailHeader.entryCount == 0));
}

/**********************************************************************/
bool isSlabJournalDirty(const struct slab_journal *journal)
{
  return (journal->recoveryLock != 0);
}

/**
 * Put a slab journal on the dirty ring of its allocator in the correct order.
 *
 * @param journal  The journal to be marked dirty
 * @param lock     The recovery journal lock held by the slab journal
 **/
static void markSlabJournalDirty(struct slab_journal *journal,
                                 SequenceNumber       lock)
{
  ASSERT_LOG_ONLY(!isSlabJournalDirty(journal), "slab journal was clean");

  journal->recoveryLock = lock;
  RingNode *dirtyRing   = &journal->slab->allocator->dirty_slab_journals;
  RingNode *node        = dirtyRing->prev;
  while (node != dirtyRing) {
    struct slab_journal *dirtyJournal = slabJournalFromDirtyNode(node);
    if (dirtyJournal->recoveryLock <= journal->recoveryLock) {
      break;
    }

    node = node->prev;
  }

  pushRingNode(node->next, &journal->dirtyNode);
}

/**********************************************************************/
static void markSlabJournalClean(struct slab_journal *journal)
{
  journal->recoveryLock = 0;
  unspliceRingNode(&journal->dirtyNode);
}

/**
 * Implements WaiterCallback. This callback is invoked on all vios waiting
 * to make slab journal entries after the VDO has gone into read-only mode.
 **/
static void abortWaiter(struct waiter *waiter,
                        void          *context __attribute__((unused)))
{
  continueDataVIO(waiterAsDataVIO(waiter), VDO_READ_ONLY);
}

/**********************************************************************/
void abortSlabJournalWaiters(struct slab_journal *journal)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == journal->slab->allocator->thread_id),
                  "abortSlabJournalWaiters() called on correct thread");
  notifyAllWaiters(&journal->entryWaiters, abortWaiter, journal);
  checkIfSlabDrained(journal->slab);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All vios waiting for to make entries
 * will be awakened with an error. All flushes will complete as soon as all
 * pending IO is done.
 *
 * @param journal    The journal which has failed
 * @param errorCode  The error result triggering this call
 **/
static void enterJournalReadOnlyMode(struct slab_journal *journal,
                                     int                  errorCode)
{
  enter_read_only_mode(journal->slab->allocator->read_only_notifier, errorCode);
  abortSlabJournalWaiters(journal);
}

/**
 * Actually advance the head of the journal now that any necessary flushes
 * are complete.
 *
 * @param journal  The journal to be reaped
 **/
static void finishReaping(struct slab_journal *journal)
{
  journal->head = journal->unreapable;
  addEntries(journal);
  checkIfSlabDrained(journal->slab);
}

/**********************************************************************/
static void reapSlabJournal(struct slab_journal *journal);

/**
 * Finish reaping now that we have flushed the lower layer and then try
 * reaping again in case we deferred reaping due to an outstanding vio.
 *
 * @param completion  The flush vio
 **/
static void completeReaping(struct vdo_completion *completion)
{
  struct vio_pool_entry *entry   = completion->parent;
  struct slab_journal   *journal = entry->parent;
  return_vio(journal->slab->allocator, entry);
  finishReaping(journal);
  reapSlabJournal(journal);
}

/**
 * Handle an error flushing the lower layer.
 *
 * @param completion  The flush vio
 **/
static void handleFlushError(struct vdo_completion *completion)
{
  struct slab_journal *journal
    = ((struct vio_pool_entry *) completion->parent)->parent;
  enterJournalReadOnlyMode(journal, completion->result);
  completeReaping(completion);
}

/**
 * A waiter callback for getting a vio with which to flush the lower
 * layer prior to reaping.
 *
 * @param waiter      The journal as a flush waiter
 * @param vioContext  The newly acquired flush vio
 **/
static void flushForReaping(struct waiter *waiter, void *vioContext)
{
  struct slab_journal   *journal = slabJournalFromFlushWaiter(waiter);
  struct vio_pool_entry *entry   = vioContext;
  struct vio            *vio     = entry->vio;

  entry->parent                    = journal;
  vio->completion.callbackThreadID = journal->slab->allocator->thread_id;
  launchFlush(vio, completeReaping, handleFlushError);
}

/**
 * Conduct a reap on a slab journal to reclaim unreferenced blocks.
 *
 * @param journal  The slab journal
 **/
static void reapSlabJournal(struct slab_journal *journal)
{
  if (isReaping(journal)) {
    // We already have a reap in progress so wait for it to finish.
    return;
  }

  if (isUnrecoveredSlab(journal->slab) || !is_normal(&journal->slab->state)
      || isVDOReadOnly(journal)) {
    // We must not reap in the first two cases, and there's no point in
    // read-only mode.
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

  PhysicalLayer *layer = journal->slab->allocator->completion.layer;
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
  int result = acquire_vio(journal->slab->allocator, &journal->flushWaiter);
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
static void releaseJournalLocks(struct waiter *waiter, void *context)
{
  struct slab_journal *journal = slabJournalFromSlabSummaryWaiter(waiter);
  int                  result  = *((int *) context);
  if (result != VDO_SUCCESS) {
    if (result != VDO_READ_ONLY) {
      // Don't bother logging what might be lots of errors if we are already
      // in read-only mode.
      logErrorWithStringError(result, "failed slab summary update %llu",
                              journal->summarized);
    }

    journal->updatingSlabSummary = false;
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
  SequenceNumber i;
  for (i = journal->summarized - 1; i >= first; i--) {
    // Release the lock the summarized block held on the recovery journal.
    // (During replay, recoveryStart will always be 0.)
    if (journal->recoveryJournal != NULL) {
      ZoneCount zoneNumber = journal->slab->allocator->zone_number;
      release_recovery_journal_block_reference(journal->recoveryJournal,
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
static void updateTailBlockLocation(struct slab_journal *journal)
{
  if (journal->updatingSlabSummary || isVDOReadOnly(journal)
      || (journal->lastSummarized >= journal->nextCommit)) {
    checkIfSlabDrained(journal->slab);
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
   * vdo_slab journal can only reap past sequence number 1 when all the
   * refCounts for this slab have been written to the layer. Therefore,
   * indicate that the refCounts must be loaded when the journal head has
   * reaped past sequence number 1.
   */
  TailBlockOffset blockOffset
    = getSlabJournalBlockOffset(journal, journal->summarized);
  updateSlabSummaryEntry(journal->summary, &journal->slabSummaryWaiter,
                         journal->slab->slabNumber, blockOffset,
                         (journal->head > 1), false, freeBlockCount);
}

/**********************************************************************/
void reopenSlabJournal(struct slab_journal *journal)
{
  ASSERT_LOG_ONLY(journal->tailHeader.entryCount == 0,
                  "vdo_slab journal's active block empty before reopening");
  journal->head       = journal->tail;
  initializeJournalState(journal);

  // Ensure no locks are spuriously held on an empty journal.
  SequenceNumber block;
  for (block = 1; block <= journal->size; block++) {
    ASSERT_LOG_ONLY((getLock(journal, block)->count == 0),
                    "Scrubbed journal's block %llu is not locked",
                    block);
  }

  addEntries(journal);
}

/**********************************************************************/
static SequenceNumber
getCommittingSequenceNumber(const struct vio_pool_entry *entry)
{
  const struct packed_slab_journal_block *block = entry->buffer;
  return getUInt64LE(block->header.fields.sequenceNumber);
}

/**
 * Handle post-commit processing. This is the callback registered by
 * writeSlabJournalBlock().
 *
 * @param completion  The write vio as a completion
 **/
static void completeWrite(struct vdo_completion *completion)
{
  int                    writeResult = completion->result;
  struct vio_pool_entry *entry       = completion->parent;
  struct slab_journal   *journal     = entry->parent;

  SequenceNumber committed = getCommittingSequenceNumber(entry);
  unspliceRingNode(&entry->node);
  return_vio(journal->slab->allocator, entry);

  if (writeResult != VDO_SUCCESS) {
    logErrorWithStringError(writeResult,
                            "cannot write slab journal block %llu",
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
    struct vio_pool_entry *oldest
      = asVIOPoolEntry(journal->uncommittedBlocks.next);
    journal->nextCommit = getCommittingSequenceNumber(oldest);
  }

  updateTailBlockLocation(journal);
}

/**
 * Callback from acquire_vio() registered in commitSlabJournalTail().
 *
 * @param waiter      The vio pool waiter which was just notified
 * @param vioContext  The vio pool entry for the write
 **/
static void writeSlabJournalBlock(struct waiter *waiter, void *vioContext)
{
  struct slab_journal    *journal = slabJournalFromResourceWaiter(waiter);
  struct vio_pool_entry  *entry   = vioContext;
  struct slab_journal_block_header *header = &journal->tailHeader;

  header->head = journal->head;
  pushRingNode(&journal->uncommittedBlocks, &entry->node);
  packSlabJournalBlockHeader(header, &journal->block->header);

  // Copy the tail block into the vio.
  memcpy(entry->buffer, journal->block, VDO_BLOCK_SIZE);

  int unusedEntries = journal->entriesPerBlock - header->entryCount;
  ASSERT_LOG_ONLY(unusedEntries >= 0, "vdo_slab journal block is not overfull");
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
  entry->vio->completion.callbackThreadID = journal->slab->allocator->thread_id;
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
  if (journal->slab->state.state == ADMIN_STATE_WAITING_FOR_RECOVERY) {
    finish_operation_with_result(&journal->slab->state,
                                 (isVDOReadOnly(journal)
                                  ? VDO_READ_ONLY : VDO_SUCCESS));
    return;
  }

  addEntries(journal);
}

/**********************************************************************/
void commitSlabJournalTail(struct slab_journal *journal)
{
  if ((journal->tailHeader.entryCount == 0)
      && mustMakeEntriesToFlush(journal)) {
    // There are no entries at the moment, but there are some waiters, so defer
    // initiating the flush until those entries are ready to write.
    return;
  }

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
  int result = acquire_vio(journal->slab->allocator, &journal->resourceWaiter);
  if (result != VDO_SUCCESS) {
    journal->waitingToCommit = false;
    enterJournalReadOnlyMode(journal, result);
    return;
  }
}

/**********************************************************************/
void encodeSlabJournalEntry(struct slab_journal_block_header *tailHeader,
                            SlabJournalPayload               *payload,
                            SlabBlockNumber                   sbn,
                            JournalOperation                  operation)
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
                       is_increment_operation(operation));
}

/**********************************************************************/
struct slab_journal_entry
decodeSlabJournalEntry(struct packed_slab_journal_block   *block,
                       JournalEntryCount                   entry_count)
{
  struct slab_journal_entry entry
    = unpackSlabJournalEntry(&block->payload.entries[entry_count]);
  if (block->header.fields.hasBlockMapIncrements
      && ((block->payload.fullEntries.entryTypes[entry_count / 8]
           & ((byte) 1 << (entry_count % 8))) != 0)) {
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
static void addEntry(struct slab_journal        *journal,
                     PhysicalBlockNumber         pbn,
                     JournalOperation            operation,
                     const struct journal_point *recoveryPoint)
{
  int result = ASSERT(before_journal_point(&journal->tailHeader.recoveryPoint,
                                           recoveryPoint),
                      "recovery journal point is monotonically increasing, "
                      "recovery point: %llu.%u, "
                      "block recovery point: %llu.%u",
                      recoveryPoint->sequence_number,
		      recoveryPoint->entry_count,
                      journal->tailHeader.recoveryPoint.sequence_number,
                      journal->tailHeader.recoveryPoint.entry_count);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }

  struct packed_slab_journal_block *block = journal->block;
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
bool attemptReplayIntoSlabJournal(struct slab_journal   *journal,
                                  PhysicalBlockNumber    pbn,
                                  JournalOperation       operation,
                                  struct journal_point  *recoveryPoint,
                                  struct vdo_completion *parent)
{
  // Only accept entries after the current recovery point.
  if (!before_journal_point(&journal->tailHeader.recoveryPoint,
                            recoveryPoint)) {
    return true;
  }

  struct slab_journal_block_header *header = &journal->tailHeader;
  if ((header->entryCount >= journal->fullEntriesPerBlock)
      && (header->hasBlockMapIncrements ||
          (operation == BLOCK_MAP_INCREMENT))) {
    // The tail block does not have room for the entry we are attempting
    // to add so commit the tail block now.
    commitSlabJournalTail(journal);
  }

  if (journal->waitingToCommit) {
    start_operation_with_waiter(&journal->slab->state,
                                ADMIN_STATE_WAITING_FOR_RECOVERY, parent, NULL);
    return false;
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
  return true;
}

/**
 * Check whether the journal should be saving reference blocks out.
 *
 * @param journal       The journal to check
 *
 * @return true if the journal should be requesting reference block writes
 **/
static bool requiresFlushing(const struct slab_journal *journal)
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
static bool requiresReaping(const struct slab_journal *journal)
{
  BlockCount journalLength = (journal->tail - journal->head);
  return (journalLength >= journal->blockingThreshold);
}

/**********************************************************************/
bool requiresScrubbing(const struct slab_journal *journal)
{
  BlockCount journalLength = (journal->tail - journal->head);
  return (journalLength >= journal->scrubbingThreshold);
}

/**
 * Implements WaiterCallback. This callback is invoked by addEntries() once
 * it has determined that we are ready to make another entry in the slab
 * journal.
 *
 * @param waiter        The vio which should make an entry now
 * @param context       The slab journal to make an entry in
 **/
static void addEntryFromWaiter(struct waiter *waiter, void *context)
{
  struct data_vio *dataVIO = waiterAsDataVIO(waiter);
  struct slab_journal *journal = (struct slab_journal *) context;
  struct slab_journal_block_header *header = &journal->tailHeader;
  SequenceNumber recoveryBlock = dataVIO->recoveryJournalPoint.sequence_number;

  if (header->entryCount == 0) {
    /*
     * This is the first entry in the current tail block, so get a lock
     * on the recovery journal which we will hold until this tail block is
     * committed.
     */
    getLock(journal, header->sequenceNumber)->recoveryStart = recoveryBlock;
    if (journal->recoveryJournal != NULL) {
      ZoneCount zoneNumber = journal->slab->allocator->zone_number;
      acquire_recovery_journal_block_reference(journal->recoveryJournal,
                                               recoveryBlock,
                                               ZONE_TYPE_PHYSICAL,
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
      save_several_reference_blocks(journal->slab->referenceCounts,
                                    blocksToDeadline + 1);
    }
  }

  struct journal_point slabJournalPoint = {
    .sequence_number = header->sequenceNumber,
    .entry_count     = header->entryCount,
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
static inline bool isNextEntryABlockMapIncrement(struct slab_journal *journal)
{
  struct data_vio *dataVIO
    = waiterAsDataVIO(getFirstWaiter(&journal->entryWaiters));
  return (dataVIO->operation.type == BLOCK_MAP_INCREMENT);
}

/**
 * Add as many entries as possible from the queue of vios waiting to make
 * entries. By processing the queue in order, we ensure that slab journal
 * entries are made in the same order as recovery journal entries for the
 * same increment or decrement.
 *
 * @param journal  The journal to which entries may be added
 **/
static void addEntries(struct slab_journal *journal)
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

    struct slab_journal_block_header *header = &journal->tailHeader;
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

    // If the slab is over the blocking threshold, make the vio wait.
    if (requiresReaping(journal)) {
      relaxedAdd64(&journal->events->blockedCount, 1);
      save_dirty_reference_blocks(journal->slab->referenceCounts);
      break;
    }

    if (header->entryCount == 0) {
      struct journal_lock *lock = getLock(journal, header->sequenceNumber);
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
        save_dirty_reference_blocks(journal->slab->referenceCounts);
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
        acquire_dirty_block_locks(journal->slab->referenceCounts);
      }
    }

    notifyNextWaiter(&journal->entryWaiters, addEntryFromWaiter, journal);
  }

  journal->addingEntries = false;

  // If there are no waiters, and we are flushing or saving, commit the
  // tail block.
  if (isSlabDraining(journal->slab) && !is_suspending(&journal->slab->state)
      && !hasWaiters(&journal->entryWaiters)) {
    commitSlabJournalTail(journal);
  }
}

/**********************************************************************/
void addSlabJournalEntry(struct slab_journal *journal, struct data_vio *dataVIO)
{
  if (!isSlabOpen(journal->slab)) {
    continueDataVIO(dataVIO, VDO_INVALID_ADMIN_STATE);
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
    increase_scrubbing_priority(journal->slab);
  }

  addEntries(journal);
}

/**********************************************************************/
void adjustSlabJournalBlockReference(struct slab_journal *journal,
                                     SequenceNumber       sequenceNumber,
                                     int                  adjustment)
{
  if (sequenceNumber == 0) {
    return;
  }

  if (isReplayingSlab(journal->slab)) {
    // Locks should not be used during offline replay.
    return;
  }

  ASSERT_LOG_ONLY((adjustment != 0), "adjustment must be non-zero");
  struct journal_lock *lock = getLock(journal, sequenceNumber);
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
bool releaseRecoveryJournalLock(struct slab_journal *journal,
                                SequenceNumber       recoveryLock)
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
void drainSlabJournal(struct slab_journal *journal)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == journal->slab->allocator->thread_id),
                  "drainSlabJournal() called on correct thread");
  if (is_quiescing(&journal->slab->state)) {
    // XXX: we should revisit this assertion since it is no longer clear what
    //      it is for.
    ASSERT_LOG_ONLY((!(slabIsRebuilding(journal->slab)
                       && hasWaiters(&journal->entryWaiters))),
                    "slab is recovered or has no waiters");
  }

  switch (journal->slab->state.state) {
  case ADMIN_STATE_REBUILDING:
  case ADMIN_STATE_SUSPENDING:
  case ADMIN_STATE_SAVE_FOR_SCRUBBING:
    break;

  default:
    commitSlabJournalTail(journal);
  }
}

/**
 * Finish the decode process by returning the vio and notifying the slab that
 * we're done.
 *
 * @param completion  The vio as a completion
 **/
static void finishDecodingJournal(struct vdo_completion *completion)
{
  int                    result  = completion->result;
  struct vio_pool_entry *entry   = completion->parent;
  struct slab_journal   *journal = entry->parent;
  return_vio(journal->slab->allocator, entry);
  notifySlabJournalIsLoaded(journal->slab, result);
}

/**
 * Set up the in-memory journal state to the state which was written to disk.
 * This is the callback registered in readSlabJournalTail().
 *
 * @param completion  The vio which was used to read the journal tail
 **/
static void setDecodedState(struct vdo_completion *completion)
{
  struct vio_pool_entry            *entry   = completion->parent;
  struct slab_journal              *journal = entry->parent;
  struct packed_slab_journal_block *block   = entry->buffer;

  struct slab_journal_block_header header;
  unpackSlabJournalBlockHeader(&block->header, &header);

  if ((header.metadataType != VDO_METADATA_SLAB_JOURNAL)
      || (header.nonce != journal->slab->allocator->nonce)) {
    finishDecodingJournal(completion);
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
  finishDecodingJournal(completion);
}

/**
 * This reads the slab journal tail block by using a vio acquired from the vio
 * pool. This is the success callback from acquireVIOFromPool() when decoding
 * the slab journal.
 *
 * @param waiter      The vio pool waiter which has just been notified
 * @param vioContext  The vio pool entry given to the waiter
 **/
static void readSlabJournalTail(struct waiter *waiter, void *vioContext)
{
  struct slab_journal   *journal = slabJournalFromResourceWaiter(waiter);
  struct vdo_slab       *slab    = journal->slab;
  struct vio_pool_entry *entry   = vioContext;
  TailBlockOffset lastCommitPoint
    = getSummarizedTailBlockOffset(journal->summary, slab->slabNumber);
  entry->parent = journal;


  // struct vdo_slab summary keeps the commit point offset, so the tail block is the
  // block before that. Calculation supports small journals in unit tests.
  TailBlockOffset tailBlock = ((lastCommitPoint == 0)
                               ? (TailBlockOffset) (journal->size - 1)
                               : (lastCommitPoint - 1));
  entry->vio->completion.callbackThreadID = slab->allocator->thread_id;
  launchReadMetadataVIO(entry->vio, slab->journalOrigin + tailBlock,
                        setDecodedState, finishDecodingJournal);
}

/**********************************************************************/
void decodeSlabJournal(struct slab_journal *journal)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == journal->slab->allocator->thread_id),
                  "decodeSlabJournal() called on correct thread");
  struct vdo_slab *slab = journal->slab;
  TailBlockOffset lastCommitPoint
    = getSummarizedTailBlockOffset(journal->summary, slab->slabNumber);
  if ((lastCommitPoint == 0)
      && !mustLoadRefCounts(journal->summary, slab->slabNumber)) {
    /*
     * This slab claims that it has a tail block at (journal->size - 1), but
     * a head of 1. This is impossible, due to the scrubbing threshold, on
     * a real system, so don't bother reading the (bogus) data off disk.
     */
    ASSERT_LOG_ONLY(((journal->size < 16)
                     || (journal->scrubbingThreshold < (journal->size - 1))),
                    "Scrubbing threshold protects against reads of unwritten"
                    "slab journal blocks");
    notifySlabJournalIsLoaded(slab, VDO_SUCCESS);
    return;
  }

  journal->resourceWaiter.callback = readSlabJournalTail;
  int result = acquire_vio(slab->allocator, &journal->resourceWaiter);
  if (result != VDO_SUCCESS) {
    notifySlabJournalIsLoaded(slab, result);
  }
}

/**********************************************************************/
void dumpSlabJournal(const struct slab_journal *journal)
{
  logInfo("  slab journal: entryWaiters=%zu waitingToCommit=%s"
          " updatingSlabSummary=%s head=%llu unreapable=%" PRIu64
          " tail=%llu nextCommit=%llu summarized=%" PRIu64
          " lastSummarized=%llu recoveryJournalLock=%" PRIu64
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
