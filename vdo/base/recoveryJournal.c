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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/recoveryJournal.c#4 $
 */

#include "recoveryJournal.h"
#include "recoveryJournalInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "constants.h"
#include "dataVIO.h"
#include "extent.h"
#include "header.h"
#include "numUtils.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "vdoInternal.h"

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

static const Header *CURRENT_RECOVERY_JOURNAL_HEADER
  = &RECOVERY_JOURNAL_HEADER_7_0;

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
 * Return the block associated with a ring node.
 *
 * @param node The ring node to recast as a block
 *
 * @return The block
 **/
static RecoveryJournalBlock *blockFromRingNode(RingNode *node)
{
  STATIC_ASSERT(offsetof(RecoveryJournalBlock, ringNode) == 0);
  return (RecoveryJournalBlock *) node;
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
    notifyAllWaiters(&journal->decrementWaiters, continueWaiter, &notifyContext);
    notifyAllWaiters(&journal->incrementWaiters, continueWaiter, &notifyContext);
  }

  if (!journal->closeRequested || journal->completion.complete
      || journal->reaping || hasBlockWaiters(journal)
      || hasWaiters(&journal->incrementWaiters)
      || hasWaiters(&journal->decrementWaiters)) {
    return false;
  }

  RecoveryJournalBlock *block;
  while ((block = popActiveList(journal)) != NULL) {
    // There can be active blocks with no entries if a journal is created
    // but never written to.
    if (!isReadOnly(journal->readOnlyContext)) {
      ASSERT_LOG_ONLY(((block->entryCount == 0) ||
                       (block->entryCount == block->header->entryCount)),
                      "journal being closed is inactive");
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
 * Free a tail block and null out the reference to it.
 *
 * @param blockPtr  The reference to the tail block to free
 **/
static void freeTailBlock(RecoveryJournalBlock **blockPtr)
{
  RecoveryJournalBlock *block = *blockPtr;
  if (block == NULL) {
    return;
  }

  FREE(block->block);
  freeVIO(&block->vio);
  FREE(block);
  *blockPtr = NULL;
}

/**
 * Construct a journal block
 *
 * @param journal  The journal to which the block will belong
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeJournalBlock(RecoveryJournal *journal)
{
  RecoveryJournalBlock *block;
  int result = ALLOCATE(1, RecoveryJournalBlock, __func__, &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Allocate a full block for the journal block even though not all of the
  // space is used since the VIO needs to write a full disk block.
  result = ALLOCATE(VDO_BLOCK_SIZE, char, "PackedJournalBlock", &block->block);
  if (result != VDO_SUCCESS) {
    freeTailBlock(&block);
    return result;
  }

  result = createVIO(journal->completion.layer, VIO_TYPE_RECOVERY_JOURNAL,
                     VIO_PRIORITY_HIGH, block, block->block, &block->vio);
  if (result != VDO_SUCCESS) {
    freeTailBlock(&block);
    return result;
  }

  block->vio->completion.callbackThreadID = journal->threadID;
  initializeRing(&block->ringNode);
  pushRingNode(&journal->freeTailBlocks, &block->ringNode);
  block->header  = (PackedJournalHeader *) block->block;
  block->journal = journal;
  return VDO_SUCCESS;
}

/**
 * Advance to the next sector of the current block and initialize it.
 *
 * @param block  The block whose sector should be advanced
 **/
static void advanceSector(RecoveryJournalBlock *block)
{
  char *sectorBuffer = (char *) block->sector;
  block->sector = (PackedJournalSector *) (sectorBuffer + VDO_SECTOR_SIZE);
  block->sector->checkByte  = block->header->checkByte;
  block->sector->recoveryCount = block->journal->recoveryCount;
  block->sector->entryCount = 0;
}

/**
 * Initialize the active block.
 *
 * @param journal The journal whose active block is being initialized
 **/
static void initializeActiveBlock(RecoveryJournal *journal)
{
  RecoveryJournalBlock *block = journal->activeBlock;
  memset(block->block, 0x0, VDO_BLOCK_SIZE);
  *(block->header) = (PackedJournalHeader) {
    .entryCount         = 0,
    .sequenceNumber     = journal->tail,
    .nonce              = journal->nonce,
    .metadataType       = VDO_METADATA_RECOVERY_JOURNAL,
    .logicalBlocksUsed  = journal->logicalBlocksUsed,
    .blockMapDataBlocks = journal->blockMapDataBlocks,
    .checkByte          = computeRecoveryCheckByte(journal, journal->tail),
    .recoveryCount      = journal->recoveryCount,
  };

  block->blockNumber = getRecoveryJournalBlockNumber(journal, journal->tail);
  block->entryCount  = 0;
  block->sector      = (PackedJournalSector *) block->block;
  advanceSector(block);
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
  initializeActiveBlock(journal);
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

  // Ensure that a block is large enough to store
  // RECOVERY_JOURNAL_ENTRIES_PER_BLOCK entries.
  STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
                <= ((VDO_BLOCK_SIZE - sizeof(PackedJournalHeader))
                    / sizeof(RecoveryJournalEntry)));
  journal->entriesPerBlock   = RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
  journal->entriesPerSector  = ((VDO_SECTOR_SIZE - sizeof(PackedJournalSector))
                                / sizeof(RecoveryJournalEntry));
  journal->lastSectorEntries = (RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
                                % journal->entriesPerSector);
  if (journal->lastSectorEntries == 0) {
    journal->lastSectorEntries = journal->entriesPerSector;
  }

  BlockCount journalLength = getRecoveryJournalLength(journalSize);
  journal->availableSpace  = journal->entriesPerBlock * journalLength;

  // Only make the tail buffer and VIO in normal operation since the formatter
  // doesn't need them.
  if (layer->createMetadataVIO != NULL) {
    for (BlockCount i = 0; i < tailBufferSize; i++) {
      result = makeJournalBlock(journal);
      if (result != VDO_SUCCESS) {
        freeRecoveryJournal(&journal);
        return result;
      }
    }

    result = makeLockCounter(layer, journal, reapRecoveryJournalCallback,
                             journal->threadID, threadConfig->logicalZoneCount,
                             threadConfig->physicalZoneCount, journal->size,
                             &journal->lockCounter);
    if (result != VDO_SUCCESS) {
      freeRecoveryJournal(&journal);
      return result;
    }

    setActiveBlock(journal);

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
    freeTailBlock(&block);
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
  initializeActiveBlock(journal);
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
int decodeRecoveryJournalEntry(VDO                  *vdo,
                               RecoveryJournalEntry *entry,
                               JournalOperation     *operation,
                               BlockMapSlot         *slot,
                               PhysicalBlockNumber  *pbn)
{
  decodeOperationAndBlockMapSlot(entry, operation, slot);
  PhysicalBlockNumber unpackedPBN = unpackPBN(&entry->blockMapEntry);
  if ((slot->pbn >= vdo->config.physicalBlocks)
      || (slot->slot >= BLOCK_MAP_ENTRIES_PER_PAGE)
      || isInvalid(&entry->blockMapEntry)
      || !isPhysicalDataBlock(vdo->depot, unpackedPBN)) {
    return logErrorWithStringError(VDO_CORRUPT_JOURNAL, "Invalid entry:"
                                   " (%" PRIu64 ", %" PRIu16 ") to %" PRIu64
                                   " (%s) is not within bounds",
                                   slot->pbn, slot->slot, unpackedPBN,
                                   getJournalOperationName(*operation));
  }

  if ((*operation == BLOCK_MAP_INCREMENT)
      && (isCompressed(entry->blockMapEntry.mappingState)
          || (unpackedPBN == ZERO_BLOCK))) {
    return logErrorWithStringError(VDO_CORRUPT_JOURNAL, "Invalid entry:"
                                   " (%" PRIu64 ", %" PRIu16 ") to %" PRIu64
                                   " (%s) is not a valid tree mapping",
                                   slot->pbn, slot->slot, unpackedPBN,
                                   getJournalOperationName(*operation));
  }

  if (pbn != NULL) {
    *pbn = unpackedPBN;
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
int encodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  // When we're not closing, we must record the first block that might have
  // entries that need to be applied.
  RecoveryJournalState7_0 state = {
    .journalStart       = getRecoveryJournalHead(journal),
    .logicalBlocksUsed  = journal->logicalBlocksUsed,
    .blockMapDataBlocks = journal->blockMapDataBlocks,
  };
  if (journal->closeRequested) {
    // If the journal is closed, we should start one past the active block
    // (since the active block is not guaranteed to be empty).
    state.journalStart = journal->tail;
  }

  return encodeWithHeader(CURRENT_RECOVERY_JOURNAL_HEADER, &state, buffer);
}

/**********************************************************************/
int decodeSodiumRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  // Sodium uses version 7.0, same as head, currently.
  return decodeRecoveryJournal(journal, buffer);
}

/**********************************************************************/
int decodeRecoveryJournal(RecoveryJournal *journal, Buffer *buffer)
{
  Header header;
  int result = decodeHeader(buffer, &header);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateHeader(CURRENT_RECOVERY_JOURNAL_HEADER, &header,
                          true, __func__);
  if (result != VDO_SUCCESS) {
    return result;
  }

  RecoveryJournalState7_0 state;
  result = getBytesFromBuffer(buffer, sizeof(RecoveryJournalState7_0), &state);
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
    initializeActiveBlock(journal);
  }

  return VDO_SUCCESS;
}

/**
 * Check whether a journal block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the the block is full
 **/
__attribute__((warn_unused_result))
static inline bool isBlockFull(RecoveryJournalBlock *block)
{
  return (block->journal->entriesPerBlock == block->entryCount);
}

/**
 * Check whether the current sector of a journal block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the sector is full
 **/
__attribute__((warn_unused_result))
static inline bool isSectorFull(RecoveryJournalBlock *block)
{
  return (block->journal->entriesPerSector == block->sector->entryCount);
}

/**
 * Check whether a journal block is empty.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the block has no entries
 **/
__attribute__((warn_unused_result))
static inline bool isEmpty(RecoveryJournalBlock *block)
{
  return (block->entryCount == 0);
}

/**
 * Check whether all the entries in a journal block have been committed.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the the block is complete
 **/
__attribute__((warn_unused_result))
static bool isBlockComplete(const RecoveryJournalBlock *block)
{
  return (!block->committing
          && (block->journal->entriesPerBlock == block->header->entryCount));
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
  if (!isEmpty(block)) {
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
  BlockCount blockNumber
    = getRecoveryJournalBlockNumber(journal, block->header->sequenceNumber);
  initializeLockCount(journal->lockCounter, blockNumber,
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
  RecoveryJournal *journal = block->journal;
  BlockCount blockNumber
    = getRecoveryJournalBlockNumber(journal, block->header->sequenceNumber);
  releaseJournalZoneReference(journal->lockCounter, blockNumber);
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

  if (block->entryCount == block->header->entryCount) {
    // Update stats to reflect the journal block we've just opened or the
    // committing block we're extending and will have to write again.
    journal->events.blocks.started++;
  }

  // Record the point at which we will make the journal entry.
  dataVIO->recoveryJournalPoint = (JournalPoint) {
    .sequenceNumber = block->header->sequenceNumber,
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

  // Update stats to reflect the journal entry we're going to write.
  journal->events.entries.started++;
  block->entryCount++;
  journal->availableSpace--;
  if (isBlockFull(block)) {
    advanceTail(journal);
  }

  // Enqueue the DataVIO to wait for its entry to commit.
  int result = enqueueDataVIO(&block->entryWaiters, dataVIO,
                              THIS_LOCATION("$F($j-$js)"));
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    continueDataVIO(dataVIO, result);
  }

  if (isBlockFull(block)) {
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
    } else {
      if (!isBlockComplete(block)) {
        return;
      }
    }
    recycleJournalBlock(block);
  }
}

/**
 * Check whether a journal block should be committed.
 *
 * @param journal  The recovery journal
 * @param block    The journal block in question
 *
 * @return <code>true</code> if the block should be committed now
 **/
static bool shouldCommit(RecoveryJournal *journal, RecoveryJournalBlock *block)
{
  // Never commit in read-only mode, if already committing the block, or
  // if there are no entries to commit.
  if (isReadOnly(journal->readOnlyContext) || block->committing
      || !hasWaiters(&block->entryWaiters)) {
    return false;
  }

  // Always commit filled journal blocks.
  if (isBlockFull(block)) {
    return true;
  }

  /*
   * We want to commit any journal blocks that have VIOs waiting on them, but
   * we'd also like to accumulate entries instead of always writing a journal
   * block immediately after the first entry is added. If there are any
   * pending journal writes, we can safely defer committing this partial
   * journal block until the last pending write completes, using the last
   * write's completion as a flush/wake-up.
   */
  return (journal->pendingWriteCount == 0);
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
  block->committing                  = false;

  // If this block is the latest block to be acknowledged, record that fact.
  if (block->header->sequenceNumber > journal->lastWriteAcknowledged) {
    journal->lastWriteAcknowledged = block->header->sequenceNumber;
  }

  RecoveryJournalBlock *lastActiveBlock
    = blockFromRingNode(journal->activeTailBlocks.next);
  ASSERT_LOG_ONLY((block->header->sequenceNumber
                   >= lastActiveBlock->header->sequenceNumber),
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
                          block->header->sequenceNumber);
  enterJournalReadOnlyMode(journal, completion->result);
  completeWrite(completion);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int getJournalBlockPBN(RecoveryJournalBlock *block,
                              PhysicalBlockNumber  *pbnPtr)
{
  RecoveryJournal *journal = block->journal;
  int result = translateToPBN(journal->partition, block->blockNumber, pbnPtr);
  if (result != VDO_SUCCESS) {
    logErrorWithStringError(result,
                            "Error translating recovery journal block "
                            "number %" PRIu64, block->blockNumber);
  }
  return result;
}

/**
 * Actually add entries from the queue to the given block.
 *
 * @param block  The journal block
 *
 * @return  VDO_SUCESS or an error code
 **/
__attribute__((warn_unused_result))
static int addEntries(RecoveryJournalBlock *block)
{
  while (hasWaiters(&block->entryWaiters)) {
    DataVIO *dataVIO
      = waiterAsDataVIO(dequeueNextWaiter(&block->entryWaiters));
    if (dataVIO->operation.type == DATA_INCREMENT) {
      // In order to not lose committed sectors of this partial write, we must
      // flush before the partial write entries are committed.
      block->hasPartialWriteEntry = (block->hasPartialWriteEntry
                                     || dataVIO->isPartialWrite);
      /*
       * In order to not lose acknowledged writes with the FUA flag set, we
       * must issue a flush to cover the data write and also all previous
       * journal writes, and we must issue a FUA on the journal write.
       */
      block->hasFUAEntry = (block->hasFUAEntry
                            || vioRequiresFlushAfter(dataVIOAsVIO(dataVIO)));
    }

    // Encode the entry.
    RecoveryJournalEntry *newEntry
      = &block->sector->entries[block->sector->entryCount++];
    TreeLock *lock = &dataVIO->treeLock;
    encodeOperationAndBlockMapSlot(newEntry, dataVIO->operation.type,
                                   lock->treeSlots[lock->height].blockMapSlot);
    newEntry->blockMapEntry = packPBN(dataVIO->operation.pbn,
                                      dataVIO->operation.state);

    if (isIncrementOperation(dataVIO->operation.type)) {
      dataVIO->recoverySequenceNumber = block->header->sequenceNumber;
    }

    // Enqueue the DataVIO to wait for its entry to commit.
    int result = enqueueDataVIO(&block->commitWaiters, dataVIO,
                                THIS_LOCATION("$F($j-$js)"));
    if (result != VDO_SUCCESS) {
      continueDataVIO(dataVIO, result);
      return result;
    }

    if (isSectorFull(block)) {
      advanceSector(block);
    }
  }

  return VDO_SUCCESS;
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
  if (!shouldCommit(journal, block)) {
    return;
  }

  PhysicalBlockNumber blockPBN;
  int result = getJournalBlockPBN(block, &blockPBN);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }

  block->entriesInCommit = countWaiters(&block->entryWaiters);
  result = addEntries(block);
  if (result != VDO_SUCCESS) {
    enterJournalReadOnlyMode(journal, result);
    return;
  }

  // Update stats to reflect the block and entries we're about to write.
  journal->pendingWriteCount      += 1;
  journal->events.blocks.written  += 1;
  journal->events.entries.written += block->entriesInCommit;

  block->header->blockMapHead    = journal->blockMapHead;
  block->header->slabJournalHead = journal->slabJournalHead;
  block->header->entryCount      = block->entryCount;
  block->committing              = true;

  /*
   * In sync mode, when we are writing an increment entry for a request with
   * FUA, or when making the increment entry for a partial write, we need to
   * make sure all the data being mapped to by this block is stable on disk
   * and also that the recovery journal is stable up to the current block, so
   * we must flush before writing.
   *
   * In sync mode, and for FUA, we also need to make sure that the write we
   * are doing is stable, so we issue the write with FUA.
   */
  PhysicalLayer *layer        = journal->completion.layer;
  bool           sync         = !layer->isFlushRequired(layer);
  bool           fua          = sync || block->hasFUAEntry;
  bool           flushBefore  = fua || block->hasPartialWriteEntry;
  block->hasFUAEntry          = false;
  block->hasPartialWriteEntry = false;
  launchWriteMetadataVIOWithFlush(block->vio, blockPBN, completeWrite,
                                  handleWriteError, flushBefore, fua);
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

  // If the block map head will advance, we must flush any block map page
  // modified by the entries we are reaping. If the slab journal head will
  // advance, we must flush the slab summary update covering the slab journal
  // that just released some lock.
  journal->reaping = true;
  launchFlush(journal->flushVIO, completeReaping, handleFlushError);
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
    RecoveryJournalBlock *block = blockFromRingNode(node);
    logInfo("    sequence number %" PRIu64 "; committed entry count %" PRIu16
            "; %s; %zu entry waiters; %zu commit waiters",
            block->header->sequenceNumber, block->header->entryCount,
            (block->committing ? "committing" : "waiting"),
            countWaiters(&block->entryWaiters),
            countWaiters(&block->commitWaiters));
  }
}
