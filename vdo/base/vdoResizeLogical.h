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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResizeLogical.h#1 $
 */

#ifndef VDO_RESIZE_LOGICAL_H
#define VDO_RESIZE_LOGICAL_H

#include "types.h"

/**
 * Grow the logical size of the VDO. This method may only be called when the
 * VDO has been suspended and must not be called from a base thread.
 *
 * @param vdo               The VDO to grow
 * @param newLogicalBlocks  The size to which the VDO should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int performGrowLogical(VDO *vdo, BlockCount newLogicalBlocks);

/**
 * Prepare to grow the logical size of the VDO. This method may only be called
 * while the VDO is running.
 *
 * @param vdo               The VDO to prepare for growth
 * @param newLogicalBlocks  The size to which the VDO should be grown
 *
 * @return VDO_SUCCESS or an error
 **/
int prepareToGrowLogical(VDO *vdo, BlockCount newLogicalBlocks);

#endif /* VDO_RESIZE_LOGICAL_H */
