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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoLayout.h#2 $
 */

/**
 * VDOLayout is an object which manages the layout of a VDO. It wraps
 * FixedLayout, but includes the knowledge of exactly which partitions a VDO is
 * expected to have. Because of this knowledge, the VDOLayout validates the
 * FixedLayout encoded in the super block at load time, obviating the need for
 * subsequent error checking when other modules need to get partitions from the
 * layout.
 *
 * The VDOLayout also manages the preparation and growth of the layout for grow
 * physical operations.
 **/

#ifndef VDO_LAYOUT_H
#define VDO_LAYOUT_H

#include "fixedLayout.h"
#include "types.h"

/**
 * Make a VDO layout with the specified parameters.
 *
 * @param [in]  physicalBlocks  The number of physical blocks in the VDO
 * @param [in]  startingOffset  The starting offset of the layout
 * @param [in]  blockMapBlocks  The size of the block map partition
 * @param [in]  journalBlocks   The size of the journal partition
 * @param [in]  summaryBlocks   The size of the slab summary partition
 * @param [out] vdoLayoutPtr    A pointer to hold the new VDOLayout
 *
 * @return VDO_SUCCESS or an error
 **/
int makeVDOLayout(BlockCount            physicalBlocks,
                  PhysicalBlockNumber   startingOffset,
                  BlockCount            blockMapBlocks,
                  BlockCount            journalBlocks,
                  BlockCount            summaryBlocks,
                  VDOLayout           **vdoLayoutPtr)
  __attribute__((warn_unused_result));

/**
 * Decode a VDOLayout from a buffer.
 *
 * @param [in]  buffer        The buffer from which to decode
 * @param [out] vdoLayoutPtr  A pointer to hold the VDOLayout
 *
 * @return VDO_SUCCESS or an error
 **/
int decodeVDOLayout(Buffer *buffer, VDOLayout **vdoLayoutPtr)
  __attribute__((warn_unused_result));

/**
 * Free a VDOLayout and NULL out the reference to it.
 *
 * @param vdoLayoutPtr  The pointer to a VDOLayout to free
 **/
void freeVDOLayout(VDOLayout **vdoLayoutPtr);

/**
 * Get a partition from a VDOLayout. Because the layout's FixedLayout has
 * already been validated, this can not fail.
 *
 * @param vdoLayout  The VDOLayout from which to get the partition
 * @param id         The ID of the desired partition
 *
 * @return The requested partition
 **/
Partition *getVDOPartition(VDOLayout *vdoLayout, PartitionID id)
  __attribute__((warn_unused_result));

/**
 * Prepare the layout to be grown.
 *
 * @param vdoLayout          The layout to grow
 * @param oldPhysicalBlocks  The current size of the VDO
 * @param newPhysicalBlocks  The size to which the VDO will be grown
 * @param layer              The layer being grown
 *
 * @return VDO_SUCCESS or an error code
 **/
int prepareToGrowVDOLayout(VDOLayout     *vdoLayout,
                           BlockCount     oldPhysicalBlocks,
                           BlockCount     newPhysicalBlocks,
                           PhysicalLayer *layer)
  __attribute__((warn_unused_result));

/**
 * Get the size of the next layout.
 *
 * @param vdoLayout  The layout to check
 *
 * @return The size which was specified when the layout was prepared for growth
 *         or 0 if the layout is not prepared to grow
 **/
BlockCount getNextVDOLayoutSize(VDOLayout *vdoLayout)
  __attribute__((warn_unused_result));

/**
 * Get the size of the next block allocator partition.
 *
 * @param vdoLayout  The VDOLayout which has been prepared to grow
 *
 * @return The size of the block allocator partition in the next layout or 0
 *         if the layout is not prepared to grow
 **/
BlockCount getNextBlockAllocatorPartitionSize(VDOLayout *vdoLayout)
  __attribute__((warn_unused_result));

/**
 * Grow the layout by swapping in the prepared layout.
 *
 * @param vdoLayout  The layout to grow
 *
 * @return The new size of the VDO
 **/
BlockCount growVDOLayout(VDOLayout *vdoLayout)
  __attribute__((warn_unused_result));

/**
 * Revert the last growth attempt.
 *
 * @param vdoLayout  The layout to revert
 *
 * @return The reverted size (in blocks) of the VDO
 **/
BlockCount revertVDOLayout(VDOLayout *vdoLayout)
  __attribute__((warn_unused_result));

/**
 * Clean up any unused resources once an attempt to grow has completed.
 *
 * @param vdoLayout  The layout
 **/
void finishVDOLayoutGrowth(VDOLayout *vdoLayout);

/**
 * Copy a partition from the location specified in the current layout to that in
 * the next layout.
 *
 * @param layout       The VDOLayout which is prepared to grow
 * @param partitionID  The ID of the partition to copy
 * @param parent       The completion to notify when the copy is complete
 **/
void copyPartition(VDOLayout     *layout,
                   PartitionID    partitionID,
                   VDOCompletion *parent);

/**
 * Get the size of an encoded VDOLayout.
 *
 * @param vdoLayout  The VDOLayout
 *
 * @return The encoded size of the VDOLayout
 **/
size_t getVDOLayoutEncodedSize(const VDOLayout *vdoLayout)
  __attribute__((warn_unused_result));

/**
 * Encode a VDOLayout into a buffer.
 *
 * @param vdoLayout  The VDOLayout to encode
 * @param buffer     The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeVDOLayout(const VDOLayout *vdoLayout, Buffer *buffer)
  __attribute__((warn_unused_result));

#endif // VDO_LAYOUT_H
