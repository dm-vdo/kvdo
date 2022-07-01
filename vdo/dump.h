/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DUMP_H
#define DUMP_H

#include "kernel-types.h"

int vdo_dump(struct vdo *vdo,
	     unsigned int argc,
	     char *const *argv,
	     const char *why);

void vdo_dump_all(struct vdo *vdo, const char *why);

void dump_data_vio(void *data);

#endif /* DUMP_H */
