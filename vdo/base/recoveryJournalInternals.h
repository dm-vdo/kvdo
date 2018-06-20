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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournalInternals.h#4 $
 */

#ifndef RECOVERY_JOURNAL_INTERNALS_H
#define RECOVERY_JOURNAL_INTERNALS_H

#include "numeric.h"

#include "blockMapEntry.h"
#include "blockMappingState.h"
#include "journalPoint.h"
#include "lockCounter.h"
#include "readOnlyModeContext.h"
#include "recoveryJournal.h"
#include "ringNode.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

enum {
  // Allowing more than 311 entries in each block changes the math
  // concerning the amortization of metadata writes and recovery speed.
  RECOVERY_JOURNAL_ENTRIES_PER_BLOCK = 311,
};

/**
 * A recovery journal entry stores two physical locations: a data location
 * that is the value of a single mapping in the block map tree, and the
 * location of the block map page and and slot that is either acquiring or
 * releasing a reference to the data location. The journal entry also stores
 * an operation code that says whether the reference is being acquired (an
 * increment) or released (a decrement), and whether the mapping is for a
 * logical block or for the block map tree itself.
 **/
typedef struct {
  BlockMapSlot     slot;
  DataLocation     mapping;
  JournalOperation operation;
} RecoveryJournalEntry;

/** The packed, on-disk representation of a recovery journal entry. */
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    /**
     * In little-endian bit order:
     * Bits 15..12:  The four highest bits of the 36-bit physical block number
     *               of the block map tree page
     * Bits 11..2:   The 10-bit block map page slot number
     * Bits 1..0:    The 2-bit JournalOperation of the entry
     **/
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned operation     : 2;
    unsigned slotLow       : 6;
    unsigned slotHigh      : 4;
    unsigned pbnHighNibble : 4;
#else
    unsigned slotLow       : 6;
    unsigned operation     : 2;
    unsigned pbnHighNibble : 4;
    unsigned slotHigh      : 4;
#endif

    /**
     * Bits 47..16:  The 32 low-order bits of the block map page PBN,
     *               in little-endian byte order
     **/
    byte pbnLowWord[4];

    /**
     * Bits 87..48:  The five-byte block map entry encoding the location that
     *               was or will be stored in the block map page slot
     **/
    BlockMapEntry blockMapEntry;
  } fields;

  // A raw view of the packed encoding.
  uint8_t raw[11];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining packed entries in GDB.
  struct __attribute__((packed)) {
    unsigned      operation     : 2;
    unsigned      slot          : 10;
    unsigned      pbnHighNibble : 4;
    uint32_t      pbnLowWord;
    BlockMapEntry blockMapEntry;
  } littleEndian;
#endif
} PackedRecoveryJournalEntry;

typedef struct {
  SequenceNumber    blockMapHead;       // Block map head sequence number
  SequenceNumber    slabJournalHead;    // Slab journal head sequence number
  SequenceNumber    sequenceNumber;     // Sequence number for this block
  Nonce             nonce;              // A given VDO instance's nonce
  VDOMetadataType   metadataType;       // Metadata type
  JournalEntryCount entryCount;         // Number of entries written
  BlockCount        logicalBlocksUsed;  // Count of logical blocks in use
  BlockCount        blockMapDataBlocks; // Count of allocated block map pages
  uint8_t           checkByte;          // The protection check byte
  uint8_t           recoveryCount;      // The number of recoveries completed
} __attribute__((packed)) PackedJournalHeader;

typedef struct {
  /** The protection check byte */
  uint8_t checkByte;

  /** The number of recoveries completed */
  uint8_t recoveryCount;

  /** The number of entries in this sector */
  uint8_t entryCount;

  /** Journal entries for this sector */
  PackedRecoveryJournalEntry entries[];
} __attribute__((packed)) PackedJournalSector;

typedef struct {
  /** The doubly linked pointers for the free or active lists */
  RingNode             ringNode;
  /** The journal to which this block belongs */
  RecoveryJournal     *journal;
  /** A pointer to a block-sized buffer holding the packed block data */
  char                *block;
  /** A pointer to the header in the packed block buffer */
  PackedJournalHeader *header;
  /** A pointer to the current sector in the packed block buffer */
  PackedJournalSector *sector;
  /** The VIO for writing this block */
  VIO                 *vio;
  /** The location of this block in the on-disk journal */
  PhysicalBlockNumber  blockNumber;
  /** Whether this block is being committed */
  bool                 committing;
  /** Whether this block has an uncommitted increment for a partial write */
  bool                 hasPartialWriteEntry;
  /** Whether this block has an uncommitted increment for a write with FUA */
  bool                 hasFUAEntry;
  /** The total number of entries in this block */
  JournalEntryCount    entryCount;
  /** The number of new entries in the current commit */
  JournalEntryCount    entriesInCommit;
  /** The queue of VIOs which will make entries for the next commit */
  WaitQueue            entryWaiters;
  /** The queue of VIOs waiting for the current commit */
  WaitQueue            commitWaiters;
} RecoveryJournalBlock;

struct recoveryJournal {
  /** The completion for shutdown */
  VDOCompletion              completion;
  /** The thread ID of the journal zone */
  ThreadID                   threadID;
  /** The slab depot which can hold locks on this journal */
  SlabDepot                 *depot;
  /** The block map which can hold locks on this journal */
  BlockMap                  *blockMap;
  /** The queue of VIOs waiting to make increment entries */
  WaitQueue                  incrementWaiters;
  /** The queue of VIOs waiting to make decrement entries */
  WaitQueue                  decrementWaiters;
  /** The number of free entries in the journal */
  uint64_t                   availableSpace;
  /** The number of decrement entries which need to be made */
  VIOCount                   pendingDecrementCount;
  /**
   * Whether the journal is adding entries from the increment or
   * decrement waiters queues
   **/
  bool                       addingEntries;
  /** The context for tracking read-only mode */
  ReadOnlyModeContext       *readOnlyContext;
  /** Whether a request has been made to close the journal */
  bool                       closeRequested;
  /** Whether a reap is in progress */
  bool                       reaping;
  /** The partition which holds the journal on disk */
  Partition                 *partition;
  /** The oldest active block in the journal on disk for block map rebuild */
  SequenceNumber             blockMapHead;
  /** The oldest active block in the journal on disk for slab journal replay */
  SequenceNumber             slabJournalHead;
  /** The newest block in the journal on disk to which a write has finished */
  SequenceNumber             lastWriteAcknowledged;
  /** The end of the half-open interval of the active journal */
  SequenceNumber             tail;
  /** The point at which the last entry will have been added */
  JournalPoint               appendPoint;
  /** The journal point of the VIO most recently released from the journal */
  JournalPoint               commitPoint;
  /** The nonce of the VDO */
  Nonce                      nonce;
  /** The number of recoveries completed by the VDO */
  uint8_t                    recoveryCount;
  /** The number of entries which fit in a single block */
  JournalEntryCount          entriesPerBlock;
  /** The number of entries which fit in a single sector */
  JournalEntryCount          entriesPerSector;
  /** The number of entries in the last sector when a block is full */
  JournalEntryCount          lastSectorEntries;
  /** Unused in-memory journal blocks */
  RingNode                   freeTailBlocks;
  /** In-memory journal blocks with records */
  RingNode                   activeTailBlocks;
  /** A pointer to the active block (the one we are writing to now) */
  RecoveryJournalBlock      *activeBlock;
  /** The new block map reap head after reaping */
  SequenceNumber             blockMapReapHead;
  /** The head block number for the block map rebuild range */
  BlockCount                 blockMapHeadBlockNumber;
  /** The new slab journal reap head after reaping */
  SequenceNumber             slabJournalReapHead;
  /** The head block number for the slab journal replay range */
  BlockCount                 slabJournalHeadBlockNumber;
  /** The VIO on which we can call flush (less ick, but still ick) */
  VIO                       *flushVIO;
  /** The data block which must live in the VIO in the flush extent */
  char                      *unusedFlushVIOData;
  /** The number of blocks in the on-disk journal */
  BlockCount                 size;
  /** The number of logical blocks that are in-use */
  BlockCount                 logicalBlocksUsed;
  /** The number of block map pages that are allocated */
  BlockCount                 blockMapDataBlocks;
  /** The number of journal blocks written but not yet acknowledged */
  BlockCount                 pendingWriteCount;
  /** The threshold at which slab journal tail blocks will be written out */
  BlockCount                 slabJournalCommitThreshold;
  /** Counters for events in the journal that are reported as statistics */
  RecoveryJournalStatistics  events;
  /** The locks for each on-disk block */
  LockCounter               *lockCounter;
};

/**
 * Return the packed, on-disk representation of a recovery journal entry.
 *
 * @param entry   The journal entry to pack
 *
 * @return  The packed representation of the journal entry
 **/
static inline PackedRecoveryJournalEntry
packRecoveryJournalEntry(const RecoveryJournalEntry *entry)
{
  PackedRecoveryJournalEntry packed = {
    .fields = {
      .operation     = entry->operation,
      .slotLow       = entry->slot.slot & 0x3F,
      .slotHigh      = (entry->slot.slot >> 6) & 0x0F,
      .pbnHighNibble = (entry->slot.pbn >> 32) & 0x0F,
      .blockMapEntry = packPBN(entry->mapping.pbn, entry->mapping.state),
    }
  };
  storeUInt32LE(packed.fields.pbnLowWord, entry->slot.pbn & UINT_MAX);
  return packed;
}

/**
 * Unpack the on-disk representation of a recovery journal entry.
 *
 * @param entry  The recovery journal entry to unpack
 *
 * @return  The unpacked entry
 **/
static inline RecoveryJournalEntry
unpackRecoveryJournalEntry(const PackedRecoveryJournalEntry *entry)
{
  PhysicalBlockNumber low32 = getUInt32LE(entry->fields.pbnLowWord);
  PhysicalBlockNumber high4 = entry->fields.pbnHighNibble;
  return (RecoveryJournalEntry) {
    .operation = entry->fields.operation,
    .slot      = {
      .pbn  = ((high4 << 32) | low32),
      .slot = (entry->fields.slotLow | (entry->fields.slotHigh << 6)),
    },
    .mapping = unpackBlockMapEntry(&entry->fields.blockMapEntry),
  };
}

/**
 * Validate a recovery journal entry.
 *
 * @param vdo     The VDO
 * @param entry   The entry to validate
 *
 * @return VDO_SUCCESS or an error
 **/
int validateRecoveryJournalEntry(const VDO                  *vdo,
                                 const RecoveryJournalEntry *entry)
  __attribute__((warn_unused_result));

/**
 * Get the physical block number for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number of the desired block
 *
 * @return The block number corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline
PhysicalBlockNumber getRecoveryJournalBlockNumber(RecoveryJournal *journal,
                                                  SequenceNumber   sequence)
{
  // Since journal size is a power of two, the block number modulus can just
  // be extracted from the low-order bits of the sequence.
  return (sequence & (journal->size - 1));
}

/**
 * Compute the checkByte for a given sequence number.
 *
 * @param journal   The journal
 * @param sequence  The sequence number
 *
 * @return The check byte corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline uint8_t computeRecoveryCheckByte(RecoveryJournal *journal,
                                               SequenceNumber   sequence)
{
  // The check byte must change with each trip around the journal.
  return (((sequence / journal->size) & 0x7F) | 0x80);
}

#endif // RECOVERY_JOURNAL_INTERNALS_H
