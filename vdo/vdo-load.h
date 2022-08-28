/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_LOAD_H
#define VDO_LOAD_H

#include "kernel-types.h"

int __must_check vdo_load(struct vdo *vdo);

int __must_check
vdo_prepare_to_load(struct vdo *vdo);

#endif /* VDO_LOAD_H */
