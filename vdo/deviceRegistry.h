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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/deviceRegistry.h#3 $
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "kernelTypes.h"
#include "types.h"

/**
 * Method type for vdo matching methods.
 *
 * A filter function returns false if the vdo doesn't match.
 **/
typedef bool vdo_filter_t(struct vdo *vdo, void *context);

/**
 * Initialize the necessary structures for the device registry.
 **/
void initialize_vdo_device_registry_once(void);

/**
 * Register a VDO; it must not already be registered.
 *
 * @param vdo  The vdo to register
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check register_vdo(struct vdo *vdo);

/**
 * Remove a vdo from the device registry.
 *
 * @param vdo  The vdo to remove
 **/
void unregister_vdo(struct vdo *vdo);

/**
 * Find and return the first (if any) vdo matching a given filter function.
 *
 * @param filter   The filter function to apply to vdos
 * @param context  A bit of context to provide the filter
 **/
struct vdo * __must_check
find_vdo_matching(vdo_filter_t *filter, void *context);

#endif // DEVICE_REGISTRY_H
