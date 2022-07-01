/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef READ_ONLY_REBUILD_H
#define READ_ONLY_REBUILD_H

#include "completion.h"
#include "vdo.h"

void vdo_launch_rebuild(struct vdo *vdo, struct vdo_completion *parent);

#endif /* READ_ONLY_REBUILD_H */
