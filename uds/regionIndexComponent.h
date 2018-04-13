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
 * $Id: //eng/uds-releases/gloria/src/uds/regionIndexComponent.h#1 $
 */

#ifndef REGION_INDEX_COMPONENT_H
#define REGION_INDEX_COMPONENT_H

#include "indexComponent.h"
#include "regionIndexState.h"

/**
 * Make an index component for a SingleFileLayout.
 *
 * @param ris           The region index state of in which this component
 *                        instance shall reside.
 * @param info          The component info specification for this component.
 * @param zoneCount     How many active zones are in use.
 * @param data          Component-specific data.
 * @param context       Component-specific context.
 * @param componentPtr  Where to store the resulting component.
 *
 * @return UDS_SUCCESS or an error code
 **/
int makeRegionIndexComponent(RegionIndexState          *ris,
                             const IndexComponentInfo  *info,
                             unsigned int               zoneCount,
                             void                      *data,
                             void                      *context,
                             IndexComponent           **componentPtr)
  __attribute__((warn_unused_result));

#endif // REGION_INDEX_COMPONENT_H
