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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/dirtyLists.h#1 $
 */

#ifndef DIRTY_LISTS_H
#define DIRTY_LISTS_H

#include "ringNode.h"
#include "types.h"

/**
 * A collection of lists of dirty elements ordered by age. An element is always
 * placed on the oldest list in which it was dirtied (moving between lists or
 * removing altogether is cheap). Whenever the current period is advanced, any
 * elements older than the maxium age are expired. If an element is to be added
 * with a dirty age older than the maximum age, it is expired immediately.
 **/
typedef struct dirtyLists DirtyLists;

/**
 * A function which will be called with a ring of dirty elements which have
 * been expired. All of the expired elements must be removed from the ring
 * before this function returns.
 *
 * @param expired  The list of expired elements
 * @param context  The context for the callback
 **/
typedef void DirtyCallback(RingNode *expired, void *context);

/**
 * Construct a new set of dirty lists.
 *
 * @param [in]  maximumAge     The age at which an element will be expired
 * @param [in]  callback       The function to call when a set of elements have
 *                             expired
 * @param [in]  context        The context for the callback
 * @param [out] dirtyListsPtr  A pointer to hold the new DirtyLists
 *
 * @return VDO_SUCCESS or an error
 **/
int makeDirtyLists(BlockCount      maximumAge,
                   DirtyCallback  *callback,
                   void           *context,
                   DirtyLists    **dirtyListsPtr)
  __attribute__((warn_unused_result));

/**
 * Free a set of dirty lists and null out the pointer to them.
 *
 * @param dirtyListsPtr A pointer to the dirty lists to be freed
 **/
void freeDirtyLists(DirtyLists **dirtyListsPtr);

/**
 * Set the current period. This function should only be called once.
 *
 * @param dirtyLists  The dirtyLists
 * @param period      The current period
 **/
void setCurrentPeriod(DirtyLists *dirtyLists, SequenceNumber period);

/**
 * Add an element to the dirty lists.
 *
 * @param dirtyLists  The DirtyLists receiving the element
 * @param node        The RingNode of the element to add
 * @param oldPeriod   The period in which the element was previous dirtied,
 *                    or 0 if it was not dirty
 * @param newPeriod   The period in which the element has now been dirtied,
 *                    or 0 if it does not hold a lock
 **/
void addToDirtyLists(DirtyLists     *dirtyLists,
                     RingNode       *node,
                     SequenceNumber  oldPeriod,
                     SequenceNumber  newPeriod);

/**
 * Advance the current period. If the current period is greater than the number
 * of lists, expire the oldest lists.
 *
 * @param dirtyLists  The DirtyLists to advance
 * @param period      The new current period
 **/
void advancePeriod(DirtyLists *dirtyLists, SequenceNumber period);

/**
 * Flush all dirty lists. This will cause the period to be advanced past the
 * current period.
 *
 * @param dirtyLists  The dirtyLists to flush
 **/
void flushDirtyLists(DirtyLists *dirtyLists);

#endif // DIRTY_LISTS_H
