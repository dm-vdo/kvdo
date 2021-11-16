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

#ifndef PARTITION_COPY_H
#define PARTITION_COPY_H

#include "fixed-layout.h"
#include "kernel-types.h"
#include "types.h"

/**
 * Make a copy completion.
 *
 * @param [in]  vdo             The VDO on which the partitions reside
 * @param [out] completion_ptr  A pointer to hold the copy completion
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
vdo_make_copy_completion(struct vdo *vdo,
			 struct vdo_completion **completion_ptr);

void vdo_free_copy_completion(struct vdo_completion *completion);

void vdo_copy_partition(struct vdo_completion *completion,
			struct partition *source,
			struct partition *target,
			struct vdo_completion *parent);

#endif /* PARTITION_COPY_H */
