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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/recoveryUtils.c#9 $
 */

#include "recoveryUtils.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "completion.h"
#include "extent.h"
#include "packedRecoveryJournalBlock.h"
#include "recoveryJournalEntry.h"
#include "recoveryJournalInternals.h"
#include "slabDepot.h"
#include "vdoInternal.h"

/**
 * Finish loading the journal by freeing the extent and notifying the parent.
 * This callback is registered in loadJournalAsync().
 *
 * @param completion  The load extent
 **/
static void finishJournalLoad(struct vdo_completion *completion)
{
  int                    result = completion->result;
  struct vdo_completion *parent = completion->parent;
  struct vdo_extent     *extent = as_vdo_extent(completion);
  free_extent(&extent);
  finishCompletion(parent, result);
}

/**********************************************************************/
void loadJournalAsync(struct recovery_journal  *journal,
                      struct vdo_completion    *parent,
                      char                    **journalDataPtr)
{
  int result = ALLOCATE(journal->size * VDO_BLOCK_SIZE, char, __func__,
                        journalDataPtr);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  struct vdo_extent *extent;
  result = create_extent(parent->layer, VIO_TYPE_RECOVERY_JOURNAL,
                         VIO_PRIORITY_METADATA, journal->size,
                         *journalDataPtr, &extent);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  prepareCompletion(&extent->completion, finishJournalLoad, finishJournalLoad,
                    parent->callbackThreadID, parent);
  read_metadata_extent(extent,
                       get_fixed_layout_partition_offset(journal->partition));
}

/**
 * Determine whether the given header describe a valid block for the
 * given journal that could appear at the given offset in the journal.
 *
 * @param journal  The journal to use
 * @param header   The unpacked block header to check
 * @param offset   An offset indicating where the block was in the journal
 *
 * @return <code>True</code> if the header matches
 **/
__attribute__((warn_unused_result))
static bool
isCongruentRecoveryJournalBlock(struct recovery_journal            *journal,
                                const struct recovery_block_header *header,
                                PhysicalBlockNumber                 offset)
{
  PhysicalBlockNumber expectedOffset
    = getRecoveryJournalBlockNumber(journal, header->sequenceNumber);
  return ((expectedOffset == offset)
          && isValidRecoveryJournalBlock(journal, header));
}

/**********************************************************************/
bool findHeadAndTail(struct recovery_journal *journal,
                     char                    *journalData,
                     SequenceNumber          *tailPtr,
                     SequenceNumber          *blockMapHeadPtr,
                     SequenceNumber          *slabJournalHeadPtr)
{
  SequenceNumber   highestTail        = journal->tail;
  SequenceNumber   blockMapHeadMax    = 0;
  SequenceNumber   slabJournalHeadMax = 0;
  bool             foundEntries       = false;
  for (PhysicalBlockNumber i = 0; i < journal->size; i++) {
    PackedJournalHeader *packedHeader
      = getJournalBlockHeader(journal, journalData, i);
    struct recovery_block_header header;
    unpackRecoveryBlockHeader(packedHeader, &header);

    if (!isCongruentRecoveryJournalBlock(journal, &header, i)) {
      // This block is old, unformatted, or doesn't belong at this location.
      continue;
    }

    if (header.sequenceNumber >= highestTail) {
      foundEntries = true;
      highestTail  = header.sequenceNumber;
    }
    if (header.blockMapHead > blockMapHeadMax) {
      blockMapHeadMax = header.blockMapHead;
    }
    if (header.slabJournalHead > slabJournalHeadMax) {
      slabJournalHeadMax = header.slabJournalHead;
    }
  }

  *tailPtr = highestTail;
  if (!foundEntries) {
    return false;
  }

  *blockMapHeadPtr = blockMapHeadMax;
  if (slabJournalHeadPtr != NULL) {
    *slabJournalHeadPtr = slabJournalHeadMax;
  }
  return true;
}

/**********************************************************************/
int validateRecoveryJournalEntry(const struct vdo                    *vdo,
                                 const struct recovery_journal_entry *entry)
{
  if ((entry->slot.pbn >= vdo->config.physicalBlocks)
      || (entry->slot.slot >= BLOCK_MAP_ENTRIES_PER_PAGE)
      || !isValidLocation(&entry->mapping)
      || !isPhysicalDataBlock(vdo->depot, entry->mapping.pbn)) {
    return logErrorWithStringError(VDO_CORRUPT_JOURNAL, "Invalid entry:"
                                   " (%llu, %" PRIu16 ") to %" PRIu64
                                   " (%s) is not within bounds",
                                   entry->slot.pbn, entry->slot.slot,
                                   entry->mapping.pbn,
                                   getJournalOperationName(entry->operation));
  }

  if ((entry->operation == BLOCK_MAP_INCREMENT)
      && (isCompressed(entry->mapping.state)
          || (entry->mapping.pbn == ZERO_BLOCK))) {
    return logErrorWithStringError(VDO_CORRUPT_JOURNAL, "Invalid entry:"
                                   " (%llu, %" PRIu16 ") to %" PRIu64
                                   " (%s) is not a valid tree mapping",
                                   entry->slot.pbn, entry->slot.slot,
                                   entry->mapping.pbn,
                                   getJournalOperationName(entry->operation));
  }

  return VDO_SUCCESS;
}
