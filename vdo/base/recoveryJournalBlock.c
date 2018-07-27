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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournalBlock.c#7 $
 */

#include "recoveryJournalBlock.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "dataVIO.h"
#include "fixedLayout.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
#include "ringNode.h"
#include "vio.h"
#include "waitQueue.h"

/**********************************************************************/
int makeRecoveryBlock(RecoveryJournal       *journal,
                      RecoveryJournalBlock **blockPtr)
{
  // Ensure that a block is large enough to store
  // RECOVERY_JOURNAL_ENTRIES_PER_BLOCK entries.
  STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
                <= ((VDO_BLOCK_SIZE - sizeof(PackedJournalHeader))
                    / sizeof(PackedRecoveryJournalEntry)));

  RecoveryJournalBlock *block;
  int result = ALLOCATE(1, RecoveryJournalBlock, __func__, &block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Allocate a full block for the journal block even though not all of the
  // space is used since the VIO needs to write a full disk block.
  result = ALLOCATE(VDO_BLOCK_SIZE, char, "PackedJournalBlock", &block->block);
  if (result != VDO_SUCCESS) {
    freeRecoveryBlock(&block);
    return result;
  }

  result = createVIO(journal->completion.layer, VIO_TYPE_RECOVERY_JOURNAL,
                     VIO_PRIORITY_HIGH, block, block->block, &block->vio);
  if (result != VDO_SUCCESS) {
    freeRecoveryBlock(&block);
    return result;
  }

  block->vio->completion.callbackThreadID = journal->threadID;
  initializeRing(&block->ringNode);
  block->journal = journal;

  *blockPtr = block;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeRecoveryBlock(RecoveryJournalBlock **blockPtr)
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
 * Get a pointer to the packed journal block header in the block buffer.
 *
 * @param block  The recovery block
 *
 * @return The block's header
 **/
static inline
PackedJournalHeader *getBlockHeader(const RecoveryJournalBlock *block)
{
  return (PackedJournalHeader *) block->block;
}

/**
 * Set the current sector of the current block and initialize it.
 *
 * @param block  The block to update
 * @param sector A pointer to the first byte of the new sector
 **/
static void setActiveSector(RecoveryJournalBlock *block, void *sector)
{
  block->sector                = (PackedJournalSector *) sector;
  block->sector->checkByte     = getBlockHeader(block)->fields.checkByte;
  block->sector->recoveryCount = block->journal->recoveryCount;
  block->sector->entryCount    = 0;
}

/**********************************************************************/
void initializeRecoveryBlock(RecoveryJournalBlock *block)
{
  memset(block->block, 0x0, VDO_BLOCK_SIZE);

  RecoveryJournal *journal     = block->journal;
  block->sequenceNumber        = journal->tail;
  block->entryCount            = 0;
  block->uncommittedEntryCount = 0;

  block->blockNumber = getRecoveryJournalBlockNumber(journal, journal->tail);

  RecoveryBlockHeader unpacked = {
    .metadataType       = VDO_METADATA_RECOVERY_JOURNAL,
    .blockMapDataBlocks = journal->blockMapDataBlocks,
    .logicalBlocksUsed  = journal->logicalBlocksUsed,
    .nonce              = journal->nonce,
    .recoveryCount      = journal->recoveryCount,
    .sequenceNumber     = journal->tail,
    .checkByte          = computeRecoveryCheckByte(journal, journal->tail),
  };
  PackedJournalHeader *header = getBlockHeader(block);
  packRecoveryBlockHeader(&unpacked, header);

  setActiveSector(block, getJournalBlockSector(header, 1));
}

/**********************************************************************/
int enqueueRecoveryBlockEntry(RecoveryJournalBlock *block, DataVIO *dataVIO)
{
  // First queued entry indicates this is a journal block we've just opened
  // or a committing block we're extending and will have to write again.
  bool newBatch = !hasWaiters(&block->entryWaiters);

  // Enqueue the DataVIO to wait for its entry to commit.
  int result = enqueueDataVIO(&block->entryWaiters, dataVIO,
                              THIS_LOCATION("$F($j-$js)"));
  if (result != VDO_SUCCESS) {
    return result;
  }

  block->entryCount++;
  block->uncommittedEntryCount++;

  // Update stats to reflect the journal entry we're going to write.
  if (newBatch) {
    block->journal->events.blocks.started++;
  }
  block->journal->events.entries.started++;

  return VDO_SUCCESS;
}

/**
 * Check whether the current sector of a block is full.
 *
 * @param block  The block to check
 *
 * @return <code>true</code> if the sector is full
 **/
__attribute__((warn_unused_result))
static bool isSectorFull(const RecoveryJournalBlock *block)
{
  return (block->sector->entryCount == RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
}

/**
 * Actually add entries from the queue to the given block.
 *
 * @param block  The journal block
 *
 * @return VDO_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int addQueuedRecoveryEntries(RecoveryJournalBlock *block)
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

    // Compose and encode the entry.
    PackedRecoveryJournalEntry *packedEntry
      = &block->sector->entries[block->sector->entryCount++];
    TreeLock *lock = &dataVIO->treeLock;
    RecoveryJournalEntry newEntry = {
      .mapping   = {
        .pbn     = dataVIO->operation.pbn,
        .state   = dataVIO->operation.state,
      },
      .operation = dataVIO->operation.type,
      .slot      = lock->treeSlots[lock->height].blockMapSlot,
    };
    *packedEntry = packRecoveryJournalEntry(&newEntry);

    if (isIncrementOperation(dataVIO->operation.type)) {
      dataVIO->recoverySequenceNumber = block->sequenceNumber;
    }

    // Enqueue the DataVIO to wait for its entry to commit.
    int result = enqueueDataVIO(&block->commitWaiters, dataVIO,
                                THIS_LOCATION("$F($j-$js)"));
    if (result != VDO_SUCCESS) {
      continueDataVIO(dataVIO, result);
      return result;
    }

    if (isSectorFull(block)) {
      setActiveSector(block, (char *) block->sector + VDO_SECTOR_SIZE);
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int getRecoveryBlockPBN(RecoveryJournalBlock *block,
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
 * Check whether a journal block should be committed.
 *
 * @param block  The journal block in question
 *
 * @return <code>true</code> if the block should be committed now
 **/
static bool shouldCommit(RecoveryJournalBlock *block)
{
  // Never commit in read-only mode, if already committing the block, or
  // if there are no entries to commit.
  if (block->committing || !hasWaiters(&block->entryWaiters)
      || isReadOnly(block->journal->readOnlyContext)) {
    return false;
  }

  // Always commit filled journal blocks.
  if (isRecoveryBlockFull(block)) {
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
  return (block->journal->pendingWriteCount == 0);
}

/**********************************************************************/
int commitRecoveryBlock(RecoveryJournalBlock *block,
                        VDOAction            *callback,
                        VDOAction            *errorHandler)
{
  if (!shouldCommit(block)) {
    return VDO_SUCCESS;
  }

  PhysicalBlockNumber blockPBN;
  int result = getRecoveryBlockPBN(block, &blockPBN);
  if (result != VDO_SUCCESS) {
    return result;
  }

  block->entriesInCommit = countWaiters(&block->entryWaiters);
  result = addQueuedRecoveryEntries(block);
  if (result != VDO_SUCCESS) {
    return result;
  }

  RecoveryJournal     *journal = block->journal;
  PackedJournalHeader *header  = getBlockHeader(block);

  // Update stats to reflect the block and entries we're about to write.
  journal->pendingWriteCount      += 1;
  journal->events.blocks.written  += 1;
  journal->events.entries.written += block->entriesInCommit;

  storeUInt64LE(header->fields.blockMapHead,    journal->blockMapHead);
  storeUInt64LE(header->fields.slabJournalHead, journal->slabJournalHead);
  storeUInt16LE(header->fields.entryCount,      block->entryCount);

  block->committing = true;

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
  launchWriteMetadataVIOWithFlush(block->vio, blockPBN, callback, errorHandler,
                                  flushBefore, fua);

  return VDO_SUCCESS;
}

/**********************************************************************/
void dumpRecoveryBlock(const RecoveryJournalBlock *block)
{
  logInfo("    sequence number %" PRIu64 "; entries %" PRIu16
          "; %s; %zu entry waiters; %zu commit waiters",
          block->sequenceNumber,
          block->entryCount,
          (block->committing ? "committing" : "waiting"),
          countWaiters(&block->entryWaiters),
          countWaiters(&block->commitWaiters));
}
