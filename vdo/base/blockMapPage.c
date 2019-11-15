/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapPage.c#7 $
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

static const struct version_number BLOCK_MAP_4_1 = {
  .majorVersion = 4,
  .minorVersion = 1,
};

/**********************************************************************/
bool isCurrentBlockMapPage(const struct block_map_page *page)
{
  return areSameVersion(BLOCK_MAP_4_1, unpackVersionNumber(page->version));
}

/**********************************************************************/
struct block_map_page *formatBlockMapPage(void                *buffer,
                                          Nonce                nonce,
                                          PhysicalBlockNumber  pbn,
                                          bool                 initialized)
{
  memset(buffer, 0, VDO_BLOCK_SIZE);
  struct block_map_page *page = (struct block_map_page *) buffer;
  page->version = packVersionNumber(BLOCK_MAP_4_1);
  storeUInt64LE(page->header.fields.nonce, nonce);
  storeUInt64LE(page->header.fields.pbn, pbn);
  page->header.fields.initialized = initialized;
  return page;
}

/**********************************************************************/
BlockMapPageValidity validateBlockMapPage(struct block_map_page *page,
                                          Nonce                  nonce,
                                          PhysicalBlockNumber    pbn)
{
  // Make sure the page layout isn't accidentally changed by changing the
  // length of the page header.
  STATIC_ASSERT_SIZEOF(PageHeader, PAGE_HEADER_4_1_SIZE);

  if (!areSameVersion(BLOCK_MAP_4_1, unpackVersionNumber(page->version))
      || !isBlockMapPageInitialized(page)
      || (nonce != getUInt64LE(page->header.fields.nonce))) {
    return BLOCK_MAP_PAGE_INVALID;
  }

  if (pbn != getBlockMapPagePBN(page)) {
    return BLOCK_MAP_PAGE_BAD;
  }

  return BLOCK_MAP_PAGE_VALID;
}

/**********************************************************************/
void updateBlockMapPage(struct block_map_page *page,
                        struct data_vio       *dataVIO,
                        PhysicalBlockNumber    pbn,
                        BlockMappingState      mappingState,
                        SequenceNumber        *recoveryLock)
{
  // Encode the new mapping.
  struct tree_lock *treeLock = &dataVIO->treeLock;
  SlotNumber slot = treeLock->treeSlots[treeLock->height].blockMapSlot.slot;
  page->entries[slot] = packPBN(pbn, mappingState);

  // Adjust references (locks) on the recovery journal blocks.
  struct block_map_zone *zone      = getBlockMapForZone(dataVIO->logical.zone);
  struct block_map      *blockMap  = zone->blockMap;
  RecoveryJournal       *journal   = blockMap->journal;
  SequenceNumber         oldLocked = *recoveryLock;
  SequenceNumber         newLocked = dataVIO->recoverySequenceNumber;

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

    *recoveryLock = newLocked;
  }

  // Release the transferred lock from the data_vio.
  releasePerEntryLockFromOtherZone(journal, newLocked);
  dataVIO->recoverySequenceNumber = 0;
}
