/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef REFERENCE_COUNT_REBUILD_H
#define REFERENCE_COUNT_REBUILD_H

#include "kernel-types.h"
#include "types.h"

void vdo_rebuild_reference_counts(struct vdo *vdo,
				  struct vdo_completion *parent,
				  block_count_t *logical_blocks_used,
				  block_count_t *block_map_data_blocks);

#endif /* REFERENCE_COUNT_REBUILD_H */
