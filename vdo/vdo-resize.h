/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_RESIZE_H
#define VDO_RESIZE_H

#include "kernel-types.h"
#include "types.h"

int vdo_perform_grow_physical(struct vdo *vdo,
			      block_count_t new_physical_blocks);

int __must_check
vdo_prepare_to_grow_physical(struct vdo *vdo,
			     block_count_t new_physical_blocks);

#endif /* VDO_RESIZE_H */
