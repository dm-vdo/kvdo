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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/deviceRegistry.c#1 $
 */

#include "deviceRegistry.h"

#include <linux/list.h>
#include <linux/types.h>
#include <linux/spinlock.h>

#include "memoryAlloc.h"

/*
 * We don't expect this set to ever get really large, so a linked list
 * is adequate. We can use a PointerMap if we need to later.
 */
typedef struct {
  struct list_head links;
  rwlock_t         lock;
} DeviceRegistry;

typedef struct {
  struct list_head    links;
  char               *name;
  KernelLayer        *layer;
} RegisteredDevice;

static DeviceRegistry registry;

/**********************************************************************/
void initializeDeviceRegistryOnce(void)
{
  INIT_LIST_HEAD(&registry.links);
  rwlock_init(&registry.lock);
}

/**
 * Find a layer in the registry if it exists there. Must be called holding
 * the lock.
 *
 * @param name       The name of the layer to remove
 *
 * @return the device object found, if any
 **/
__attribute__((warn_unused_result))
static RegisteredDevice *findLayerLocked(char *name)
{
  RegisteredDevice *device;
  list_for_each_entry(device, &registry.links, links) {
    if (strcmp(device->name, name) == 0) {
      return device;
    }
  }
  return NULL;
}

/**********************************************************************/
KernelLayer *getLayerByName(char *name)
{
  read_lock(&registry.lock);
  RegisteredDevice *device = findLayerLocked(name);
  read_unlock(&registry.lock);
  if (device == NULL) {
    return NULL;
  }
  return device->layer;
}

/**********************************************************************/
int addLayerToDeviceRegistry(char *name, KernelLayer *layer)
{
  RegisteredDevice *newDevice;
  int result = ALLOCATE(1, RegisteredDevice, __func__, &newDevice);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = duplicateString(name, "name", &newDevice->name);
  if (result != VDO_SUCCESS) {
    FREE(newDevice);
    return result;
  }

  INIT_LIST_HEAD(&newDevice->links);
  newDevice->layer = layer;

  write_lock(&registry.lock);
  RegisteredDevice *oldDevice = findLayerLocked(name);
  result = ASSERT(oldDevice == NULL, "Device not already registered");
  if (result == VDO_SUCCESS) {
    list_add_tail(&newDevice->links, &registry.links);
  }
  write_unlock(&registry.lock);

  return result;
}

/**********************************************************************/
void removeLayerFromDeviceRegistry(char *name)
{
  write_lock(&registry.lock);
  RegisteredDevice *device = findLayerLocked(name);
  if (device != NULL) {
    list_del_init(&device->links);
    FREE(device->name);
  }
  write_unlock(&registry.lock);
  FREE(device);
}
