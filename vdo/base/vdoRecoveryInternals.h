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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoRecoveryInternals.h#11 $
 */

#ifndef VDO_RECOVERY_INTERNALS_H
#define VDO_RECOVERY_INTERNALS_H

#include "vdoRecovery.h"

#include "blockMapRecovery.h"
#include "intMap.h"
#include "journalPoint.h"
#include "ringNode.h"
#include "types.h"
#include "waitQueue.h"

/**
 * The absolute position of an entry in the recovery journal, including
 * the sector number and the entry number within the sector.
 **/
struct recovery_point {
  SequenceNumber    sequenceNumber; // Block sequence number
  uint8_t           sectorCount;    // Sector number
  JournalEntryCount entryCount;     // Entry number
};

struct recovery_completion {
  /** The completion header */
  struct vdo_completion                  completion;
  /** The sub-task completion */
  struct vdo_completion                  subTaskCompletion;
  /** The struct vdo in question */
  struct vdo                            *vdo;
  /** The struct block_allocator whose journals are being recovered */
  struct block_allocator                *allocator;
  /** A buffer to hold the data read off disk */
  char                                  *journalData;
  /** The number of increfs */
  size_t                                 increfCount;

  /** The entry data for the block map recovery */
  struct numbered_block_mapping         *entries;
  /** The number of entries in the entry array */
  size_t                                 entryCount;
  /** The sequence number of the first valid block for block map recovery */
  SequenceNumber                         blockMapHead;
  /** The sequence number of the first valid block for slab journal replay */
  SequenceNumber                         slabJournalHead;
  /** The sequence number of the last valid block of the journal (if known) */
  SequenceNumber                         tail;
  /**
   * The highest sequence number of the journal, not the same as the tail,
   * since the tail ignores blocks after the first hole.
   */
  SequenceNumber                         highestTail;

  /** A location just beyond the last valid entry of the journal */
  struct recovery_point                  tailRecoveryPoint;
  /** The location of the next recovery journal entry to apply */
  struct recovery_point                  nextRecoveryPoint;
  /** The number of logical blocks currently known to be in use */
  BlockCount                             logicalBlocksUsed;
  /** The number of block map data blocks known to be allocated */
  BlockCount                             blockMapDataBlocks;
  /** The journal point to give to the next synthesized decref */
  struct journal_point                   nextJournalPoint;
  /** The number of entries played into slab journals */
  size_t                                 entriesAddedToSlabJournals;

  // Decref synthesis fields

  /** An intMap for use in finding which slots are missing decrefs */
  struct int_map                        *slotEntryMap;
  /** The number of synthesized decrefs */
  size_t                                 missingDecrefCount;
  /** The number of incomplete decrefs */
  size_t                                 incompleteDecrefCount;
  /** The fake journal point of the next missing decref */
  struct journal_point                   nextSynthesizedJournalPoint;
  /** The queue of missing decrefs */
  struct wait_queue                      missingDecrefs[];
};

/**
 * Convert a generic completion to a recovery_completion.
 *
 * @param completion  The completion to convert
 *
 * @return The recovery_completion
 **/
__attribute__((warn_unused_result))
static inline struct recovery_completion *
asRecoveryCompletion(struct vdo_completion *completion)
{
  assertCompletionType(completion->type, RECOVERY_COMPLETION);
  return container_of(completion, struct recovery_completion, completion);
}

/**
 * Allocate and initialize a recovery_completion.
 *
 * @param vdo          The vdo in question
 * @param recoveryPtr  A pointer to hold the new recovery_completion
 *
 * @return VDO_SUCCESS or a status code
 **/
int makeRecoveryCompletion(struct vdo                  *vdo,
                           struct recovery_completion **recoveryPtr)
  __attribute__((warn_unused_result));

/**
 * Free a recovery_completion and all underlying structures.
 *
 * @param recoveryPtr  A pointer to the recovery completion to free
 **/
void freeRecoveryCompletion(struct recovery_completion **recoveryPtr);

#endif // VDO_RECOVERY_INTERNALS_H
