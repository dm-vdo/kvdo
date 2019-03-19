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
 * $Id: //eng/uds-releases/gloria/src/uds/indexComponentInternal.h#1 $
 */

#ifndef INDEX_COMPONENT_INTERNAL_H
#define INDEX_COMPONENT_INTERNAL_H

#include "indexComponent.h"

struct writeZone {
  IndexComponent           *component;
  IncrementalWriterCommand  phase;
  IORegion                 *region;
  BufferedWriter           *writer;
  unsigned int              zone;
};

/**
 * Initialize the common part of an index component.
 *
 * @param component     the component sub-object
 * @param info          a component info specification
 * @param zoneCount     how many actual zones are in use
 * @param data          private data associated with the component
 * @param context       private context associated with the component
 * @param ops           the vector of index component operations
 *
 * @return UDS_SUCCESS or an error code
 **/
int initIndexComponent(IndexComponent           *component,
                       const IndexComponentInfo *info,
                       unsigned int              zoneCount,
                       void                     *data,
                       void                     *context,
                       const IndexComponentOps  *ops)
  __attribute__((warn_unused_result));

/**
 * Destroy the common part of an index component.
 *
 * @param component     the index component sub-object
 **/
void destroyIndexComponent(IndexComponent *component);

/**
 * Construct an array of ReadPortal instances, one for each
 * zone which exists in the directory.
 *
 * @param [in]  portal          a read portal instance
 * @param [in]  component       the index compoennt
 * @param [in]  readZones       actual number of read zones
 *
 * @return UDS_SUCCESS or an error code
 **/
int initReadPortal(ReadPortal               *portal,
                   IndexComponent           *component,
                   unsigned int              readZones)
  __attribute__((warn_unused_result));

/**
 * Destroy the contents of a read portal.
 *
 * @param readPortal     the read portal
 **/
void destroyReadPortal(ReadPortal *readPortal);

/**
 * Destroy the contents of a write zone.
 *
 * @param writeZone     the write zone
 **/
void destroyWriteZone(WriteZone *writeZone);

/**
 * Generic helper to free write zones.
 *
 * @param component     the index component
 * @param zoneFreeFunc  if non-NULL, a function to free the zone
 **/
void freeWriteZonesHelper(IndexComponent  *component,
                          void           (*zoneFreeFunc)(WriteZone *wz));

#endif // INDEX_COMPONENT_INTERNAL_H
