/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/statusProcfs.h#3 $
 *
 */

#ifndef STATUS_PROC_H
#define STATUS_PROC_H

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "kernelLayer.h"

/**
 * Initializes the /proc/vdo directory. Should be called once when the
 * module is loaded.
 *
 * @return 0 on success, nonzero on failure
 */
int vdo_init_procfs(void);

/**
 * Destroys the /proc/vdo directory. Should be called once when the
 * module is unloaded.
 */
void vdo_destroy_procfs(void);

/**
 * Creates a subdirectory in the /proc/vdo filesystem for a particular
 * vdo.
 *
 * @param layer    the kernel layer
 * @param name     the subdirectory name
 * @param private  pointer to private storage for procfs data
 *
 * @return 0 on success, nonzero on failure
 */
int vdo_create_procfs_entry(struct kernel_layer *layer,
			    const char *name,
			    void **private);

/**
 * Destroys a subdirectory in the /proc/vdo filesystem for a
 * particular vdo.
 *
 * @param name     the subdirectory name
 * @param private  private storage for procfs data
 */
void vdo_destroy_procfs_entry(const char *name, void *private);

/**
 * Retrieves the current kernel statistics.
 *
 * @param layer    the kernel layer
 * @param stats    pointer to the structure to fill in
 */
void get_kernel_stats(struct kernel_layer *layer, KernelStatistics *stats);

#endif  /* STATUS_PROC_H */
