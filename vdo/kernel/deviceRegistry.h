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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/deviceRegistry.h#1 $
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "kernelTypes.h"

/**
 * Initialize the necessary structures for the device registry.
 **/
void initializeDeviceRegistryOnce(void);

/**
 * Get a layer by name, if any layer has that device name.
 *
 * @param name  The name to look up
 *
 * @return the layer, if any was found
 **/
KernelLayer *getLayerByName(char *name)
  __attribute__((warn_unused_result));

/**
 * Add a layer and name to the device registry. The name must not
 * already exist in the registry, and will be duplicated.
 *
 * @param name   The name of the layer
 * @param layer  The layer to add
 *
 * @return VDO_SUCCESS or an error
 **/
int addLayerToDeviceRegistry(char *name, KernelLayer *layer)
  __attribute__((warn_unused_result));

/**
 * Remove a layer from the device registry.
 *
 * @param name  The name of the layer to remove
 **/
void removeLayerFromDeviceRegistry(char *name);

#endif // DEVICE_REGISTRY_H
