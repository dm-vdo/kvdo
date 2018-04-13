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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoRecoveryInternals.h#1 $
 */

#ifndef VDO_RECOVERY_INTERNALS_H
#define VDO_RECOVERY_INTERNALS_H

#include "vdoRecovery.h"

#include "blockMapRecovery.h"
#include "intMap.h"
#include "journalPoint.h"
#include "ringNode.h"
#include "types.h"

/**
 * The absolute position of an entry in the recovery journal, including
 * the sector number and the entry number within the sector.
 **/
typedef struct {
  SequenceNumber    sequenceNumber; // Block sequence number
  uint8_t           sectorCount;    // Sector number
  JournalEntryCount entryCount;     // Entry number
} RecoveryPoint;

typedef struct {
  /** The completion header */
  VDOCompletion         completion;
  /** The sub-task completion */
  VDOCompletion         subTaskCompletion;
  /** The VDO in question */
  VDO                  *vdo;
  /** A buffer to hold the data read off disk */
  char                 *journalData;
  /** The number of increfs */
  size_t                increfCount;

  /** The entry data for the block map recovery */
  NumberedBlockMapping *entries;
  /** The number of entries in the entry array */
  size_t                entryCount;
  /** The sequence number of the first valid block for block map recovery */
  SequenceNumber        blockMapHead;
  /** The sequence number of the first valid block for slab journal replay */
  SequenceNumber        slabJournalHead;
  /** The sequence number of the last valid block of the journal (if known) */
  SequenceNumber        tail;
  /**
   * The highest sequence number of the journal, not the same as the tail,
   * since the tail ignores blocks after the first hole.
   */
  SequenceNumber        highestTail;

  /** A location just beyond the last valid entry of the journal */
  RecoveryPoint         tailRecoveryPoint;
  /** The location of the next recovery journal entry to apply */
  RecoveryPoint         nextRecoveryPoint;
  /** The number of logical blocks currently known to be in use */
  BlockCount            logicalBlocksUsed;
  /** The number of block map data blocks known to be allocated */
  BlockCount            blockMapDataBlocks;
  /** The journal point to give to the next synthesized decref */
  JournalPoint          nextJournalPoint;
  /** The number of entries played into slab journals */
  size_t                entriesAddedToSlabJournals;

  // Decref synthesis fields

  /** An intMap for use in finding which slots are missing decrefs */
  IntMap               *slotEntryMap;
  /** A ring node to hold a list of incomplete MissingDecrefs */
  RingNode              incompleteDecrefs;
  /** A ring node to hold a list of complete MissingDecrefs */
  RingNode              completeDecrefs;
  /** The number of outstanding page requests */
  PageCount             outstanding;
  /** The number of synthesized decrefs */
  size_t                missingDecrefCount;
} RecoveryCompletion;

/**
 * Convert a generic completion to a RecoveryCompletion.
 *
 * @param completion  The completion to convert
 *
 * @return The RecoveryCompletion
 **/
__attribute__((warn_unused_result))
static inline RecoveryCompletion *
asRecoveryCompletion(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(RecoveryCompletion, completion) == 0);
  assertCompletionType(completion->type, RECOVERY_COMPLETION);
  return (RecoveryCompletion *) completion;
}

/**
 * Allocate and initialize a RecoveryCompletion.
 *
 * @param vdo         The VDO in question
 * @param recoveryPtr  A pointer to hold the new RecoveryCompletion
 *
 * @return VDO_SUCCESS or a status code
 **/
int makeRecoveryCompletion(VDO *vdo, RecoveryCompletion **recoveryPtr)
  __attribute__((warn_unused_result));

/**
 * Free a RecoveryCompletion and all underlying structures.
 *
 * @param recoveryPtr  A pointer to the recovery completion to free
 **/
void freeRecoveryCompletion(RecoveryCompletion **recoveryPtr);

/**
 * Add recovery journal entries into slab journals, waiting when necessary.
 * This method is exposed only for testing purposes.
 *
 * @param completion  The sub-task completion
 **/
void addSlabJournalEntries(VDOCompletion *completion);

#endif // VDO_RECOVERY_INTERNALS_H
