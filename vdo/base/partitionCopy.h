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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/partitionCopy.h#4 $
 */

#ifndef PARTITION_COPY_H
#define PARTITION_COPY_H

#include "fixedLayout.h"
#include "physicalLayer.h"
#include "types.h"

/**
 * Make a copy completion.
 *
 * @param [in]  layer           The layer on which the partitions reside
 * @param [out] completion_ptr  A pointer to hold the copy completion
 *
 * @return VDO_SUCCESS or an error
 **/
int make_copy_completion(PhysicalLayer *layer,
			 struct vdo_completion **completion_ptr)
	__attribute__((warn_unused_result));

/**
 * Free a copy completion and NULL out the reference to it.
 *
 * @param completion_ptr  A pointer to the complete to be freed
 **/
void free_copy_completion(struct vdo_completion **completion_ptr);

/**
 * Copy a partition.
 *
 * @param completion    The copy completion to use
 * @param source        The partition to copy from
 * @param target        The partition to copy to
 * @param parent        The parent to finish when the copy is complete
 **/
void copy_partition_async(struct vdo_completion *completion,
			  struct partition *source,
			  struct partition *target,
			  struct vdo_completion *parent);

#endif /* PARTITION_COPY_H */
