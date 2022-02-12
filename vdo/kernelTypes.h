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
 *
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/kernelTypes.h#18 $
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include "types.h"

struct atomic_bio_stats;
struct dedupe_context;
struct dedupe_index;
struct kernel_layer;
struct vdo_work_item;
struct vdo_work_queue;

typedef void (*vdo_work_function)(struct vdo_work_item *work_item);

#endif /* KERNEL_TYPES_H */
