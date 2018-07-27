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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabJournalInternals.h#4 $
 */

#ifndef SLAB_JOURNAL_INTERNALS_H
#define SLAB_JOURNAL_INTERNALS_H

#include "slabJournal.h"

#include "numeric.h"

#include "blockAllocatorInternals.h"
#include "blockMapEntry.h"
#include "journalPoint.h"
#include "slab.h"
#include "slabSummary.h"
#include "statistics.h"
#include "waitQueue.h"

/**
 * Slab journal blocks may have one of two formats, depending upon whether or
 * not any of the entries in the block are block map increments. Since the
 * steady state for a VDO is that all of the necessary block map pages will
 * be allocated, most slab journal blocks will have only data entries. Such
 * blocks can hold more entries, hence the two formats.
 **/

/** A single slab journal entry */
struct slabJournalEntry {
  SlabBlockNumber  sbn;
  JournalOperation operation;
};

/** A single slab journal entry in its on-disk form */
typedef union {
  struct __attribute__((packed)) {
    uint8_t offsetLow8;
    uint8_t offsetMid8;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned offsetHigh7 : 7;
    unsigned increment   : 1;
#else
    unsigned increment   : 1;
    unsigned offsetHigh7 : 7;
#endif
  } fields;

  // A raw view of the packed encoding.
  uint8_t raw[3];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining packed entries in GDB.
  struct __attribute__((packed)) {
    unsigned offset    : 23;
    unsigned increment : 1;
  } littleEndian;
#endif
} __attribute__((packed)) PackedSlabJournalEntry;

/** The unpacked representation of the header of a slab journal block */
typedef struct {
  /** Sequence number for head of journal */
  SequenceNumber     head;
  /** Sequence number for this block */
  SequenceNumber     sequenceNumber;
  /** The nonce for a given VDO instance */
  Nonce              nonce;
  /** Recovery journal point for last entry */
  JournalPoint       recoveryPoint;
  /** Metadata type */
  VDOMetadataType    metadataType;
  /** Whether this block contains block map increments */
  bool               hasBlockMapIncrements;
  /** The number of entries in the block */
  JournalEntryCount  entryCount;
} SlabJournalBlockHeader;

/**
 * The packed, on-disk representation of a slab journal block header.
 * All fields are kept in little-endian byte order.
 **/
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    /** 64-bit sequence number for head of journal */
    byte               head[8];
    /** 64-bit sequence number for this block */
    byte               sequenceNumber[8];
    /** Recovery journal point for last entry, packed into 64 bits */
    PackedJournalPoint recoveryPoint;
    /** The 64-bit nonce for a given VDO instance */
    byte               nonce[8];
    /** 8-bit metadata type (should always be two, for the slab journal) */
    uint8_t            metadataType;
    /** Whether this block contains block map increments */
    bool               hasBlockMapIncrements;
    /** 16-bit count of the entries encoded in the block */
    byte               entryCount[2];
  } fields;

  // A raw view of the packed encoding.
  uint8_t raw[8 + 8 + 8 + 8 + 1 + 1 + 2];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining packed entries in GDB.
  struct __attribute__((packed)) {
    SequenceNumber     head;
    SequenceNumber     sequenceNumber;
    PackedJournalPoint recoveryPoint;
    Nonce              nonce;
    VDOMetadataType    metadataType;
    bool               hasBlockMapIncrements;
    JournalEntryCount  entryCount;
  } littleEndian;
#endif
} PackedSlabJournalBlockHeader;

enum {
  SLAB_JOURNAL_PAYLOAD_SIZE
    = VDO_BLOCK_SIZE - sizeof(PackedSlabJournalBlockHeader),
  SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK = (SLAB_JOURNAL_PAYLOAD_SIZE * 8) / 25,
  SLAB_JOURNAL_ENTRY_TYPES_SIZE = ((SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK - 1)
                                   / 8) + 1,
  SLAB_JOURNAL_ENTRIES_PER_BLOCK = (SLAB_JOURNAL_PAYLOAD_SIZE
                                    / sizeof(PackedSlabJournalEntry)),
};

/** The payload of a slab journal block which has block map increments */
typedef struct {
  /* The entries themselves */
  PackedSlabJournalEntry entries[SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK];
  /* The bit map indicating which entries are block map increments */
  byte                   entryTypes[SLAB_JOURNAL_ENTRY_TYPES_SIZE];
} __attribute__((packed)) FullSlabJournalEntries;

typedef union {
  /* Entries which include block map increments */
  FullSlabJournalEntries fullEntries;
  /* Entries which are only data updates */
  PackedSlabJournalEntry entries[SLAB_JOURNAL_ENTRIES_PER_BLOCK];
  /* Ensure the payload fills to the end of the block */
  byte                   space[SLAB_JOURNAL_PAYLOAD_SIZE];
} __attribute__((packed)) SlabJournalPayload;

typedef struct {
  PackedSlabJournalBlockHeader header;
  SlabJournalPayload           payload;
} __attribute__((packed)) PackedSlabJournalBlock;

typedef enum {
  NOT_FLUSHING,
  FLUSH_REQUESTED,
  FLUSH_INITIATED,
} FlushState;

typedef struct {
  uint16_t       count;
  SequenceNumber recoveryStart;
} JournalLock;

struct slabJournal {
  /** The completion for load, flush, and close */
  VDOCompletion                completion;

  /** A waiter object for getting a VIO pool entry */
  Waiter                       resourceWaiter;
  /** A waiter object for updating the slab summary */
  Waiter                       slabSummaryWaiter;
  /** A waiter object for getting an extent with which to flush */
  Waiter                       flushWaiter;
  /** The queue of VIOs waiting to make an entry */
  WaitQueue                    entryWaiters;
  /** The parent slab reference of this journal */
  Slab                        *slab;

  /** Whether a flush has been requested or initiated */
  FlushState                   flushState;
  /** Whether a request has been made to close the journal */
  bool                         closeRequested;
  /** Whether a tail block commit is pending */
  bool                         waitingToCommit;
  /** Whether a completion is waiting for slab journal space */
  bool                         waitingForSpace;
  /** Whether the journal is updating the slab summary */
  bool                         updatingSlabSummary;
  /** Whether the journal is adding entries from the entryWaiters queue */
  bool                         addingEntries;
  /** Whether a partial write is in progress */
  bool                         partialWriteInProgress;

  /** The oldest block in the journal on disk */
  SequenceNumber               head;
  /** The oldest block in the journal which may not be reaped */
  SequenceNumber               unreapable;
  /** The end of the half-open interval of the active journal */
  SequenceNumber               tail;
  /** The next journal block to be committed */
  SequenceNumber               nextCommit;
  /** The tail sequence number that is written in the slab summary */
  SequenceNumber               summarized;
  /** The tail sequence number that was last summarized in slab summary */
  SequenceNumber               lastSummarized;

  /** The sequence number of the recovery journal lock */
  SequenceNumber               recoveryLock;

  /**
   * The number of entries which fit in a single block. Can't use the constant
   * because unit tests change this number.
   **/
  JournalEntryCount            entriesPerBlock;
  /**
   * The number of full entries which fit in a single block. Can't use the
   * constant because unit tests change this number.
   **/
  JournalEntryCount            fullEntriesPerBlock;

  /** The recovery journal of the VDO (slab journal holds locks on it) */
  RecoveryJournal             *recoveryJournal;

  /** The slab summary to update tail block location */
  SlabSummaryZone             *summary;
  /** The statistics shared by all slab journals in our physical zone */
  AtomicSlabJournalStatistics *events;
  /** A ring of the VIO pool entries for outstanding journal block writes */
  RingNode                     uncommittedBlocks;

  /**
   * The current tail block header state. This will be packed into
   * the block just before it is written.
   **/
  SlabJournalBlockHeader       tailHeader;
  /** A pointer to a block-sized buffer holding the packed block data */
  PackedSlabJournalBlock      *block;

  /** The number of blocks in the on-disk journal */
  BlockCount                   size;
  /** The number of blocks at which to start pushing reference blocks */
  BlockCount                   flushingThreshold;
  /** The number of blocks at which all reference blocks should be writing */
  BlockCount                   flushingDeadline;
  /** The number of blocks at which to wait for reference blocks to write */
  BlockCount                   blockingThreshold;
  /** The number of blocks at which to scrub the slab before coming online */
  BlockCount                   scrubbingThreshold;

  /** This node is for BlockAllocator to keep a queue of dirty journals */
  RingNode                     dirtyNode;

  /** The lock for the oldest unreaped block of the journal */
  JournalLock                 *reapLock;
  /** The locks for each on disk block */
  JournalLock                  locks[];
};

/**
 * Get the slab journal block offset of the given sequence number.
 *
 * @param journal   The slab journal
 * @param sequence  The sequence number
 *
 * @return the offset corresponding to the sequence number
 **/
__attribute__((warn_unused_result))
static inline TailBlockOffset
getSlabJournalBlockOffset(SlabJournal *journal, SequenceNumber sequence)
{
  return (sequence % journal->size);
}

/**
 * Encode a slab journal entry (exposed for unit tests).
 *
 * @param tailHeader  The unpacked header for the block
 * @param payload     The journal block payload to hold the entry
 * @param sbn         The slab block number of the entry to encode
 * @param operation   The type of the entry
 **/
void encodeSlabJournalEntry(SlabJournalBlockHeader *tailHeader,
                            SlabJournalPayload     *payload,
                            SlabBlockNumber         sbn,
                            JournalOperation        operation);

/**
 * Decode a slab journal entry.
 *
 * @param block       The journal block holding the entry
 * @param entryCount  The number of the entry
 *
 * @return The decoded entry
 **/
SlabJournalEntry decodeSlabJournalEntry(PackedSlabJournalBlock *block,
                                        JournalEntryCount       entryCount)
  __attribute__((warn_unused_result));

/**
 * Generate the packed encoding of a slab journal entry.
 *
 * @param packed       The entry into which to pack the values
 * @param sbn          The slab block number of the entry to encode
 * @param isIncrement  The increment flag
 **/
static inline void packSlabJournalEntry(PackedSlabJournalEntry *packed,
                                        SlabBlockNumber         sbn,
                                        bool                    isIncrement)
{
  packed->fields.offsetLow8  = (sbn & 0x0000FF);
  packed->fields.offsetMid8  = (sbn & 0x00FF00) >> 8;
  packed->fields.offsetHigh7 = (sbn & 0x7F0000) >> 16;
  packed->fields.increment   = isIncrement ? 1 : 0;
}

/**
 * Decode the packed representation of a slab journal entry.
 *
 * @param packed  The packed entry to decode
 *
 * @return The decoded slab journal entry
 **/
__attribute__((warn_unused_result))
static inline
SlabJournalEntry unpackSlabJournalEntry(const PackedSlabJournalEntry *packed)
{
  SlabJournalEntry entry;
  entry.sbn = packed->fields.offsetHigh7;
  entry.sbn <<= 8;
  entry.sbn |= packed->fields.offsetMid8;
  entry.sbn <<= 8;
  entry.sbn |= packed->fields.offsetLow8;
  entry.operation
    = (packed->fields.increment ? DATA_INCREMENT : DATA_DECREMENT);
  return entry;
}

/**
 * Generate the packed representation of a slab block header.
 *
 * @param header  The header containing the values to encode
 * @param packed  The header into which to pack the values
 **/
static inline
void packSlabJournalBlockHeader(const SlabJournalBlockHeader *header,
                                PackedSlabJournalBlockHeader *packed)
{
  storeUInt64LE(packed->fields.head,           header->head);
  storeUInt64LE(packed->fields.sequenceNumber, header->sequenceNumber);
  storeUInt64LE(packed->fields.nonce,          header->nonce);
  storeUInt16LE(packed->fields.entryCount,     header->entryCount);

  packed->fields.metadataType          = header->metadataType;
  packed->fields.hasBlockMapIncrements = header->hasBlockMapIncrements;

  packJournalPoint(&header->recoveryPoint, &packed->fields.recoveryPoint);
}

/**
 * Decode the packed representation of a slab block header.
 *
 * @param packed  The packed header to decode
 * @param header  The header into which to unpack the values
 **/
static inline
void unpackSlabJournalBlockHeader(const PackedSlabJournalBlockHeader *packed,
                                  SlabJournalBlockHeader             *header)
{
  *header = (SlabJournalBlockHeader) {
    .head                  = getUInt64LE(packed->fields.head),
    .sequenceNumber        = getUInt64LE(packed->fields.sequenceNumber),
    .nonce                 = getUInt64LE(packed->fields.nonce),
    .entryCount            = getUInt16LE(packed->fields.entryCount),
    .metadataType          = packed->fields.metadataType,
    .hasBlockMapIncrements = packed->fields.hasBlockMapIncrements,
  };
  unpackJournalPoint(&packed->fields.recoveryPoint, &header->recoveryPoint);
}

#endif // SLAB_JOURNAL_INTERNALS_H
