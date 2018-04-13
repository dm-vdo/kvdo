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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResize.h#1 $
 */

#ifndef VDO_RESIZE_H
#define VDO_RESIZE_H

#include "types.h"

/**
 * Make the completion for an asynchronous resize.
 *
 * @param vdo                The VDO
 * @param newPhysicalBlocks  The new physical size in blocks
 * @param completionPtr      A pointer to hold the completion
 *
 * @return VDO_SUCCESS or an error
 **/
int makeResizeVDOCompletion(VDO            *vdo,
                            BlockCount      newPhysicalBlocks,
                            VDOCompletion **completionPtr)
  __attribute__((warn_unused_result));

/**
 * Free the completion for an asynchronous resize, and NULL out the
 * reference to it.
 *
 * @param completionPtr  A reference to the completion to free
 **/
void freeResizeVDOCompletion(VDOCompletion **completionPtr);

/**
 * Grow the physical size of the VDO. This method may only be called when the
 * VDO has been suspended and must not be called from a base thread.
 *
 * @param vdo                The VDO to resize
 * @param newPhysicalBlocks  The new physical size in blocks
 *
 * @return VDO_SUCCESS or an error
 **/
int performGrowPhysical(VDO *vdo, BlockCount newPhysicalBlocks);

/**
 * Prepare to resize the VDO, allocating memory as needed.
 *
 * @param vdo                The VDO
 * @param newPhysicalBlocks  The new physical size in blocks
 **/
int prepareToGrowPhysical(VDO *vdo, BlockCount newPhysicalBlocks)
  __attribute__((warn_unused_result));

#endif /* VDO_RESIZE_H */
