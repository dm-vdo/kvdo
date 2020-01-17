/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/commonStats.h#1 $
 *
 */

#ifndef STATUS_PROC_H
#define STATUS_PROC_H

#include "kernelLayer.h"

/**
 * Retrieves the current kernel statistics.
 *
 * @param layer    the kernel layer
 * @param stats    pointer to the structure to fill in
 */
void get_kernel_statistics(struct kernel_layer *layer, KernelStatistics *stats);

#endif  /* STATUS_PROC_H */
