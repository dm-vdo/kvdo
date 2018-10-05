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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/partitionCopy.h#2 $
 */

#ifndef PARTITION_COPY_H
#define PARTITION_COPY_H

#include "fixedLayout.h"
#include "physicalLayer.h"
#include "types.h"

/**
 * Make a copy completion.
 *
 * @param [in]  layer          The layer on which the partitions reside
 * @param [out] completionPtr  A pointer to hold the copy completion
 *
 * @return VDO_SUCCESS or an error
 **/
int makeCopyCompletion(PhysicalLayer *layer, VDOCompletion **completionPtr)
  __attribute__((warn_unused_result));

/**
 * Free a copy completion and NULL out the reference to it.
 *
 * @param completionPtr  A pointer to the complete to be freed
 **/
void freeCopyCompletion(VDOCompletion **completionPtr);

/**
 * Copy a partition.
 *
 * @param completion    The copy completion to use
 * @param source        The partition to copy from
 * @param target        The partition to copy to
 * @param parent        The parent to finish when the copy is complete
 **/
void copyPartitionAsync(VDOCompletion *completion,
                        Partition     *source,
                        Partition     *target,
                        VDOCompletion *parent);

#endif /* PARTITION_COPY_H */
