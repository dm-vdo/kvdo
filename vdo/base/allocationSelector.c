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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/allocationSelector.c#1 $
 */

#include "allocationSelector.h"
#include "allocationSelectorInternals.h"

#include "memoryAlloc.h"

#include "types.h"

enum {
  ALLOCATIONS_PER_ZONE = 128,
};

/**********************************************************************/
int makeAllocationSelector(ZoneCount            physicalZoneCount,
                           ThreadID             threadID,
                           AllocationSelector **selectorPtr)
{
  AllocationSelector *selector;
  int result = ALLOCATE(1, AllocationSelector, __func__, &selector);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *selector = (AllocationSelector) {
    .nextAllocationZone = threadID % physicalZoneCount,
    .lastPhysicalZone   = physicalZoneCount - 1,
  };

  *selectorPtr = selector;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeAllocationSelector(AllocationSelector **selectorPtr)
{
  AllocationSelector *selector = *selectorPtr;
  if (selector == NULL) {
    return;
  }

  FREE(selector);
  *selectorPtr = NULL;
}

/**********************************************************************/
ZoneCount getNextAllocationZone(AllocationSelector *selector)
{
  if (selector->lastPhysicalZone > 0) {
    if (selector->allocationCount < ALLOCATIONS_PER_ZONE) {
      selector->allocationCount++;
    } else {
      selector->allocationCount = 1;
      if (selector->nextAllocationZone < selector->lastPhysicalZone) {
        selector->nextAllocationZone++;
      } else {
        selector->nextAllocationZone = 0;
      }
    }
  }

  return selector->nextAllocationZone;
}
