/*
 * Copyright Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 */

#ifndef POOL_SYSFS_H
#define POOL_SYSFS_H

#include <linux/kobject.h>

// The kobj_type used for setting up the kernel layer kobject.
extern struct kobj_type vdo_directory_type;

// The sysfs_ops used for the "statistics" subdirectory.
extern struct sysfs_ops vdo_pool_stats_sysfs_ops;
// The attribute used for the "statistics" subdirectory.
extern struct attribute *vdo_pool_stats_attrs[];

#endif /* POOL_SYSFS_H */
