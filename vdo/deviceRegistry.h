/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/deviceRegistry.h#5 $
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "kernelTypes.h"

/**
 * Initialize the necessary structures for the device registry.
 **/
void initialize_device_registry_once(void);

/**
 * Add a layer to the device registry. The layer must not already exist in the
 * registry.
 *
 * @param layer  The layer to add
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check add_layer_to_device_registry(struct kernel_layer *layer);

/**
 * Remove a layer from the device registry.
 *
 * @param layer  The layer to remove
 **/
void remove_layer_from_device_registry(struct kernel_layer *layer);

/**
 * Find and return the first (if any) layer matching a given filter function.
 *
 * @param filter   The filter function to apply to layers
 * @param context  A bit of context to provide the filter.
 **/
struct kernel_layer * __must_check
find_layer_matching(LayerFilter *filter, void *context);

#endif // DEVICE_REGISTRY_H
