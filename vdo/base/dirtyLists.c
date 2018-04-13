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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/dirtyLists.c#1 $
 */

#include "dirtyLists.h"
#include "dirtyListsInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "types.h"

struct dirtyLists {
  /** The number of periods after which an element will be expired */
  BlockCount      maximumAge;
  /** The oldest period which has unexpired elements */
  SequenceNumber  oldestPeriod;
  /** One more than the current period */
  SequenceNumber  nextPeriod;
  /** The function to call on expired elements */
  DirtyCallback  *callback;
  /** The callback context */
  void           *context;
  /** The offset in the array of lists of the oldest period */
  BlockCount      offset;
  /** The list of elements which are being expired */
  RingNode        expired;
  /** The lists of dirty elements */
  RingNode        lists[];
};

/**********************************************************************/
int makeDirtyLists(BlockCount      maximumAge,
                   DirtyCallback  *callback,
                   void           *context,
                   DirtyLists    **dirtyListsPtr)
{
  DirtyLists *dirtyLists;
  int result = ALLOCATE_EXTENDED(DirtyLists, maximumAge, RingNode, __func__,
                                 &dirtyLists);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dirtyLists->maximumAge = maximumAge;
  dirtyLists->callback   = callback;
  dirtyLists->context    = context;

  initializeRing(&dirtyLists->expired);
  for (BlockCount i = 0; i < maximumAge; i++) {
    initializeRing(&dirtyLists->lists[i]);
  }

  *dirtyListsPtr = dirtyLists;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeDirtyLists(DirtyLists **dirtyListsPtr)
{
  DirtyLists *lists = *dirtyListsPtr;
  if (lists == NULL) {
    return;
  }

  FREE(lists);
  *dirtyListsPtr = NULL;
}

/**********************************************************************/
void setCurrentPeriod(DirtyLists *dirtyLists, SequenceNumber period)
{
  ASSERT_LOG_ONLY(dirtyLists->nextPeriod == 0, "current period not set");
  dirtyLists->oldestPeriod = period;
  dirtyLists->nextPeriod   = period + 1;
  dirtyLists->offset       = period % dirtyLists->maximumAge;
}

/**
 * Expire the oldest list.
 *
 * @param dirtyLists  The DirtyLists to expire
 **/
static void expireOldestList(DirtyLists *dirtyLists)
{
  dirtyLists->oldestPeriod++;
  RingNode *ring = &(dirtyLists->lists[dirtyLists->offset++]);
  if (!isRingEmpty(ring)) {
    spliceRingChainBefore(ring->next, ring->prev, &dirtyLists->expired);
  }

  if (dirtyLists->offset == dirtyLists->maximumAge) {
    dirtyLists->offset = 0;
  }
}

/**
 * Update the period if necessary.
 *
 * @param dirtyLists  The DirtyLists
 * @param period      The new period
 **/
static void updatePeriod(DirtyLists *dirtyLists, SequenceNumber period)
{
  while (dirtyLists->nextPeriod <= period) {
    if ((dirtyLists->nextPeriod - dirtyLists->oldestPeriod)
        == dirtyLists->maximumAge) {
      expireOldestList(dirtyLists);
    }
    dirtyLists->nextPeriod++;
  }
}

/**
 * Write out the expired list.
 *
 * @param dirtyLists  The dirtyLists
 **/
static void writeExpiredElements(DirtyLists *dirtyLists)
{
  if (isRingEmpty(&dirtyLists->expired)) {
    return;
  }

  dirtyLists->callback(&dirtyLists->expired, dirtyLists->context);
  ASSERT_LOG_ONLY(isRingEmpty(&dirtyLists->expired),
                  "no expired elements remain");
}

/**********************************************************************/
void addToDirtyLists(DirtyLists     *dirtyLists,
                     RingNode       *node,
                     SequenceNumber  oldPeriod,
                     SequenceNumber  newPeriod)
{
  if ((oldPeriod == newPeriod)
      || ((oldPeriod != 0) && (oldPeriod < newPeriod))) {
    return;
  }

  if (newPeriod < dirtyLists->oldestPeriod) {
    pushRingNode(&dirtyLists->expired, node);
  } else {
    updatePeriod(dirtyLists, newPeriod);
    pushRingNode(&dirtyLists->lists[newPeriod % dirtyLists->maximumAge], node);
  }

  writeExpiredElements(dirtyLists);
}

/**********************************************************************/
void advancePeriod(DirtyLists *dirtyLists, SequenceNumber period)
{
  updatePeriod(dirtyLists, period);
  writeExpiredElements(dirtyLists);
}

/**********************************************************************/
void flushDirtyLists(DirtyLists *dirtyLists)
{
  while (dirtyLists->oldestPeriod < dirtyLists->nextPeriod) {
    expireOldestList(dirtyLists);
  }
  writeExpiredElements(dirtyLists);
}

/**********************************************************************/
SequenceNumber getDirtyListsNextPeriod(DirtyLists *dirtyLists)
{
  return dirtyLists->nextPeriod;
}
