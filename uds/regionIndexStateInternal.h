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
 * $Id: //eng/uds-releases/gloria/src/uds/regionIndexStateInternal.h#1 $
 */

#ifndef REGION_INDEX_STATE_INTERNAL_H
#define REGION_INDEX_STATE_INTERNAL_H

#include "regionIndexState.h"

/**
 * Open an IORegion for a specified state, mode, kind, and zone.
 * This helper function is used by RegionIndexComponent.
 *
 * @param ris           The region index state.
 * @param mode          One of IO_READ or IO_WRITE.
 * @param kind          The kind if index save region to open.
 * @param zone          The zone number for the region.
 * @param regionPtr     Where to store the region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int openRegionStateRegion(RegionIndexState  *ris,
                          IOAccessMode       mode,
                          RegionKind         kind,
                          unsigned int       zone,
                          IORegion         **regionPtr)
  __attribute__((warn_unused_result));

#endif // REGION_INDEX_STATE_INTERNAL_H
