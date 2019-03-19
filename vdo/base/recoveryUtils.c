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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/recoveryUtils.c#1 $
 */

#include "recoveryUtils.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "extent.h"
#include "recoveryJournalInternals.h"

/**
 * Finish loading the journal by freeing the extent and notifying the parent.
 * This callback is registered in loadJournalAsync().
 *
 * @param completion  The load extent
 **/
static void finishJournalLoad(VDOCompletion *completion)
{
  int            result = completion->result;
  VDOCompletion *parent = completion->parent;
  VDOExtent     *extent = asVDOExtent(completion);
  freeExtent(&extent);
  finishCompletion(parent, result);
}

/**********************************************************************/
void loadJournalAsync(RecoveryJournal  *journal,
                      VDOCompletion    *parent,
                      char            **journalDataPtr)
{
  int result = ALLOCATE(journal->size * VDO_BLOCK_SIZE, char, __func__,
                        journalDataPtr);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  VDOExtent *extent;
  result = createExtent(parent->layer, VIO_TYPE_RECOVERY_JOURNAL,
                        VIO_PRIORITY_METADATA, journal->size,
                        *journalDataPtr, &extent);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  prepareCompletion(&extent->completion, finishJournalLoad, finishJournalLoad,
                    parent->callbackThreadID, parent);
  readMetadataExtent(extent,
                     getFixedLayoutPartitionOffset(journal->partition));
}

/**********************************************************************/
bool findHeadAndTail(RecoveryJournal *journal,
                     char            *journalData,
                     SequenceNumber  *tailPtr,
                     SequenceNumber  *blockMapHeadPtr,
                     SequenceNumber  *slabJournalHeadPtr)
{
  SequenceNumber   highestTail        = journal->tail;
  SequenceNumber   blockMapHeadMax    = 0;
  SequenceNumber   slabJournalHeadMax = 0;
  bool             foundEntries       = false;
  for (PhysicalBlockNumber i = 0; i < journal->size; i++) {
    PackedJournalHeader *header
      = getJournalBlockHeader(journal, journalData, i);
    if (!isCongruentRecoveryJournalBlock(journal, header, i)) {
      // This block is old, unformatted, or doesn't belong at this location.
      continue;
    }

    if (header->sequenceNumber >= highestTail) {
      foundEntries = true;
      highestTail  = header->sequenceNumber;
    }
    if (header->blockMapHead > blockMapHeadMax) {
      blockMapHeadMax = header->blockMapHead;
    }
    if (header->slabJournalHead > slabJournalHeadMax) {
      slabJournalHeadMax = header->slabJournalHead;
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
