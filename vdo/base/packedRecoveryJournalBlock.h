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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/packedRecoveryJournalBlock.h#3 $
 */

#ifndef PACKED_RECOVERY_JOURNAL_BLOCK_H
#define PACKED_RECOVERY_JOURNAL_BLOCK_H

#include "numeric.h"

#include "constants.h"
#include "recoveryJournalEntry.h"
#include "types.h"

typedef struct {
  SequenceNumber    blockMapHead;       // Block map head sequence number
  SequenceNumber    slabJournalHead;    // Slab journal head sequence number
  SequenceNumber    sequenceNumber;     // Sequence number for this block
  Nonce             nonce;              // A given VDO instance's nonce
  BlockCount        logicalBlocksUsed;  // Count of logical blocks in use
  BlockCount        blockMapDataBlocks; // Count of allocated block map pages
  JournalEntryCount entryCount;         // Number of entries written
  uint8_t           checkByte;          // The protection check byte
  uint8_t           recoveryCount;      // The number of recoveries completed
  VDOMetadataType   metadataType;       // Metadata type
} RecoveryBlockHeader;

/**
 * The packed, on-disk representation of a recovery journal block header.
 * All fields are kept in little-endian byte order.
 **/
typedef union __attribute__((packed)) {
  struct __attribute__((packed)) {
    /** Block map head 64-bit sequence number */
    byte    blockMapHead[8];

    /** Slab journal head 64-bit sequence number */
    byte    slabJournalHead[8];

    /** The 64-bit sequence number for this block */
    byte    sequenceNumber[8];

    /** A given VDO instance's 64-bit nonce */
    byte    nonce[8];

    /** 8-bit metadata type (should always be one for the recovery journal) */
    uint8_t metadataType;

    /** 16-bit count of the entries encoded in the block */
    byte    entryCount[2];

    /** 64-bit count of the logical blocks used when this block was opened */
    byte    logicalBlocksUsed[8];

    /** 64-bit count of the block map blocks used when this block was opened */
    byte    blockMapDataBlocks[8];

    /** The protection check byte */
    uint8_t checkByte;

    /** The number of recoveries completed */
    uint8_t recoveryCount;
  } fields;

  // A raw view of the packed encoding.
  uint8_t raw[8 + 8 + 8 + 8 + 1 + 2 + 8 + 8 + 1 + 1];

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // This view is only valid on little-endian machines and is only present for
  // ease of directly examining packed entries in GDB.
  struct __attribute__((packed)) {
    SequenceNumber    blockMapHead;
    SequenceNumber    slabJournalHead;
    SequenceNumber    sequenceNumber;
    Nonce             nonce;
    VDOMetadataType   metadataType;
    JournalEntryCount entryCount;
    BlockCount        logicalBlocksUsed;
    BlockCount        blockMapDataBlocks;
    uint8_t           checkByte;
    uint8_t           recoveryCount;
  } littleEndian;
#endif
} PackedJournalHeader;

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

enum {
  // Allowing more than 311 entries in each block changes the math
  // concerning the amortization of metadata writes and recovery speed.
  RECOVERY_JOURNAL_ENTRIES_PER_BLOCK = 311,
  /** The number of entries in each sector (except the last) when filled */
  RECOVERY_JOURNAL_ENTRIES_PER_SECTOR
    = ((VDO_SECTOR_SIZE - sizeof(PackedJournalSector))
       / sizeof(PackedRecoveryJournalEntry)),
  /** The number of entries in the last sector when a block is full */
  RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR
    = (RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
       % RECOVERY_JOURNAL_ENTRIES_PER_SECTOR),
};

/**
 * Find the recovery journal sector from the block header and sector number.
 *
 * @param header        The header of the recovery journal block
 * @param sectorNumber  The index of the sector (1-based)
 *
 * @return A packed recovery journal sector
 **/
__attribute__((warn_unused_result))
static inline
PackedJournalSector *getJournalBlockSector(PackedJournalHeader *header,
                                           int                  sectorNumber)
{
  char *sectorData = ((char *) header) + (VDO_SECTOR_SIZE * sectorNumber);
  return (PackedJournalSector *) sectorData;
}

/**
 * Generate the packed representation of a recovery block header.
 *
 * @param header  The header containing the values to encode
 * @param packed  The header into which to pack the values
 **/
static inline void packRecoveryBlockHeader(const RecoveryBlockHeader *header,
                                           PackedJournalHeader       *packed)
{
  storeUInt64LE(packed->fields.blockMapHead,       header->blockMapHead);
  storeUInt64LE(packed->fields.slabJournalHead,    header->slabJournalHead);
  storeUInt64LE(packed->fields.sequenceNumber,     header->sequenceNumber);
  storeUInt64LE(packed->fields.nonce,              header->nonce);
  storeUInt64LE(packed->fields.logicalBlocksUsed,  header->logicalBlocksUsed);
  storeUInt64LE(packed->fields.blockMapDataBlocks, header->blockMapDataBlocks);
  storeUInt16LE(packed->fields.entryCount,         header->entryCount);

  packed->fields.checkByte = header->checkByte;
  packed->fields.recoveryCount = header->recoveryCount;
  packed->fields.metadataType = header->metadataType;
}

/**
 * Decode the packed representation of a recovery block header.
 *
 * @param packed  The packed header to decode
 * @param header  The header into which to unpack the values
 **/
static inline void unpackRecoveryBlockHeader(const PackedJournalHeader *packed,
                                             RecoveryBlockHeader       *header)
{
  *header = (RecoveryBlockHeader) {
    .blockMapHead       = getUInt64LE(packed->fields.blockMapHead),
    .slabJournalHead    = getUInt64LE(packed->fields.slabJournalHead),
    .sequenceNumber     = getUInt64LE(packed->fields.sequenceNumber),
    .nonce              = getUInt64LE(packed->fields.nonce),
    .logicalBlocksUsed  = getUInt64LE(packed->fields.logicalBlocksUsed),
    .blockMapDataBlocks = getUInt64LE(packed->fields.blockMapDataBlocks),
    .entryCount         = getUInt16LE(packed->fields.entryCount),
    .checkByte          = packed->fields.checkByte,
    .recoveryCount      = packed->fields.recoveryCount,
    .metadataType       = packed->fields.metadataType,
  };
}

#endif // PACKED_RECOVERY_JOURNAL_BLOCK_H
