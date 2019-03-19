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
 * $Id: //eng/uds-releases/gloria/src/uds/regionIndexComponent.c#2 $
 */

#include "regionIndexComponentInternal.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "regionIndexStateInternal.h"
#include "singleFileLayoutInternals.h"

static const IndexComponentOps *getRegionIndexComponentOps(void);

/*****************************************************************************/
int makeRegionIndexComponent(RegionIndexState          *ris,
                             const IndexComponentInfo  *info,
                             unsigned int               zoneCount,
                             void                      *data,
                             void                      *context,
                             IndexComponent           **componentPtr)
{
  RegionIndexComponent *ric = NULL;
  int result = ALLOCATE(1, RegionIndexComponent, "region index component",
                        &ric);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initIndexComponent(&ric->common, info, zoneCount, data, context,
                              getRegionIndexComponentOps());
  if (result != UDS_SUCCESS) {
    FREE(ric);
    return result;
  }

  ric->ris = ris;

  *componentPtr = &ric->common;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static void ric_freeIndexComponent(IndexComponent *component)
{
  if (component == NULL) {
    return;
  }

  RegionIndexComponent *ric = asRegionIndexComponent(component);
  destroyIndexComponent(component);
  FREE(ric);
}

/*****************************************************************************/
static int ric_openWriteRegion(WriteZone *writeZone)
{
  RegionIndexComponent *ric = asRegionIndexComponent(writeZone->component);

  return openRegionStateRegion(ric->ris,
                               IO_WRITE,
                               ric->common.info->kind,
                               writeZone->zone,
                               &writeZone->region);
}

/*****************************************************************************/
static int ric_cleanupWriteFailure(IndexComponent *component
                                   __attribute__((unused)))
{
  // this is a no-op
  return UDS_SUCCESS;
}

/*****************************************************************************/
static void ric_freeWriteZones(IndexComponent *component)
{
  freeWriteZonesHelper(component, NULL);
}

/*****************************************************************************/
static int makeRegionWriteZone(RegionIndexComponent  *ric,
                               unsigned int           zone,
                               WriteZone            **zonePtr)
{
  WriteZone *wz = NULL;
  int result = ALLOCATE(1, WriteZone, "plain write zone", &wz);
  if (result != UDS_SUCCESS) {
    return result;
  }

  *wz = (WriteZone) {
    .component = &ric->common,
    .phase     = IWC_IDLE,
    .region    = NULL,
    .zone      = zone,
  };

  *zonePtr = wz;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ric_populateWriteZones(IndexComponent *component)
{
  RegionIndexComponent *ric = asRegionIndexComponent(component);

  for (unsigned int z = 0; z < component->numZones; ++z) {
    int result = makeRegionWriteZone(ric, z, &component->writeZones[z]);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ric_prepareZones(IndexComponent *component __attribute__((unused)),
                            unsigned int    numZones __attribute__((unused)))
{
  // this is a no-op
  return UDS_SUCCESS;
}

/*****************************************************************************/
static void ric_freeReadPortal(ReadPortal *portal)
{
  destroyReadPortal(portal);
  FREE(portal);
}

/*****************************************************************************/
static int ric_createReadPortal(IndexComponent  *component,
                                ReadPortal     **portalPtr)
{
  RegionIndexComponent *ric = asRegionIndexComponent(component);

  ReadPortal *portal;
  int result = ALLOCATE(1, ReadPortal, "region index component read portal",
                        &portal);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = initReadPortal(portal, component, ric->ris->loadZones);
  if (result != UDS_SUCCESS) {
    FREE(portal);
    return result;
  }

  for (unsigned int z = 0; z < portal->zones; ++z) {
    result = openRegionStateRegion(ric->ris, IO_READ, component->info->kind, z,
                                   &portal->regions[z]);
    if (result != UDS_SUCCESS) {
      while (z > 0) {
        closeIORegion(&portal->regions[--z]);
      }
      ric_freeReadPortal(portal);
      return result;
    }
  }

  *portalPtr = portal;
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int ric_discardIndexComponent(IndexComponent *component)
{
  RegionIndexComponent *ric = asRegionIndexComponent(component);
  int result = ASSERT((ric->ris->state.id == 0),
                      "Cannot have multiple subindices");
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int numZones = 0;
  unsigned int saveSlot = 0;
  result = findLatestIndexSaveSlot(ric->ris->sfl, &numZones, &saveSlot);
  if (result != UDS_SUCCESS) {
    return result;
  }

  unsigned int oldSaveSlot = ric->ris->saveSlot;
  ric->ris->saveSlot = saveSlot;

  for (unsigned int z = 0; z < numZones; ++z) {
    IORegion *region;
    result = openRegionStateRegion(ric->ris, IO_WRITE, component->info->kind,
                                   z, &region);
    if (result != UDS_SUCCESS) {
      break;
    }
    result = clearRegion(region);
    closeIORegion(&region);
    if (result != UDS_SUCCESS) {
      break;
    }
  }

  ric->ris->saveSlot = oldSaveSlot;

  return result;
}

/*****************************************************************************/

static const IndexComponentOps regionIndexComponentOps = {
  .freeMe           = ric_freeIndexComponent,
  .openWriteRegion  = ric_openWriteRegion,
  .cleanupWrite     = ric_cleanupWriteFailure,
  .populateZones    = ric_populateWriteZones,
  .freeZones        = ric_freeWriteZones,
  .prepareZones     = ric_prepareZones,
  .createReadPortal = ric_createReadPortal,
  .freeReadPortal   = ric_freeReadPortal,
  .discard          = ric_discardIndexComponent,
};

static const IndexComponentOps *getRegionIndexComponentOps(void)
{
  return &regionIndexComponentOps;
}
