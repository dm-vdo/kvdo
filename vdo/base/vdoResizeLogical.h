/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoResizeLogical.h#2 $
 */

#ifndef VDO_RESIZE_LOGICAL_H
#define VDO_RESIZE_LOGICAL_H

#include "types.h"

/**
 * Grow the logical size of the vdo. This method may only be called when the
 * vdo has been suspended and must not be called from a base thread.
 *
 * @param vdo               The vdo to grow
 * @param newLogicalBlocks  The size to which the vdo should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int performGrowLogical(struct vdo *vdo, BlockCount newLogicalBlocks);

/**
 * Prepare to grow the logical size of vdo. This method may only be called
 * while the vdo is running.
 *
 * @param vdo               The vdo to prepare for growth
 * @param newLogicalBlocks  The size to which the vdo should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int prepareToGrowLogical(struct vdo *vdo, BlockCount newLogicalBlocks);

#endif /* VDO_RESIZE_LOGICAL_H */
