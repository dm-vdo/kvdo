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
 * $Id: //eng/uds-releases/gloria/src/uds/regionIndexState.h#2 $
 */

#ifndef REGION_INDEX_STATE_H
#define REGION_INDEX_STATE_H

#include "indexStateInternals.h"
#include "permassert.h"
#include "singleFileLayout.h"

typedef struct regionIndexState {
  IndexState         state;
  SingleFileLayout  *sfl;
  unsigned int       loadZones;
  unsigned int       loadSlot;
  unsigned int       saveSlot;
} RegionIndexState;

/**
 * Allocate a region index state structure.
 *
 * @param sfl           A single file layout.
 * @param zoneCount     The number of zones
 * @param length        Number of components to hold.
 * @param statePtr      The pointer to hold the new index state.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int makeRegionIndexState(SingleFileLayout  *sfl,
                         unsigned int       zoneCount,
                         unsigned int       length,
                         IndexState       **statePtr)
  __attribute__((warn_unused_result));

/*****************************************************************************/
static INLINE RegionIndexState *asRegionIndexState(IndexState *state)
{
  return container_of(state, RegionIndexState, state);
}

#endif // REGION_INDEX_STATE_H
