/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef MESSAGE_STATS_H
#define MESSAGE_STATS_H

#include "types.h"

int vdo_write_stats(struct vdo *vdo, char *buf, unsigned int maxlen);

#endif  /* MESSAGE_STATS_H */
