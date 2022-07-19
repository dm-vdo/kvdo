// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "device-registry.h"

#include <linux/list.h>
#include <linux/types.h>
#include <linux/spinlock.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "kernel-types.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"

/*
 * We don't expect this set to ever get really large, so a linked list
 * is adequate. We can use a pointer_map if we need to later.
 */
struct device_registry {
	struct list_head links;
	/*
	 * XXX: (Some) Kernel docs say rwlocks are being deprecated in favor of
	 * RCU, please don't add more. Should we switch?
	 */
	rwlock_t lock;
};

static struct device_registry registry;

/**
 * vdo_initialize_device_registry_once() - Initialize the necessary
 *                                         structures for the device registry.
 */
void vdo_initialize_device_registry_once(void)
{
	INIT_LIST_HEAD(&registry.links);
	rwlock_init(&registry.lock);
}

/**
 * vdo_is_equal() - Implements vdo_filter_t.
 */
static bool vdo_is_equal(struct vdo *vdo, void *context)
{
	return ((void *) vdo == context);
}

/**
 * filter_vdos_locked() - Find a vdo in the registry if it exists there.
 * @filter: The filter function to apply to devices.
 * @context: A bit of context to provide the filter.
 *
 * Context: Must be called holding the lock.
 *
 * Return: the vdo object found, if any.
 */
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

/**
 * vdo_register() - Register a VDO; it must not already be registered.
 * @vdo: The vdo to register.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_register(struct vdo *vdo)
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

/**
 * vdo_unregister() - Remove a vdo from the device registry.
 * @vdo: The vdo to remove.
 */
void vdo_unregister(struct vdo *vdo)
{
	write_lock(&registry.lock);
	if (filter_vdos_locked(vdo_is_equal, vdo) == vdo) {
		list_del_init(&vdo->registration);
	}

	write_unlock(&registry.lock);
}

/**
 * vdo_find_matching() - Find and return the first (if any) vdo matching a
 *                       given filter function.
 * @filter: The filter function to apply to vdos.
 * @context: A bit of context to provide the filter.
 */
struct vdo *vdo_find_matching(vdo_filter_t *filter, void *context)
{
	struct vdo *vdo;

	read_lock(&registry.lock);
	vdo = filter_vdos_locked(filter, context);
	read_unlock(&registry.lock);
	return vdo;
}
