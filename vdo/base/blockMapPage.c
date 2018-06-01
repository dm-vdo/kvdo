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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapPage.c#2 $
 */

#include "blockMapPage.h"

#include "permassert.h"

#include "blockMap.h"
#include "blockMapInternals.h"
#include "blockMapTree.h"
#include "constants.h"
#include "dataVIO.h"
#include "recoveryJournal.h"
#include "statusCodes.h"
#include "types.h"

enum {
  PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const VersionNumber BLOCK_MAP_4_1 = {
  .majorVersion = 4,
  .minorVersion = 1,
};

static const VersionNumber *CURRENT_BLOCK_MAP_VERSION = &BLOCK_MAP_4_1;

/**********************************************************************/
bool isCurrentBlockMapPage(const BlockMapPage *page)
{
  return areSameVersion(CURRENT_BLOCK_MAP_VERSION, &page->version);
}

/**********************************************************************/
void formatBlockMapPage(void *buffer, Nonce nonce, PhysicalBlockNumber pbn)
{
  memset(buffer, 0, VDO_BLOCK_SIZE);
  BlockMapPage *page = (BlockMapPage *) buffer;
  page->version      = *CURRENT_BLOCK_MAP_VERSION;
  page->header       = (PageHeader) {
    .nonce = nonce,
    .pbn   = pbn,
  };
}

/**********************************************************************/
BlockMapPageValidity validateBlockMapPage(BlockMapPage        *page,
                                          Nonce                nonce,
                                          PhysicalBlockNumber  pbn)
{
  // Make sure the page layout isn't accidentally changed by changing the
  // length of the page header.
  STATIC_ASSERT_SIZEOF(PageHeader, PAGE_HEADER_4_1_SIZE);

  if (!areSameVersion(&BLOCK_MAP_4_1, &page->version)
      || !page->header.initialized || (page->header.nonce != nonce)) {
    return BLOCK_MAP_PAGE_INVALID;
  }

  if (page->header.pbn != pbn) {
    return BLOCK_MAP_PAGE_BAD;
  }

  page->header.recoverySequenceNumber  = 0;
  page->header.interiorTreePageWriting = false;
  page->header.generation              = 0;
  return BLOCK_MAP_PAGE_VALID;
}

/**********************************************************************/
void updateBlockMapPage(DataVIO             *dataVIO,
                        BlockMapPage        *page,
                        PhysicalBlockNumber  pbn,
                        BlockMappingState    mappingState)
{
  // Encode the new mapping.
  TreeLock *treeLock = &dataVIO->treeLock;
  SlotNumber slot = treeLock->treeSlots[treeLock->height].blockMapSlot.slot;
  page->entries[slot] = packPBN(pbn, mappingState);

  // Adjust references (locks) on the recovery journal blocks.
  BlockMapZone    *zone      = getBlockMapForZone(dataVIO->logical.zone);
  BlockMap        *blockMap  = zone->blockMap;
  RecoveryJournal *journal   = blockMap->journal;
  SequenceNumber   oldLocked = page->header.recoverySequenceNumber;
  SequenceNumber   newLocked = dataVIO->recoverySequenceNumber;

  if ((oldLocked == 0) || (oldLocked > newLocked)) {
    // Acquire a lock on the newly referenced journal block.
    acquireRecoveryJournalBlockReference(journal, newLocked, ZONE_TYPE_LOGICAL,
                                         zone->zoneNumber);

    // If the block originally held a newer lock, release it.
    if (oldLocked > 0) {
      releaseRecoveryJournalBlockReference(journal, oldLocked,
                                           ZONE_TYPE_LOGICAL,
                                           zone->zoneNumber);
    }

    // Update the lock field and the page status.
    page->header.recoverySequenceNumber = newLocked;
  }

  // Release the transferred lock from the DataVIO.
  releasePerEntryLockFromOtherZone(journal, newLocked);
  dataVIO->recoverySequenceNumber = 0;
}
