/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef POOL_SYSFS_H
#define POOL_SYSFS_H

#include <linux/kobject.h>

/* The kobj_type used for setting up the kernel layer kobject. */
extern struct kobj_type vdo_directory_type;

/* The sysfs_ops used for the "statistics" subdirectory. */
extern struct sysfs_ops vdo_pool_stats_sysfs_ops;
/* The attribute used for the "statistics" subdirectory. */
extern struct attribute *vdo_pool_stats_attrs[];

#endif /* POOL_SYSFS_H */
