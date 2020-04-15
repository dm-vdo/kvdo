/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceRegistry.c#9 $
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
	struct kernel_layer *layer;
};

static struct device_registry registry;

/**********************************************************************/
void initialize_device_registry_once(void)
{
	INIT_LIST_HEAD(&registry.links);
	rwlock_init(&registry.lock);
}

/**
 * Implements LayerFilter.
 **/
static bool layer_is_equal(struct kernel_layer *layer, void *context)
{
	return ((void *) layer == context);
}

/**
 * Find a layer in the registry if it exists there. Must be called holding
 * the lock.
 *
 * @param filter   The filter function to apply to devices
 * @param context  A bit of context to provide the filter.
 *
 * @return the layer object found, if any
 **/
static struct kernel_layer * __must_check
filter_layers_locked(LayerFilter *filter, void *context)
{
	struct registered_device *device;

	list_for_each_entry(device, &registry.links, links) {
		if (filter(device->layer, context)) {
			return device->layer;
		}
	}
	return NULL;
}

/**********************************************************************/
int add_layer_to_device_registry(struct kernel_layer *layer)
{
	struct registered_device *new_device;
	int result =
		ALLOCATE(1, struct registered_device, __func__, &new_device);
	if (result != VDO_SUCCESS) {
		return result;
	}

	INIT_LIST_HEAD(&new_device->links);
	new_device->layer = layer;

	write_lock(&registry.lock);
	struct kernel_layer *old_layer = filter_layers_locked(layer_is_equal,
							      layer);
	result = ASSERT(old_layer == NULL, "Layer not already registered");
	if (result == VDO_SUCCESS) {
		list_add_tail(&new_device->links, &registry.links);
	}
	write_unlock(&registry.lock);

	return result;
}

/**********************************************************************/
void remove_layer_from_device_registry(struct kernel_layer *layer)
{
	write_lock(&registry.lock);
	struct registered_device *device = NULL;

	list_for_each_entry(device, &registry.links, links) {
		if (device->layer == layer) {
			list_del_init(&device->links);
			FREE(device);
			break;
		}
	}
	write_unlock(&registry.lock);
}

/**********************************************************************/
struct kernel_layer *find_layer_matching(LayerFilter *filter, void *context)
{
  read_lock(&registry.lock);
  struct kernel_layer *layer = filter_layers_locked(filter, context);

  read_unlock(&registry.lock);
  return layer;
}
