/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_RESUME_H
#define VDO_RESUME_H

#include "kernel-types.h"
#include "types.h"

int vdo_preresume_internal(struct vdo *vdo,
			   struct device_config *config,
			   const char *device_name);

#endif /* VDO_RESUME_H */
