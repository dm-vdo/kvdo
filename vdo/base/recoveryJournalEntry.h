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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/recoveryJournalEntry.h#1 $
 */

#ifndef RECOVERY_JOURNAL_ENTRY_H
#define RECOVERY_JOURNAL_ENTRY_H

#include "numeric.h"

#include "blockMapEntry.h"
#include "journalPoint.h"
#include "types.h"

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

#endif // RECOVERY_JOURNAL_ENTRY_H
