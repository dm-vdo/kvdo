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
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "kernel-types.h"
#include "types.h"

/**
 * Method type for vdo matching methods.
 *
 * A filter function returns false if the vdo doesn't match.
 **/
typedef bool vdo_filter_t(struct vdo *vdo, void *context);

void vdo_initialize_device_registry_once(void);

int __must_check vdo_register(struct vdo *vdo);

void vdo_unregister(struct vdo *vdo);

struct vdo * __must_check
vdo_find_matching(vdo_filter_t *filter, void *context);

#endif /* DEVICE_REGISTRY_H */
