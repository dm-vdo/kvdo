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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceRegistry.c#4 $
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
struct device_registry {
	struct list_head links;
	rwlock_t lock;
};

struct registered_device {
	struct list_head links;
	char *name;
	KernelLayer *layer;
};

static struct device_registry registry;

/**********************************************************************/
void initialize_device_registry_once(void)
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
static struct registered_device *find_layer_locked(char *name)
{
	struct registered_device *device;
	list_for_each_entry (device, &registry.links, links) {
		if (strcmp(device->name, name) == 0) {
			return device;
		}
	}
	return NULL;
}

/**********************************************************************/
KernelLayer *get_layer_by_name(char *name)
{
	read_lock(&registry.lock);
	struct registered_device *device = find_layer_locked(name);
	read_unlock(&registry.lock);
	if (device == NULL) {
		return NULL;
	}
	return device->layer;
}

/**********************************************************************/
int add_layer_to_device_registry(char *name, KernelLayer *layer)
{
	struct registered_device *new_device;
	int result =
		ALLOCATE(1, struct registered_device, __func__, &new_device);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = duplicateString(name, "name", &new_device->name);
	if (result != VDO_SUCCESS) {
		FREE(new_device);
		return result;
	}

	INIT_LIST_HEAD(&new_device->links);
	new_device->layer = layer;

	write_lock(&registry.lock);
	struct registered_device *old_device = find_layer_locked(name);
	result = ASSERT(old_device == NULL, "Device not already registered");
	if (result == VDO_SUCCESS) {
		list_add_tail(&new_device->links, &registry.links);
	}
	write_unlock(&registry.lock);

	return result;
}

/**********************************************************************/
void remove_layer_from_device_registry(char *name)
{
	write_lock(&registry.lock);
	struct registered_device *device = find_layer_locked(name);
	if (device != NULL) {
		list_del_init(&device->links);
		FREE(device->name);
	}
	write_unlock(&registry.lock);
	FREE(device);
}
