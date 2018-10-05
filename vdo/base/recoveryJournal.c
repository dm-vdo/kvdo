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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournal.c#13 $
 */

#include "recoveryJournal.h"
#include "recoveryJournalInternals.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "constants.h"
#include "dataVIO.h"
#include "extent.h"
#include "header.h"
#include "numUtils.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalBlock.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "waitQueue.h"

typedef struct {
  SequenceNumber journalStart;       // Sequence number to start the journal
  BlockCount     logicalBlocksUsed;  // Number of logical blocks used by VDO
  BlockCount     blockMapDataBlocks; // Number of block map pages allocated
} __attribute__((packed)) RecoveryJournalState7_0;

static const Header RECOVERY_JOURNAL_HEADER_7_0 = {
  .id = RECOVERY_JOURNAL,
  .version = {
    .majorVersion = 7,
    .minorVersion = 0,
  },
  .size = sizeof(RecoveryJournalState7_0),
};

static const uint64_t RECOVERY_COUNT_MASK = 0xff;

enum {
  /*
   * The number of reserved blocks must be large enough to prevent a
   * new recovery journal block write from overwriting a block which
   * appears to still be a valid head block of the journal. Currently,
   * that means reserving enough space for all 2048 VIOs, or 8 blocks.
   */
  RECOVERY_JOURNAL_RESERVED_BLOCKS = 8,
};

/**********************************************************************/
const char *getJournalOperationName(JournalOperation operation)
{
  switch (operation) {
  case DATA_DECREMENT:
    return "data decrement";

  case DATA_INCREMENT:
    return "data increment";

  case BLOCK_MAP_DECREMENT:
    return "block map decrement";

  case BLOCK_MAP_INCREMENT:
    return "block map increment";

  default:
    return "unknown journal operation";
  }
}

/**
 * Get a block from the end of the free list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static RecoveryJournalBlock *popFreeList(RecoveryJournal *journal)
{
  return blockFromRingNode(popRingNode(&journal->freeTailBlocks));
}

/**
 * Get a block from the end of the active list.
 *
 * @param journal  The journal
 *
 * @return The block or <code>NULL</code> if the list is empty
 **/
static RecoveryJournalBlock *popActiveList(RecoveryJournal *journal)
{
  return blockFromRingNode(popRingNode(&journal->activeTailBlocks));
}

/**
 * Assert that we are running on the journal thread.
 *
 * @param journal       The journal
 * @param functionName  The function doing the check (for logging)
 **/
static void assertOnJournalThread(RecoveryJournal *journal,
                                  const char      *functionName)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == journal->threadID),
                  "%s() called on journal thread", functionName);
}

/**
 * WaiterCallback implementation invoked whenever a DataVIO is to be released
 * from the journal, either because its entry was committed to disk,
 * or because there was an error.
 **/
static void continueWaiter(Waiter *waiter, void *context)
{
  DataVIO *dataVIO = waiterAsDataVIO(waiter);
  dataVIOAddTraceRecord(dataVIO,
                        THIS_LOCATION("$F($j-$js);"
                                      "cb=continueJournalWaiter($j-$js)"));
  int waitResult = *((int *) context);
  continueDataVIO(dataVIO, waitResult);
}

/**
 * Check whether the journal has any waiters on any blocks.
 *
 * @param journal  The journal in question
 *
 * @return <code>true</code> if any block has a waiter
 **/
static inline bool hasBlockWaiters(RecoveryJournal *journal)
{
  // Either the first active tail block (if it exists) has waiters,
  // or no active tail block has waiters.
  if (isRingEmpty(&journal->activeTailBlocks)) {
    return false;
  }

  RecoveryJournalBlock *block
    = blockFromRingNode(journal->activeTailBlocks.next);
  return (hasWaiters(&block->entryWaiters)
          || hasWaiters(&block->commitWaiters));
}

/**
 * Check whether the journal should close and may do so, and if so, notify
 * the completion waiting for the close.
 *
 * @param journal The journal which may have just closed
 *
 * @return <code>true</code> if the journal has closed
 **/
static inline bool checkForClosure(RecoveryJournal *journal)
{
  if (isReadOnly(journal->readOnlyContext)) {
    int notifyContext = VDO_READ_ONLY;
    notifyAllWaiters(&journal->decrementWaiters, continueWaiter,
                     &notifyContext);
    notifyAllWaiters(&journal->incrementWaiters, continueWaiter,
                     &notifyContext);
  }

  if (!journal->closeRequested || journal->completion.complete
      || journal->reaping || hasBlockWaiters(journal)
      || hasWaiters(&journal->incrementWaiters)
      || hasWaiters(&journal->decrementWaiters)) {
    return false;
  }

  RecoveryJournalBlock *block;
  while ((block = popActiveList(journal)) != NULL) {
    if (!isReadOnly(journal->readOnlyContext)) {
      ASSERT_LOG_ONLY(!isRecoveryBlockDirty(block),
                      "all blocks in a journal being closed must be inactive");
    }
    pushRingNode(&journal->freeTailBlocks, &block->ringNode);
  }

  finishCompletion(&journal->completion, (isReadOnly(journal->readOnlyContext)
                                          ? VDO_READ_ONLY
                                          : VDO_SUCCESS));
  return true;
}

/**********************************************************************/
void notifyRecoveryJournalOfReadOnlyMode(RecoveryJournal *journal)
{
  checkForClosure(journal);
}

/**
 * Put the journal in read-only mode. All attempts to add entries after
 * this function is called will fail. All VIOs waiting for commits will be
 * awakened with an error.
 *
 * @param journal    The journal which has failed
 * @param errorCode  The error result triggering this call
 **/
static void enterJournalReadOnlyMode(RecoveryJournal *journal, int errorCode)
{
  enterReadOnlyMode(journal->readOnlyContext, errorCode);
  checkForClosure(journal);
}

/**********************************************************************/
SequenceNumber getCurrentJournalSequenceNumber(RecoveryJournal *journal)
{
  return journal->tail;
}

/**
 * Get the head of the recovery journal, which is the lowest sequence number of
 * the block map head and the slab journal head.
 *
 * @param journal    The journal
 *
 * @return the head of the journal
 **/
static inline SequenceNumber getRecoveryJournalHead(RecoveryJournal *journal)
{
  return minSequenceNumber(journal->blockMapHead, journal->slabJournalHead);
}

/**
 * Compute the recovery count byte for a given recovery count.
 *
 * @param recoveryCount  The recovery count
 *
 * @return The byte corresponding to the recovery count
 **/
__attribute__((warn_unused_result))
static inline uint8_t computeRecoveryCountByte(uint64_t recoveryCount)
{
  return (uint8_t) (recoveryCount & RECOVERY_COUNT_MASK);
}

/**
 * Check whether the journal is over the threshold, and if so, force the oldest
 * slab journal tail block to commit.
 *
 * @param journal    The journal
 **/
static void checkSlabJournalCommitThreshold(RecoveryJournal *journal)
{
  BlockCount currentLength = journal->tail - journal->slabJournalHead;
  if (currentLength > journal->slabJournalCommitThreshold) {
    journal->events.slabJournalCommitsRequested++;
    commitOldestSlabJournalTailBlocks(journal->depot,
                                      journal->slabJournalHead);
  }
}

/**********************************************************************/
static void reapRecoveryJournal(RecoveryJournal *journal);
static void assignEntries(RecoveryJournal *journal);

/**
 * Finish reaping the journal.
 *
 * @param journal The journal being reaped
 **/
static void finishReaping(RecoveryJournal *journal)
{
  SequenceNumber oldHead    = getRecoveryJournalHead(journal);
  journal->blockMapHead     = journal->blockMapReapHead;
  journal->slabJournalHead  = journal->slabJournalReapHead;
  BlockCount blocksReaped   = getRecoveryJournalHead(journal) - oldHead;
  journal->availableSpace  += blocksReaped * journal->entriesPerBlock;
  journal->reaping          = false;
  checkSlabJournalCommitThreshold(journal);
  assignEntries(journal);
  checkForClosure(journal);
}

/**
 * Finish reaping the journal after flushing the lower layer. This is the
 * callback registered in reapRecoveryJournal().
 *
 * @param completion  The journal's flush VIO
 **/
static void completeReaping(VDOCompletion *completion)
{
  RecoveryJournal *journal = completion->parent;
  finishReaping(journal);

  // Try reaping again in case more locks were released while flush was out.
  reapRecoveryJournal(journal);
}

/**
 * Handle an error when flushing the lower layer due to reaping.
 *
 * @param completion  The journal's flush VIO
 **/
static void handleFlushError(VDOCompletion *completion)
{
  RecoveryJournal *journal = completion->parent;
  journal->reaping = false;
  enterJournalReadOnlyMode(journal, completion->result);
}

/**
 * Set a new active block. If there are no free blocks, the active block
 * will be set to <code>NULL</code>.
 *
 * @param journal  The journal which needs a new active block
 **/
static void setActiveBlock(RecoveryJournal *journal)
{
  journal->activeBlock = popFreeList(journal);
  pushRingNode(&journal->activeTailBlocks, &journal->activeBlock->ringNode);
  initializeRecoveryBlock(journal->activeBlock);
}

/**
 * Set all journal fields appropriately to start journaling from the current
 * active block.
 *
 * @param journal  The journal to be reset based on its active block
 **/
static void initializeJournalState(RecoveryJournal *journal)
{
  journal->appendPoint.sequenceNumber = journal->tail;
  journal->lastWriteAcknowledged      = journal->tail;
  journal->blockMapHead               = journal->tail;
  journal->slabJournalHead            = journal->tail;
  journal->blockMapReapHead           = journal->tail;
  journal->slabJournalReapHead        = journal->tail;
  journal->blockMapHeadBlockNumber
    = getRecoveryJournalBlockNumber(journal, journal->blockMapHead);
  journal->slabJournalHeadBlockNumber
    = getRecoveryJournalBlockNumber(journal, journal->slabJournalHead);
}

/**********************************************************************/
BlockCount getRecoveryJournalLength(BlockCount journalSize)
{
  BlockCount reservedBlocks = journalSize / 4;
  if (reservedBlocks > RECOVERY_JOURNAL_RESERVED_BLOCKS) {
    reservedBlocks = RECOVERY_JOURNAL_RESERVED_BLOCKS;
  }
  return (journalSize - reservedBlocks);
}

/**
 * Attempt to reap the journal now that all the locks on some journal block
 * have been released. This is the callback registered with the lock counter.
 *
 * @param completion  The lock counter completion
 **/
static void reapRecoveryJournalCallback(VDOCompletion *completion)
{
  RecoveryJournal *journal = (RecoveryJournal *) completion->parent;
  // The acknowledgement must be done before reaping so that there is no
  // race between acknowledging the notification and unlocks wishing to notify.
  acknowledgeUnlock(journal->lockCounter);
  reapRecoveryJournal(journal);
  checkSlabJournalCommitThreshold(journal);
}

/**********************************************************************/
int makeRecoveryJournal(Nonce                 nonce,
                        PhysicalLayer        *layer,
                        Partition            *partition,
                        uint64_t              recoveryCount,
                        BlockCount            journalSize,
                        BlockCount            tailBufferSize,
                        ReadOnlyModeContext  *readOnlyContext,
                        const ThreadConfig   *threadConfig,
                        RecoveryJournal     **journalPtr)
{
  RecoveryJournal *journal;
  int result = ALLOCATE(1, RecoveryJournal, __func__, &journal);
  if (result != VDO_SUCCESS) {
    return result;
  }

  initializeCompletion(&journal->completion, RECOVERY_JOURNAL_COMPLETION,
                       layer);

  initializeRing(&journal->freeTailBlocks);
  initializeRing(&journal->activeTailBlocks);

  journal->threadID        = getJournalZoneThread(threadConfig);
  journal->partition       = partition;
  journal->nonce           = nonce;
  journal->recoveryCount   = computeRecoveryCountByte(recoveryCount);
  journal->size            = journalSize;
  journal->readOnlyContext = readOnlyContext;
  journal->tail            = 1;
  journal->slabJournalCommitThreshold = (journalSize * 2) / 3;
  initializeJournalState(journal);

  journal->entriesPerBlock = RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
  BlockCount journalLength = getRecoveryJournalLength(journalSize);
  journal->availableSpace  = journal->entriesPerBlock * journalLength;

  // Only make the tail buffer and VIO in normal operation since the formatter
  // doesn't need them.
  if (layer->createMetadataVIO != NULL) {
    for (BlockCount i = 0; i < tailBufferSize; i++) {
      RecoveryJournalBlock *block;
      result = makeRecoveryBlock(journal, &block);
      if (result != VDO_SUCCESS) {
        freeRecoveryJournal(&journal);
        return result;
      }

      pushRingNode(&journal->freeTailBlocks, &block->ringNode);
    }

    result = makeLockCounter(layer, journal, reapRecoveryJournalCallback,
                             journal->threadID, threadConfig->logicalZoneCount,
                             threadConfig->physicalZoneCount, journal->size,
                             &journal->lockCounter);
    if (result != VDO_SUCCESS) {
      freeRecoveryJournal(&journal);
      return result;
    }

    result = ALLOCATE(VDO_BLOCK_SIZE, char, "journal flush data",
                      &journal->unusedFlushVIOData);
    if (result != VDO_SUCCESS) {
      freeRecoveryJournal(&journal);
      return result;
    }

    result = createVIO(layer, VIO_TYPE_RECOVERY_JOURNAL, VIO_PRIORITY_HIGH,
                       journal, journal->unusedFlushVIOData,
                       &journal->flushVIO);
    if (result != VDO_SUCCESS) {
      freeRecoveryJournal(&journal);
      return result;
    }

    setActiveBlock(journal);
    journal->flushVIO->completion.callbackThreadID = journal->threadID;
  }

  *journalPtr = journal;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeRecoveryJournal(RecoveryJournal **journalPtr)
{
  RecoveryJournal *journal = *journalPtr;
  if (journal == NULL) {
    return;
  }

  freeLockCounter(&journal->lockCounter);
  freeVIO(&journal->flushVIO);
  FREE(journal->unusedFlushVIOData);

  ASSERT_LOG_ONLY(isRingEmpty(&journal->activeTailBlocks),
                  "journal being freed has no active tail blocks");
  RecoveryJournalBlock *block;
  while ((block = popFreeList(journal)) != NULL) {
    freeRecoveryBlock(&block);
  }

  FREE(journal);
  *journalPtr = NULL;
}

/**********************************************************************/
void setRecoveryJournalPartition(RecoveryJournal *journal,
                                 Partition       *partition)
{
  journal->partition = partition;
}

/**********************************************************************/
void initializeRecoveryJournalPostRecovery(RecoveryJournal *journal,
                                           uint64_t         recoveryCount,
                                           SequenceNumber   tail)
{
  journal->tail          = tail + 1;
  journal->recoveryCount = computeRecoveryCountByte(recoveryCount);
  initializeRecoveryBlock(journal->activeBlock);
  initializeJournalState(journal);
}

/**********************************************************************/
void initializeRecoveryJournalPostRebuild(RecoveryJournal *journal,
                                          uint64_t         recoveryCount,
                                          SequenceNumber   tail,
                                          BlockCount       logicalBlocksUsed,
                                          BlockCount       blockMapDataBlocks)
{
  initializeRecoveryJournalPostRecovery(journal, recoveryCount, tail);
  journal->logicalBlocksUsed  = logicalBlocksUsed;
  journal->blockMapDataBlocks = blockMapDataBlocks;
}

/**********************************************************************/
BlockCount getJournalBlockMapDataBlocksUsed(RecoveryJournal *journal)
{
  return journal->blockMapDataBlocks;
}

/**********************************************************************/
void setJournalBlockMapDataBlocksUsed(RecoveryJournal *journal,
                                      BlockCount       pages)
{
  journal->blockMapDataBlocks = pages;
}

/**********************************************************************/
void openRecoveryJournal(RecoveryJournal *journal,
                         SlabDepot       *depot,
                         BlockMap        *blockMap)
{
  journal->depot    = depot;
  journal->blockMap = blockMap;
}

/**********************************************************************/
size_t getRecoveryJournalEncodedSize(void)
{
  return ENCODED_HEADER_SIZE + sizeof(RecoveryJournalState7_0);
}

/**********************************************************************/
int encodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  SequenceNumber journalStart;
  if (journal->closeRequested) {
    // If the journal is closed, we should start one past the active block
    // (since the active block is not guaranteed to be empty).
    journalStart = journal->tail;
  } else {
    // When we're not closing, we must record the first block that might have
    // entries that need to be applied.
    journalStart = getRecoveryJournalHead(journal);
  }

  int result = encodeHeader(&RECOVERY_JOURNAL_HEADER_7_0, buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = putUInt64LEIntoBuffer(buffer, journalStart);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, journal->logicalBlocksUsed);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, journal->blockMapDataBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == encodedSize,
                "encoded recovery journal component size"
                " must match header size");
}

/**
 * Decode recovery journal component state version 7.0 from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decodeRecoveryJournalState_7_0(Buffer                  *buffer,
                                          RecoveryJournalState7_0 *state)
{
  size_t initialLength = contentLength(buffer);

  int result = getUInt64LEFromBuffer(buffer, &state->journalStart);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->logicalBlocksUsed);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->blockMapDataBlocks);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(RECOVERY_JOURNAL_HEADER_7_0.size == decodedSize,
                "decoded slab depot component size must match header size");
}

/**********************************************************************/
int decodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(&RECOVERY_JOURNAL_HEADER_7_0, &header,
                          true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  RecoveryJournalState7_0 state;
  result = decodeRecoveryJournalState_7_0(buffer, &state);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Update recovery journal in-memory information.
  journal->tail               = state.journalStart;
  journal->logicalBlocksUsed  = state.logicalBlocksUsed;
  journal->blockMapDataBlocks = state.blockMapDataBlocks;
  initializeJournalState(journal);

  if (journal->completion.layer->createMetadataVIO != NULL) {
    // Only restore the activeBlock pointer in normal operation since the
    // formatter doesn't need it.
    initializeRecoveryBlock(journal->activeBlock);
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int decodeSodiumRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  // Sodium uses version 7.0, same as head, currently.
  return decodeRecoveryJournal(journal, buffer);
}

/**
 * Advance the tail of the journal.
 *
 * @param journal  The journal whose tail should be advanced
 **/
static void advanceTail(RecoveryJournal *journal)
{
  advanceBlockMapEra(journal->blockMap, journal->tail + 1);
  setActiveBlock(journal);
  journal->tail++;
  // VDO does not support sequence numbers above 1 << 48 in the slab journal.
  if (journal->tail >= (1ULL << 48)) {
    enterJournalReadOnlyMode(journal, VDO_JOURNAL_OVERFLOW);
  }
}

/**
 * Check whether there is space to make a given type of entry.
 *
 * @param journal    The journal to check
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to make an
 *         entry of the specified type
 **/
static bool checkForEntrySpace(RecoveryJournal *journal, bool increment)
{
  if (increment) {
    return ((journal->availableSpace - journal->pendingDecrementCount) > 1);
  }

  return (journal->availableSpace > 0);
}

/**
 * Prepare the currently active block to receive an entry and check whether
 * an entry of the given type may be assigned at this time.
 *
 * @param block      The current active block of the journal
 * @param increment  Set to <code>true</code> if the desired entry is an
 *                   increment
 *
 * @return <code>true</code> if there is space in the journal to store an
 *         entry of the specified type
 **/
static bool prepareToAssignEntry(RecoveryJournalBlock *block, bool increment)
{
  if (block == NULL) {
    return false;
  }

  RecoveryJournal *journal = block->journal;
  if (!isRecoveryBlockEmpty(block)) {
    return checkForEntrySpace(journal, increment);
  }

  if ((journal->tail - getRecoveryJournalHead(journal)) > journal->size) {
    // Cannot use this block since the journal is full.
    journal->events.diskFull++;
    return false;
  }

  if (!checkForEntrySpace(journal, increment)) {
    return false;
  }

  /*
   * Don't allow the new block to be reaped until all of its entries have been
   * committed to the block map and until the journal block has been fully
   * committed as well. Because the block map update is done only after any
   * slab journal entries have been made, the per-entry lock for the block map
   * entry serves to protect those as well.
   */
  initializeLockCount(journal->lockCounter, block->blockNumber,
                      journal->entriesPerBlock + 1);
  return true;
}

/**********************************************************************/
static void writeBlock(RecoveryJournal *journal, RecoveryJournalBlock *block);

/**
 * Release a reference to a journal block.
 *
 * @param block  The journal block from which to release a reference
 **/
static void releaseJournalBlockReference(RecoveryJournalBlock *block)
{
  releaseJournalZoneReference(block->journal->lockCounter, block->blockNumber);
}

/**
 * Implements WaiterCallback. Assign an entry waiter to the active block.
 **/
static void assignEntry(Waiter *waiter, void *context)
{
  DataVIO              *dataVIO = waiterAsDataVIO(waiter);
  RecoveryJournalBlock *block   = (RecoveryJournalBlock *) context;
  RecoveryJournal      *journal = block->journal;

  if (journal->tail == getRecoveryJournalHead(journal)) {
    // We are putting the first entry into a new journal.
    journal->tail++;
  }

  // Record the point at which we will make the journal entry.
  dataVIO->recoveryJournalPoint = (JournalPoint) {
    .sequenceNumber = block->sequenceNumber,
    .entryCount     = block->entryCount,
  };

  switch (dataVIO->operation.type) {
  case DATA_INCREMENT:
    if (dataVIO->operation.state != MAPPING_STATE_UNMAPPED) {
      journal->logicalBlocksUsed++;
    }
    journal->pendingDecrementCount++;
    break;

  case DATA_DECREMENT:
    if (dataVIO->operation.state != MAPPING_STATE_UNMAPPED) {
      journal->logicalBlocksUsed--;
    }

    // Per-entry locks need not be held for decrement entries since the lock
    // held for the incref entry will protect this entry as well.
    releaseJournalBlockReference(block);
    ASSERT_LOG_ONLY((journal->pendingDecrementCount != 0),
                    "decrement follows increment");
    journal->pendingDecrementCount--;
    break;

  case BLOCK_MAP_INCREMENT:
    journal->blockMapDataBlocks++;
    break;

  default:
    logError("Invalid journal operation %u", dataVIO->operation.type);
    enterJournalReadOnlyMode(journal, VDO_NOT_IMPLEMENTED);
    continueDataVIO(dataVIO, VDO_NOT_IMPLEMENTED);
    return;
  }

  journal->availableSpace--;
  int result = enqueueRecoveryBlockEntry(block, dataVIO);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    continueDataVIO(dataVIO, result);
  }

  if (isRecoveryBlockFull(block)) {
    advanceTail(journal);

    // Only attempt to write the block once we've filled it. Commits of
    // partially filled journal blocks are handled outside the append loop.
    writeBlock(journal, block);
  }

  // Force out slab journal tail blocks when threshold is reached.
  checkSlabJournalCommitThreshold(journal);
}

/**********************************************************************/
static bool assignEntriesFromQueue(RecoveryJournal *journal,
                                   WaitQueue       *queue,
                                   bool             increment)
{
  while (hasWaiters(queue)) {
    // There must always be room to make a decrement entry.
    RecoveryJournalBlock *block = journal->activeBlock;
    if (!prepareToAssignEntry(block, increment)) {
      if (!increment) {
        logError("No space for decrement entry in recovery journal");
        enterJournalReadOnlyMode(journal, VDO_RECOVERY_JOURNAL_FULL);
      }
      return false;
    }

    notifyNextWaiter(queue, assignEntry, block);
  }

  return true;
}

/**********************************************************************/
static void assignEntries(RecoveryJournal *journal)
{
  if (journal->addingEntries) {
    // Protect against re-entrancy.
    return;
  }

  journal->addingEntries = true;
  if (assignEntriesFromQueue(journal, &journal->decrementWaiters, false)) {
    assignEntriesFromQueue(journal, &journal->incrementWaiters, true);
  }

  // We might not have committed a partial block because there were still
  // incoming entries, but they might have been increments with no room.
  writeBlock(journal, journal->activeBlock);
  journal->addingEntries = false;
}

/**
 * Prepare an in-memory journal block to be reused now that it has been fully
 * committed.
 *
 * @param block  The block to be recycled
 **/
static void recycleJournalBlock(RecoveryJournalBlock *block)
{
  RecoveryJournal *journal = block->journal;
  pushRingNode(&journal->freeTailBlocks, &block->ringNode);

  // Release our own lock against reaping now that the block is completely
  // committed, or we're giving up because we're in read-only mode.
  if (block->entryCount > 0) {
    releaseJournalBlockReference(block);
  }

  /*
   * We may have just completed the last outstanding block write, which
   * forces us to finally write out uncommitted entries in a partially full
   * tail block so those VIOs don't wait forever.
   */
  writeBlock(journal, journal->activeBlock);
}

/**
 * WaiterCallback implementation invoked whenever a VIO is to be released
 * from the journal because its entry was committed to disk.
 **/
static void continueCommittedWaiter(Waiter *waiter, void *context)
{
  DataVIO         *dataVIO = waiterAsDataVIO(waiter);
  RecoveryJournal *journal = (RecoveryJournal *) context;
  ASSERT_LOG_ONLY(beforeJournalPoint(&journal->commitPoint,
                                     &dataVIO->recoveryJournalPoint),
                  "DataVIOs released from recovery journal in order. "
                  "Recovery journal point is (%" PRIu64 ", %" PRIu16 "), "
                  "but commit waiter point is (%" PRIu64 ", %" PRIu16 ")",
                  journal->commitPoint.sequenceNumber,
                  journal->commitPoint.entryCount,
                  dataVIO->recoveryJournalPoint.sequenceNumber,
                  dataVIO->recoveryJournalPoint.entryCount);
  journal->commitPoint = dataVIO->recoveryJournalPoint;

  int result
    = (isReadOnly(journal->readOnlyContext) ? VDO_READ_ONLY : VDO_SUCCESS);
  continueWaiter(waiter, &result);
}

/**
 * Notify any VIOs whose entries have now committed, and recycle any
 * journal blocks which have been fully committed.
 *
 * @param journal  The recovery journal to update
 **/
static void notifyCommitWaiters(RecoveryJournal *journal)
{
  RecoveryJournalBlock *lastIterationBlock = NULL;
  while (!isRingEmpty(&journal->activeTailBlocks)) {
    RecoveryJournalBlock *block
      = blockFromRingNode(journal->activeTailBlocks.next);

    int result = ASSERT(block != lastIterationBlock,
                        "Journal notification has entered an infinite loop");
    if (result != VDO_SUCCESS) {
      enterJournalReadOnlyMode(journal, result);
      return;
    }
    lastIterationBlock = block;

    if (block->committing) {
      return;
    }

    notifyAllWaiters(&block->commitWaiters, continueCommittedWaiter, journal);
    if (isReadOnly(journal->readOnlyContext)) {
      notifyAllWaiters(&block->entryWaiters, continueCommittedWaiter, journal);
    } else if (isRecoveryBlockDirty(block) || !isRecoveryBlockFull(block)) {
      // Don't recycle partially-committed or partially-filled blocks.
      return;
    }
    recycleJournalBlock(block);
  }
}

/**
 * Handle post-commit processing. This is the callback registered by
 * writeBlock(). If more entries accumulated in the block being committed while
 * the commit was in progress, another commit will be initiated.
 *
 * @param completion  The completion of the VIO writing this block
 **/
static void completeWrite(VDOCompletion *completion)
{
  RecoveryJournalBlock *block   = completion->parent;
  RecoveryJournal      *journal = block->journal;
  assertOnJournalThread(journal, __func__);

  journal->pendingWriteCount        -= 1;
  journal->events.blocks.committed  += 1;
  journal->events.entries.committed += block->entriesInCommit;
  block->uncommittedEntryCount      -= block->entriesInCommit;
  block->entriesInCommit             = 0;
  block->committing                  = false;

  // If this block is the latest block to be acknowledged, record that fact.
  if (block->sequenceNumber > journal->lastWriteAcknowledged) {
    journal->lastWriteAcknowledged = block->sequenceNumber;
  }

  RecoveryJournalBlock *lastActiveBlock
    = blockFromRingNode(journal->activeTailBlocks.next);
  ASSERT_LOG_ONLY((block->sequenceNumber >= lastActiveBlock->sequenceNumber),
                  "completed journal write is still active");

  notifyCommitWaiters(journal);
  if (checkForClosure(journal)) {
    return;
  }

  // Write out the entries (if any) which accumulated while we were writing.
  if (hasWaiters(&block->entryWaiters)) {
    writeBlock(journal, block);
  }
}

/**********************************************************************/
static void handleWriteError(VDOCompletion *completion)
{
  RecoveryJournalBlock *block   = completion->parent;
  RecoveryJournal      *journal = block->journal;
  logErrorWithStringError(completion->result,
                          "cannot write recovery journal block %" PRIu64,
                          block->sequenceNumber);
  enterJournalReadOnlyMode(journal, completion->result);
  completeWrite(completion);
}

/**
 * Attempt to commit a block. If the block is not the oldest block
 * with uncommitted entries or if it is already being committed,
 * nothing will be done.
 *
 * @param journal  The recovery journal
 * @param block    The block to write
 **/
static void writeBlock(RecoveryJournal *journal, RecoveryJournalBlock *block)
{
  assertOnJournalThread(journal, __func__);
  int result = commitRecoveryBlock(block, completeWrite, handleWriteError);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }
}

/**********************************************************************/
void addRecoveryJournalEntry(RecoveryJournal *journal, DataVIO *dataVIO)
{
  assertOnJournalThread(journal, __func__);
  if (journal->closeRequested) {
    continueDataVIO(dataVIO, VDO_SHUTTING_DOWN);
    return;
  }

  if (isReadOnly(journal->readOnlyContext)) {
    continueDataVIO(dataVIO, VDO_READ_ONLY);
    return;
  }

  bool increment = isIncrementOperation(dataVIO->operation.type);
  ASSERT_LOG_ONLY((!increment || (dataVIO->recoverySequenceNumber == 0)),
                  "journal lock not held for increment");

  advanceJournalPoint(&journal->appendPoint, journal->entriesPerBlock);
  int result = enqueueDataVIO((increment
                               ? &journal->incrementWaiters
                               : &journal->decrementWaiters), dataVIO,
                              THIS_LOCATION("$F($j-$js);io=journal($j-$js)"));
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    continueDataVIO(dataVIO, result);
    return;
  }

  assignEntries(journal);
}

/**
 * Conduct a sweep on a recovery journal to reclaim unreferenced blocks.
 *
 * @param journal  The recovery journal
 **/
static void reapRecoveryJournal(RecoveryJournal *journal)
{
  if (journal->reaping) {
    // We already have an outstanding reap in progress. We need to wait for it
    // to finish.
    return;
  }

  // Start reclaiming blocks only when the journal head has no references. Then
  // stop when a block is referenced.
  while ((journal->blockMapReapHead < journal->lastWriteAcknowledged)
         && !isLocked(journal->lockCounter, journal->blockMapHeadBlockNumber,
                      ZONE_TYPE_LOGICAL)) {
    journal->blockMapReapHead++;
    if (++journal->blockMapHeadBlockNumber == journal->size) {
      journal->blockMapHeadBlockNumber = 0;
    }
  }

  while ((journal->slabJournalReapHead < journal->lastWriteAcknowledged)
         && !isLocked(journal->lockCounter,
                      journal->slabJournalHeadBlockNumber,
                      ZONE_TYPE_PHYSICAL)) {
    journal->slabJournalReapHead++;
    if (++journal->slabJournalHeadBlockNumber == journal->size) {
      journal->slabJournalHeadBlockNumber = 0;
    }
  }

  if ((journal->blockMapReapHead == journal->blockMapHead)
      && (journal->slabJournalReapHead == journal->slabJournalHead)) {
    // Nothing happened.
    return;
  }

  PhysicalLayer *layer = vioAsCompletion(journal->flushVIO)->layer;
  if (layer->isFlushRequired(layer)) {
    /*
     * If the block map head will advance, we must flush any block map page
     * modified by the entries we are reaping. If the slab journal head will
     * advance, we must flush the slab summary update covering the slab journal
     * that just released some lock.
     */
    journal->reaping = true;
    launchFlush(journal->flushVIO, completeReaping, handleFlushError);
    return;
  }

  finishReaping(journal);
}

/**********************************************************************/
void acquireRecoveryJournalBlockReference(RecoveryJournal *journal,
                                          SequenceNumber   sequenceNumber,
                                          ZoneType         zoneType,
                                          ZoneCount        zoneID)
{
  if (sequenceNumber == 0) {
    return;
  }

  BlockCount blockNumber
    = getRecoveryJournalBlockNumber(journal, sequenceNumber);
  acquireLockCountReference(journal->lockCounter, blockNumber, zoneType,
                            zoneID);
}

/**********************************************************************/
void releaseRecoveryJournalBlockReference(RecoveryJournal *journal,
                                          SequenceNumber   sequenceNumber,
                                          ZoneType         zoneType,
                                          ZoneCount        zoneID)
{
  if (sequenceNumber == 0) {
    return;
  }

  BlockCount blockNumber
    = getRecoveryJournalBlockNumber(journal, sequenceNumber);
  releaseLockCountReference(journal->lockCounter, blockNumber, zoneType,
                            zoneID);
}

/**********************************************************************/
void releasePerEntryLockFromOtherZone(RecoveryJournal *journal,
                                      SequenceNumber   sequenceNumber)
{
  if (sequenceNumber == 0) {
    return;
  }

  BlockCount blockNumber
    = getRecoveryJournalBlockNumber(journal, sequenceNumber);
  releaseJournalZoneReferenceFromOtherZone(journal->lockCounter, blockNumber);
}

/**********************************************************************/
void closeRecoveryJournal(RecoveryJournal *journal, VDOCompletion *parent)
{
  // XXX: bad things will happen if this function is called twice on the same
  //      journal.
  assertOnJournalThread(journal, __func__);
  prepareToFinishParent(&journal->completion, parent);
  journal->closeRequested = true;
  checkForClosure(journal);
}

/**********************************************************************/
BlockCount getJournalLogicalBlocksUsed(const RecoveryJournal *journal)
{
  return journal->logicalBlocksUsed;
}

/**********************************************************************/
RecoveryJournalStatistics
getRecoveryJournalStatistics(const RecoveryJournal *journal)
{
  return journal->events;
}

/**********************************************************************/
void dumpRecoveryJournalStatistics(const RecoveryJournal *journal)
{
  RecoveryJournalStatistics stats = getRecoveryJournalStatistics(journal);
  logInfo("Recovery Journal");
  logInfo("  blockMapHead=%" PRIu64 " slabJournalHead=%" PRIu64
          " lastWriteAcknowledged=%" PRIu64 " tail=%" PRIu64
          " blockMapReapHead=%" PRIu64 " slabJournalReapHead=%" PRIu64
          " diskFull=%" PRIu64 " slabJournalCommitsRequested=%" PRIu64
          " incrementWaiters=%zu decrementWaiters=%zu",
          journal->blockMapHead, journal->slabJournalHead,
          journal->lastWriteAcknowledged, journal->tail,
          journal->blockMapReapHead, journal->slabJournalReapHead,
          stats.diskFull, stats.slabJournalCommitsRequested,
          countWaiters(&journal->incrementWaiters),
          countWaiters(&journal->decrementWaiters));
  logInfo("  entries: started=%" PRIu64 " written=%" PRIu64 " committed=%"
          PRIu64,
          stats.entries.started, stats.entries.written,
          stats.entries.committed);
  logInfo("  blocks: started=%" PRIu64 " written=%" PRIu64 " committed=%"
          PRIu64,
          stats.blocks.started, stats.blocks.written,
          stats.blocks.committed);

  logInfo("  active blocks:");
  const RingNode *head = &journal->activeTailBlocks;
  for (RingNode *node = head->next; node != head; node = node->next) {
    dumpRecoveryBlock(blockFromRingNode(node));
  }
}
