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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoState.c#1 $
 */

#include "vdoState.h"

#include "permassert.h"

static const char *VDO_STATE_NAMES[] = {
  [VDO_CLEAN]               = "CLEAN",
  [VDO_DIRTY]               = "DIRTY",
  [VDO_FORCE_REBUILD]       = "FORCE_REBUILD",
  [VDO_NEW]                 = "NEW",
  [VDO_READ_ONLY_MODE]      = "READ_ONLY_MODE",
  [VDO_REBUILD_FOR_UPGRADE] = "REBUILD_FOR_UPGRADE",
  [VDO_RECOVERING]          = "RECOVERING",
  [VDO_REPLAYING]           = "REPLAYING",
};

/**********************************************************************/
const char *getVDOStateName(VDOState state)
{
  // Catch if a state has been added without updating the name array.
  STATIC_ASSERT(COUNT_OF(VDO_STATE_NAMES) == VDO_STATE_COUNT);

  int result = ASSERT(state < COUNT_OF(VDO_STATE_NAMES),
                      "VDOState value %u must have a registered name", state);
  if (result != UDS_SUCCESS) {
    return "INVALID VDO STATE CODE";
  }

  return VDO_STATE_NAMES[state];
}

/**********************************************************************/
const char *describeVDOState(VDOState state)
{
  // These strings should all fit in the 15 chars of VDOStatistics.mode.
  switch (state) {
  case VDO_RECOVERING:
    return "recovering";

  case VDO_READ_ONLY_MODE:
    return "read-only";

  default:
    return "normal";
  }
}
