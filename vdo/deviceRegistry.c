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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/deviceRegistry.c#4 $
 */

#include "deviceRegistry.h"

#include <linux/list.h>
#include <linux/types.h>
#include <linux/spinlock.h>

#include "memoryAlloc.h"
#include "permassert.h"

#include "statusCodes.h"
#include "types.h"
#include "vdo.h"

/*
 * We don't expect this set to ever get really large, so a linked list
 * is adequate. We can use a pointer_map if we need to later.
 */
struct device_registry {
	struct list_head links;
	// XXX: (Some) Kernel docs say rwlocks are being deprecated in favor of
	// RCU, please don't add more. Should we switch?
	rwlock_t lock;
};

static struct device_registry registry;

/**********************************************************************/
void initialize_vdo_device_registry_once(void)
{
	INIT_LIST_HEAD(&registry.links);
	rwlock_init(&registry.lock);
}

/**
 * Implements vdo_filter_t.
 **/
static bool vdo_is_equal(struct vdo *vdo, void *context)
{
	return ((void *) vdo == context);
}

/**
 * Find a vdo in the registry if it exists there. Must be called holding
 * the lock.
 *
 * @param filter   The filter function to apply to devices
 * @param context  A bit of context to provide the filter.
 *
 * @return the layer object found, if any
 **/
static struct vdo * __must_check
filter_vdos_locked(vdo_filter_t *filter, void *context)
{
	struct vdo *vdo;

	list_for_each_entry(vdo, &registry.links, registration) {
		if (filter(vdo, context)) {
			return vdo;
		}
	}

	return NULL;
}

/**********************************************************************/
int register_vdo(struct vdo *vdo)
{
	int result;

	write_lock(&registry.lock);
	result = ASSERT(filter_vdos_locked(vdo_is_equal, vdo) == NULL,
			"VDO not already registered");
	if (result == VDO_SUCCESS) {
		INIT_LIST_HEAD(&vdo->registration);
		list_add_tail(&vdo->registration, &registry.links);
	}
	write_unlock(&registry.lock);

	return result;
}

/**********************************************************************/
void unregister_vdo(struct vdo *vdo)
{
	write_lock(&registry.lock);
	if (filter_vdos_locked(vdo_is_equal, vdo) == vdo) {
		list_del_init(&vdo->registration);
	}

	write_unlock(&registry.lock);
}

/**********************************************************************/
struct vdo *find_vdo_matching(vdo_filter_t *filter, void *context)
{
	struct vdo *vdo;

	read_lock(&registry.lock);
	vdo = filter_vdos_locked(filter, context);
	read_unlock(&registry.lock);
	return vdo;
}
