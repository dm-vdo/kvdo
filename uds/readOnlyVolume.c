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
 * $Id: //eng/uds-releases/gloria/src/uds/readOnlyVolume.c#3 $
 */

#include "readOnlyVolume.h"

#include "volumeInternals.h"
#include "zone.h"

/**********************************************************************/
int makeReadOnlyVolume(const Configuration  *config,
                       IndexLayout          *layout,
                       Volume              **newVolume)
{
  return allocateVolume(config, layout, VOLUME_CACHE_DEFAULT_MAX_QUEUED_READS,
                        1, READ_ONLY_VOLUME, newVolume);
}

/**********************************************************************/
int getReadOnlyPage(Volume       *volume,
                    unsigned int  chapter,
                    unsigned int  pageNumber)
{
  int physicalPage = mapToPhysicalPage(volume->geometry, chapter, pageNumber);
  int result = readPageToBuffer(volume, physicalPage, volume->scratchPage);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return UDS_SUCCESS;
}
