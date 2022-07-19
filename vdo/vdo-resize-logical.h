/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_RESIZE_LOGICAL_H
#define VDO_RESIZE_LOGICAL_H

#include "kernel-types.h"
#include "types.h"

int vdo_perform_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks);

int vdo_prepare_to_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks);

#endif /* VDO_RESIZE_LOGICAL_H */
