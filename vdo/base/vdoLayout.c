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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLayout.c#7 $
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
 * @param [out] layoutPtr       A pointer to hold the new fixed_layout
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeVDOFixedLayout(BlockCount            physicalBlocks,
                              PhysicalBlockNumber   startingOffset,
                              BlockCount            blockMapBlocks,
                              BlockCount            journalBlocks,
                              BlockCount            summaryBlocks,
                              struct fixed_layout **layoutPtr)
{
  BlockCount necessarySize
    = (startingOffset + blockMapBlocks + journalBlocks + summaryBlocks);
  if (necessarySize > physicalBlocks) {
    return logErrorWithStringError(VDO_NO_SPACE, "Not enough space to"
                                   " make a VDO");
  }

  struct fixed_layout *layout;
  int result = make_fixed_layout(physicalBlocks - startingOffset,
                                 startingOffset, &layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = make_fixed_layout_partition(layout, BLOCK_MAP_PARTITION,
                                       blockMapBlocks, FROM_BEGINNING, 0);
  if (result != VDO_SUCCESS) {
    free_fixed_layout(&layout);
    return result;
  }

  result = make_fixed_layout_partition(layout, SLAB_SUMMARY_PARTITION,
                                       summaryBlocks, FROM_END, 0);
  if (result != VDO_SUCCESS) {
    free_fixed_layout(&layout);
    return result;
  }

  result = make_fixed_layout_partition(layout, RECOVERY_JOURNAL_PARTITION,
                                       journalBlocks, FROM_END, 0);
  if (result != VDO_SUCCESS) {
    free_fixed_layout(&layout);
    return result;
  }

  /*
   * The block allocator no longer traffics in relative PBNs so the offset
   * doesn't matter. We need to keep this partition around both for upgraded
   * systems, and because we decided that all of the usable space in the
   * volume, other than the super block, should be part of some partition.
   */
  result = make_fixed_layout_partition(layout, BLOCK_ALLOCATOR_PARTITION,
                                       ALL_FREE_BLOCKS, FROM_BEGINNING,
                                       blockMapBlocks);
  if (result != VDO_SUCCESS) {
    free_fixed_layout(&layout);
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
static BlockCount getPartitionOffset(struct vdo_layout *layout,
                                     PartitionID        partitionID)
{
  return get_fixed_layout_partition_offset(getVDOPartition(layout, partitionID));
}

/**********************************************************************/
int makeVDOLayout(BlockCount            physicalBlocks,
                  PhysicalBlockNumber   startingOffset,
                  BlockCount            blockMapBlocks,
                  BlockCount            journalBlocks,
                  BlockCount            summaryBlocks,
                  struct vdo_layout   **vdoLayoutPtr)
{
  struct vdo_layout *vdoLayout;
  int result = ALLOCATE(1, struct vdo_layout, __func__, &vdoLayout);
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
int decodeVDOLayout(Buffer *buffer, struct vdo_layout **vdoLayoutPtr)
{
  struct vdo_layout *vdoLayout;
  int result = ALLOCATE(1, struct vdo_layout, __func__, &vdoLayout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decode_fixed_layout(buffer, &vdoLayout->layout);
  if (result != VDO_SUCCESS) {
    freeVDOLayout(&vdoLayout);
    return result;
  }

  // Check that all the expected partitions exist
  struct partition *partition;
  uint8_t i;
  for (i = 0; i < REQUIRED_PARTITION_COUNT; i++) {
    result = get_partition(vdoLayout->layout, REQUIRED_PARTITIONS[i],
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
void freeVDOLayout(struct vdo_layout **vdoLayoutPtr)
{
  struct vdo_layout *vdoLayout = *vdoLayoutPtr;
  if (vdoLayout == NULL) {
    return;
  }

  free_copy_completion(&vdoLayout->copyCompletion);
  free_fixed_layout(&vdoLayout->nextLayout);
  free_fixed_layout(&vdoLayout->layout);
  free_fixed_layout(&vdoLayout->previousLayout);
  FREE(vdoLayout);
  *vdoLayoutPtr = NULL;
}

/**
 * Get a partition from a fixed_layout in conditions where we expect that it can
 * not fail.
 *
 * @param layout  The fixed_layout from which to get the partition
 * @param id      The ID of the partition to retrieve
 *
 * @return The desired partition
 **/
__attribute__((warn_unused_result))
static struct partition *retrievePartition(struct fixed_layout *layout,
                                           PartitionID          id)
{
  struct partition *partition;
  int result = get_partition(layout, id, &partition);
  ASSERT_LOG_ONLY(result == VDO_SUCCESS, "vdo_layout has expected partition");
  return partition;
}

/**********************************************************************/
struct partition *getVDOPartition(struct vdo_layout *vdoLayout, PartitionID id)
{
  return retrievePartition(vdoLayout->layout, id);
}

/**
 * Get a partition from a vdo_layout's next fixed_layout. This method should
 * only be called when the vdo_layout is prepared to grow.
 *
 * @param vdoLayout  The vdo_layout from which to get the partition
 * @param id         The ID of the desired partition
 *
 * @return The requested partition
 **/
__attribute__((warn_unused_result))
static struct partition *
getPartitionFromNextLayout(struct vdo_layout *vdoLayout,
                           PartitionID        id)
{
  ASSERT_LOG_ONLY(vdoLayout->nextLayout != NULL,
                  "vdo_layout is prepared to grow");
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
static BlockCount getPartitionSize(struct vdo_layout *layout,
                                   PartitionID        partitionID)
{
  return get_fixed_layout_partition_size(getVDOPartition(layout, partitionID));
}

/**********************************************************************/
int prepareToGrowVDOLayout(struct vdo_layout *vdoLayout,
                           BlockCount         oldPhysicalBlocks,
                           BlockCount         newPhysicalBlocks,
                           PhysicalLayer     *layer)
{
  if (getNextVDOLayoutSize(vdoLayout) == newPhysicalBlocks) {
    // We are already prepared to grow to the new size, so we're done.
    return VDO_SUCCESS;
  }

  // Make a copy completion if there isn't one
  if (vdoLayout->copyCompletion == NULL) {
    int result = make_copy_completion(layer, &vdoLayout->copyCompletion);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Free any unused preparation.
  free_fixed_layout(&vdoLayout->nextLayout);

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
    free_copy_completion(&vdoLayout->copyCompletion);
    return result;
  }

  // Ensure the new journal and summary are entirely within the added blocks.
  struct partition *slabSummaryPartition
    = getPartitionFromNextLayout(vdoLayout, SLAB_SUMMARY_PARTITION);
  struct partition *recoveryJournalPartition
    = getPartitionFromNextLayout(vdoLayout, RECOVERY_JOURNAL_PARTITION);
  BlockCount minNewSize
    = (oldPhysicalBlocks
       + get_fixed_layout_partition_size(slabSummaryPartition)
       + get_fixed_layout_partition_size(recoveryJournalPartition));
  if (minNewSize > newPhysicalBlocks) {
    // Copying the journal and summary would destroy some old metadata.
    free_fixed_layout(&vdoLayout->nextLayout);
    free_copy_completion(&vdoLayout->copyCompletion);
    return VDO_INCREMENT_TOO_SMALL;
  }

  return VDO_SUCCESS;
}

/**
 * Get the size of a VDO from the specified fixed_layout and the
 * starting offset thereof.
 *
 * @param layout          The fixed layout whose size to use
 * @param startingOffset  The starting offset of the layout
 *
 * @return The total size of a VDO (in blocks) with the given layout
 **/
__attribute__((warn_unused_result))
static BlockCount getVDOSize(struct fixed_layout *layout,
                             BlockCount           startingOffset)
{
  // The fixed_layout does not include the super block or any earlier
  // metadata; all that is captured in the vdo_layout's starting offset
  return get_total_fixed_layout_size(layout) + startingOffset;
}

/**********************************************************************/
BlockCount getNextVDOLayoutSize(struct vdo_layout *vdoLayout)
{
  return ((vdoLayout->nextLayout == NULL)
          ? 0 : getVDOSize(vdoLayout->nextLayout, vdoLayout->startingOffset));
}

/**********************************************************************/
BlockCount getNextBlockAllocatorPartitionSize(struct vdo_layout *vdoLayout)
{
  if (vdoLayout->nextLayout == NULL) {
    return 0;
  }

  struct partition *partition
    = getPartitionFromNextLayout(vdoLayout, BLOCK_ALLOCATOR_PARTITION);
  return get_fixed_layout_partition_size(partition);
}

/**********************************************************************/
BlockCount growVDOLayout(struct vdo_layout *vdoLayout)
{
  ASSERT_LOG_ONLY(vdoLayout->nextLayout != NULL,
                  "VDO prepared to grow physical");
  vdoLayout->previousLayout = vdoLayout->layout;
  vdoLayout->layout         = vdoLayout->nextLayout;
  vdoLayout->nextLayout     = NULL;

  return getVDOSize(vdoLayout->layout, vdoLayout->startingOffset);
}

/**********************************************************************/
BlockCount revertVDOLayout(struct vdo_layout *vdoLayout)
{
  if ((vdoLayout->previousLayout != NULL)
      && (vdoLayout->previousLayout != vdoLayout->layout)) {
    // Only revert if there's something to revert to.
    free_fixed_layout(&vdoLayout->layout);
    vdoLayout->layout         = vdoLayout->previousLayout;
    vdoLayout->previousLayout = NULL;
  }

  return getVDOSize(vdoLayout->layout, vdoLayout->startingOffset);
}

/**********************************************************************/
void finishVDOLayoutGrowth(struct vdo_layout *vdoLayout)
{
  if (vdoLayout->layout != vdoLayout->previousLayout) {
    free_fixed_layout(&vdoLayout->previousLayout);
  }

  if (vdoLayout->layout != vdoLayout->nextLayout) {
    free_fixed_layout(&vdoLayout->nextLayout);
  }

  free_copy_completion(&vdoLayout->copyCompletion);
}

/**********************************************************************/
void copyPartition(struct vdo_layout     *layout,
                   PartitionID            partitionID,
                   struct vdo_completion *parent)
{
  copy_partition_async(layout->copyCompletion,
                       getVDOPartition(layout, partitionID),
                       getPartitionFromNextLayout(layout, partitionID), parent);
}

/**********************************************************************/
size_t getVDOLayoutEncodedSize(const struct vdo_layout *vdoLayout)
{
  return get_fixed_layout_encoded_size(vdoLayout->layout);
}

/**********************************************************************/
int encodeVDOLayout(const struct vdo_layout *vdoLayout, Buffer *buffer)
{
  return encode_fixed_layout(vdoLayout->layout, buffer);
}

