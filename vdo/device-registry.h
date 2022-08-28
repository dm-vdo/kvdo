/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "kernel-types.h"
#include "types.h"

/**
 * typedef vdo_filter_t - Method type for vdo matching methods.
 *
 * A filter function returns false if the vdo doesn't match.
 */
typedef bool vdo_filter_t(struct vdo *vdo, void *context);

void vdo_initialize_device_registry_once(void);

int __must_check vdo_register(struct vdo *vdo);

void vdo_unregister(struct vdo *vdo);

struct vdo * __must_check
vdo_find_matching(vdo_filter_t *filter, void *context);

#endif /* DEVICE_REGISTRY_H */
