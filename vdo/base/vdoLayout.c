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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoLayout.c#2 $
 */

#include "vdoLayout.h"
#include "vdoLayoutInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "blockMap.h"
#include "partitionCopy.h"
#include "slab.h"
#include "slabSummary.h"
#include "types.h"
#include "vdoInternal.h"

#include "statusCodes.h"

static const PartitionID REQUIRED_PARTITIONS[] = {
  BLOCK_MAP_PARTITION,
  BLOCK_ALLOCATOR_PARTITION,
  RECOVERY_JOURNAL_PARTITION,
  SLAB_SUMMARY_PARTITION,
};

static const uint8_t REQUIRED_PARTITION_COUNT = 4;

/**
 * Make a fixed layout for a VDO.
 *
 * @param [in]  physicalBlocks  The number of physical blocks in the VDO
 * @param [in]  startingOffset  The starting offset of the layout
 * @param [in]  blockMapBlocks  The size of the block map partition
 * @param [in]  journalBlocks   The size of the journal partition
 * @param [in]  summaryBlocks   The size of the slab summary partition
 * @param [out] layoutPtr       A pointer to hold the new FixedLayout
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeVDOFixedLayout(BlockCount            physicalBlocks,
                              PhysicalBlockNumber   startingOffset,
                              BlockCount            blockMapBlocks,
                              BlockCount            journalBlocks,
                              BlockCount            summaryBlocks,
                              FixedLayout         **layoutPtr)
{
  BlockCount necessarySize
    = (startingOffset + blockMapBlocks + journalBlocks + summaryBlocks);
  if (necessarySize > physicalBlocks) {
    return logErrorWithStringError(VDO_NO_SPACE, "Not enough space to"
                                   " make a VDO");
  }

  FixedLayout *layout;
  int result = makeFixedLayout(physicalBlocks - startingOffset,
                               startingOffset, &layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeFixedLayoutPartition(layout, BLOCK_MAP_PARTITION,
                                    blockMapBlocks, FROM_BEGINNING, 0);
  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  result = makeFixedLayoutPartition(layout, SLAB_SUMMARY_PARTITION,
                                    summaryBlocks, FROM_END, 0);
  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  result = makeFixedLayoutPartition(layout, RECOVERY_JOURNAL_PARTITION,
                                    journalBlocks, FROM_END, 0);
  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  /*
   * The block allocator no longer traffics in relative PBNs so the offset
   * doesn't matter. We need to keep this partition around both for upgraded
   * systems, and because we decided that all of the usable space in the
   * volume, other than the super block, should be part of some partition.
   */
  result = makeFixedLayoutPartition(layout, BLOCK_ALLOCATOR_PARTITION,
                                    ALL_FREE_BLOCKS, FROM_BEGINNING,
                                    blockMapBlocks);
  if (result != VDO_SUCCESS) {
    freeFixedLayout(&layout);
    return result;
  }

  *layoutPtr = layout;
  return VDO_SUCCESS;
}

/**
 * Get the offset of a given partition.
 *
 * @param layout       The layout containing the partition
 * @param partitionID  The ID of the partition whose offset is desired
 *
 * @return The offset of the partition (in blocks)
 **/
__attribute__((warn_unused_result))
static BlockCount getPartitionOffset(VDOLayout   *layout,
                                     PartitionID  partitionID)
{
  return getFixedLayoutPartitionOffset(getVDOPartition(layout, partitionID));
}

/**********************************************************************/
int makeVDOLayout(BlockCount            physicalBlocks,
                  PhysicalBlockNumber   startingOffset,
                  BlockCount            blockMapBlocks,
                  BlockCount            journalBlocks,
                  BlockCount            summaryBlocks,
                  VDOLayout           **vdoLayoutPtr)
{
  VDOLayout *vdoLayout;
  int result = ALLOCATE(1, VDOLayout, __func__, &vdoLayout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeVDOFixedLayout(physicalBlocks, startingOffset, blockMapBlocks,
                              journalBlocks, summaryBlocks, &vdoLayout->layout);
  if (result != VDO_SUCCESS) {
    freeVDOLayout(&vdoLayout);
    return result;
  }

  vdoLayout->startingOffset = startingOffset;

  *vdoLayoutPtr = vdoLayout;
  return VDO_SUCCESS;
}

/**********************************************************************/
int decodeVDOLayout(Buffer *buffer, VDOLayout **vdoLayoutPtr)
{
  VDOLayout *vdoLayout;
  int result = ALLOCATE(1, VDOLayout, __func__, &vdoLayout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeFixedLayout(buffer, &vdoLayout->layout);
  if (result != VDO_SUCCESS) {
    freeVDOLayout(&vdoLayout);
    return result;
  }

  // Check that all the expected partitions exist
  Partition *partition;
  for (uint8_t i = 0; i < REQUIRED_PARTITION_COUNT; i++) {
    result = getPartition(vdoLayout->layout, REQUIRED_PARTITIONS[i],
                          &partition);
    if (result != VDO_SUCCESS) {
      freeVDOLayout(&vdoLayout);
      return logErrorWithStringError(result,
                                     "VDO layout is missing required partition"
                                     " %u", REQUIRED_PARTITIONS[i]);
    }
  }

  // XXX Assert this is the same as where we loaded the super block.
  vdoLayout->startingOffset
    = getPartitionOffset(vdoLayout, BLOCK_MAP_PARTITION);

  *vdoLayoutPtr = vdoLayout;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeVDOLayout(VDOLayout **vdoLayoutPtr)
{
  VDOLayout *vdoLayout = *vdoLayoutPtr;
  if (vdoLayout == NULL) {
    return;
  }

  freeCopyCompletion(&vdoLayout->copyCompletion);
  freeFixedLayout(&vdoLayout->nextLayout);
  freeFixedLayout(&vdoLayout->layout);
  freeFixedLayout(&vdoLayout->previousLayout);
  FREE(vdoLayout);
  *vdoLayoutPtr = NULL;
}

/**
 * Get a partition from a FixedLayout in conditions where we expect that it can
 * not fail.
 *
 * @param layout  The FixedLayout from which to get the partition
 * @param id      The ID of the partition to retrieve
 *
 * @return The desired partition
 **/
__attribute__((warn_unused_result))
static Partition *retrievePartition(FixedLayout *layout, PartitionID id)
{
  Partition *partition;
  int result = getPartition(layout, id, &partition);
  ASSERT_LOG_ONLY(result == VDO_SUCCESS, "VDOLayout has expected partition");
  return partition;
}

/**********************************************************************/
Partition *getVDOPartition(VDOLayout *vdoLayout, PartitionID id)
{
  return retrievePartition(vdoLayout->layout, id);
}

/**
 * Get a partition from a VDOLayout's next FixedLayout. This method should
 * only be called when the VDOLayout is prepared to grow.
 *
 * @param vdoLayout  The VDOLayout from which to get the partition
 * @param id         The ID of the desired partition
 *
 * @return The requested partition
 **/
__attribute__((warn_unused_result))
static Partition *getPartitionFromNextLayout(VDOLayout   *vdoLayout,
                                             PartitionID  id)
{
  ASSERT_LOG_ONLY(vdoLayout->nextLayout != NULL,
                  "VDOLayout is prepared to grow");
  return retrievePartition(vdoLayout->nextLayout, id);
}

/**
 * Get the size of a given partition.
 *
 * @param layout       The layout containing the partition
 * @param partitionID  The partition ID whose size to find
 *
 * @return The size of the partition (in blocks)
 **/
__attribute__((warn_unused_result))
static BlockCount getPartitionSize(VDOLayout *layout, PartitionID partitionID)
{
  return getFixedLayoutPartitionSize(getVDOPartition(layout, partitionID));
}

/**********************************************************************/
int prepareToGrowVDOLayout(VDOLayout     *vdoLayout,
                           BlockCount     oldPhysicalBlocks,
                           BlockCount     newPhysicalBlocks,
                           PhysicalLayer *layer)
{
  if (getNextVDOLayoutSize(vdoLayout) == newPhysicalBlocks) {
    // We are already prepared to grow to the new size, so we're done.
    return VDO_SUCCESS;
  }

  // Make a copy completion if there isn't one
  if (vdoLayout->copyCompletion == NULL) {
    int result = makeCopyCompletion(layer, &vdoLayout->copyCompletion);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Free any unused preparation.
  freeFixedLayout(&vdoLayout->nextLayout);

  // Make a new layout with the existing partition sizes for everything but the
  // block allocator partition.
  int result = makeVDOFixedLayout(newPhysicalBlocks,
                                  vdoLayout->startingOffset,
                                  getPartitionSize(vdoLayout,
                                                   BLOCK_MAP_PARTITION),
                                  getPartitionSize(vdoLayout,
                                                   RECOVERY_JOURNAL_PARTITION),
                                  getPartitionSize(vdoLayout,
                                                   SLAB_SUMMARY_PARTITION),
                                  &vdoLayout->nextLayout);
  if (result != VDO_SUCCESS) {
    freeCopyCompletion(&vdoLayout->copyCompletion);
    return result;
  }

  // Ensure the new journal and summary are entirely within the added blocks.
  Partition *slabSummaryPartition
    = getPartitionFromNextLayout(vdoLayout, SLAB_SUMMARY_PARTITION);
  Partition *recoveryJournalPartition
    = getPartitionFromNextLayout(vdoLayout, RECOVERY_JOURNAL_PARTITION);
  BlockCount minNewSize
    = (oldPhysicalBlocks
       + getFixedLayoutPartitionSize(slabSummaryPartition)
       + getFixedLayoutPartitionSize(recoveryJournalPartition));
  if (minNewSize > newPhysicalBlocks) {
    // Copying the journal and summary would destroy some old metadata.
    freeFixedLayout(&vdoLayout->nextLayout);
    freeCopyCompletion(&vdoLayout->copyCompletion);
    return VDO_INCREMENT_TOO_SMALL;
  }

  return VDO_SUCCESS;
}

/**
 * Get the size of a VDO from the specified FixedLayout and the
 * starting offset thereof.
 *
 * @param layout          The fixed layout whose size to use
 * @param startingOffset  The starting offset of the layout
 *
 * @return The total size of a VDO (in blocks) with the given layout
 **/
__attribute__((warn_unused_result))
static BlockCount getVDOSize(FixedLayout *layout, BlockCount startingOffset)
{
  // The FixedLayout does not include the super block or any earlier
  // metadata; all that is captured in the VDOLayout's starting offset
  return getTotalFixedLayoutSize(layout) + startingOffset;
}

/**********************************************************************/
BlockCount getNextVDOLayoutSize(VDOLayout *vdoLayout)
{
  return ((vdoLayout->nextLayout == NULL)
          ? 0 : getVDOSize(vdoLayout->nextLayout, vdoLayout->startingOffset));
}

/**********************************************************************/
BlockCount getNextBlockAllocatorPartitionSize(VDOLayout *vdoLayout)
{
  if (vdoLayout->nextLayout == NULL) {
    return 0;
  }

  Partition *partition = getPartitionFromNextLayout(vdoLayout,
                                                    BLOCK_ALLOCATOR_PARTITION);
  return getFixedLayoutPartitionSize(partition);
}

/**********************************************************************/
BlockCount growVDOLayout(VDOLayout *vdoLayout)
{
  ASSERT_LOG_ONLY(vdoLayout->nextLayout != NULL,
                  "VDO prepared to grow physical");
  vdoLayout->previousLayout = vdoLayout->layout;
  vdoLayout->layout         = vdoLayout->nextLayout;
  vdoLayout->nextLayout     = NULL;

  return getVDOSize(vdoLayout->layout, vdoLayout->startingOffset);
}

/**********************************************************************/
BlockCount revertVDOLayout(VDOLayout *vdoLayout)
{
  if ((vdoLayout->previousLayout != NULL)
      && (vdoLayout->previousLayout != vdoLayout->layout)) {
    // Only revert if there's something to revert to.
    freeFixedLayout(&vdoLayout->layout);
    vdoLayout->layout         = vdoLayout->previousLayout;
    vdoLayout->previousLayout = NULL;
  }

  return getVDOSize(vdoLayout->layout, vdoLayout->startingOffset);
}

/**********************************************************************/
void finishVDOLayoutGrowth(VDOLayout *vdoLayout)
{
  if (vdoLayout->layout != vdoLayout->previousLayout) {
    freeFixedLayout(&vdoLayout->previousLayout);
  }

  if (vdoLayout->layout != vdoLayout->nextLayout) {
    freeFixedLayout(&vdoLayout->nextLayout);
  }

  freeCopyCompletion(&vdoLayout->copyCompletion);
}

/**********************************************************************/
void copyPartition(VDOLayout     *layout,
                   PartitionID    partitionID,
                   VDOCompletion *parent)
{
  copyPartitionAsync(layout->copyCompletion,
                     getVDOPartition(layout, partitionID),
                     getPartitionFromNextLayout(layout, partitionID), parent);
}

/**********************************************************************/
size_t getVDOLayoutEncodedSize(const VDOLayout *vdoLayout)
{
  return getFixedLayoutEncodedSize(vdoLayout->layout);
}

/**********************************************************************/
int encodeVDOLayout(const VDOLayout *vdoLayout, Buffer *buffer)
{
  return encodeFixedLayout(vdoLayout->layout, buffer);
}

