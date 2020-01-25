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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/partitionCopy.c#7 $
 */

#include "partitionCopy.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"
#include "numUtils.h"

enum {
  STRIDE_LENGTH = 2048
};

/**
 * A partition copy completion.
 **/
struct copy_completion {
  /** completion header */
  struct vdo_completion  completion;
  /** the source partition to copy from */
  struct partition      *source;
  /** the target partition to copy to */
  struct partition      *target;
  /** the current in-partition PBN the copy is beginning at */
  PhysicalBlockNumber    currentIndex;
  /** the last block to copy */
  PhysicalBlockNumber    endingIndex;
  /** the backing data used by the extent */
  char                  *data;
  /** the extent being used to copy */
  struct vdo_extent     *extent;
};

/**
 * Convert a vdo_completion to a copy_completion.
 *
 * @param completion The completion to convert
 *
 * @return the completion as a copy_completion
 **/
__attribute__((warn_unused_result))
static inline
struct copy_completion *asCopyCompletion(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct copy_completion, completion) == 0);
  assertCompletionType(completion->type, PARTITION_COPY_COMPLETION);
  return (struct copy_completion *) completion;
}

/**********************************************************************/
int makeCopyCompletion(PhysicalLayer          *layer,
                       struct vdo_completion **completionPtr)
{
  struct copy_completion *copy;
  int result = ALLOCATE(1, struct copy_completion, __func__, &copy);
  if (result != VDO_SUCCESS) {
    return result;
  }
  initializeCompletion(&copy->completion, PARTITION_COPY_COMPLETION, layer);

  result = ALLOCATE((VDO_BLOCK_SIZE * STRIDE_LENGTH), char,
                    "partition copy extent", &copy->data);
  if (result != VDO_SUCCESS) {
    struct vdo_completion *completion = &copy->completion;
    freeCopyCompletion(&completion);
    return result;
  }

  result = create_extent(layer, VIO_TYPE_PARTITION_COPY, VIO_PRIORITY_HIGH,
                         STRIDE_LENGTH, copy->data, &copy->extent);
  if (result != VDO_SUCCESS) {
    struct vdo_completion *completion = &copy->completion;
    freeCopyCompletion(&completion);
    return result;
  }

  *completionPtr = &copy->completion;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeCopyCompletion(struct vdo_completion **completionPtr)
{
  if (*completionPtr == NULL) {
    return;
  }

  struct copy_completion *copy = asCopyCompletion(*completionPtr);
  free_extent(&copy->extent);
  FREE(copy->data);
  FREE(copy);
  *completionPtr = NULL;
}

/**********************************************************************/
static void copyPartitionStride(struct copy_completion *copy);

/**
 * Determine the number of blocks to copy in the current stride.
 *
 * @param copy  The copy completion
 *
 * @return The number of blocks to copy in the current stride
 **/
static inline BlockCount getStrideSize(struct copy_completion *copy)
{
  return minBlockCount(STRIDE_LENGTH, copy->endingIndex - copy->currentIndex);
}

/**
 * Process a completed write during a partition copy.
 *
 * @param completion  The extent which has just completed writing
 **/
static void completeWriteForCopy(struct vdo_completion *completion)
{
  struct copy_completion *copy = asCopyCompletion(completion->parent);
  copy->currentIndex += getStrideSize(copy);
  if (copy->currentIndex >= copy->endingIndex) {
    // We're done.
    finishCompletion(completion->parent, VDO_SUCCESS);
    return;
  }
  copyPartitionStride(copy);
}

/**
 * Process a completed read during a partition copy, and launch the
 * corresponding write to the new partition.
 *
 * @param completion  The extent which has just completed reading
 **/
static void completeReadForCopy(struct vdo_completion *completion)
{
  struct copy_completion *copy = asCopyCompletion(completion->parent);
  PhysicalBlockNumber layerStartBlock;
  int result = translate_to_pbn(copy->target, copy->currentIndex,
                                &layerStartBlock);
  if (result != VDO_SUCCESS) {
    finishCompletion(completion->parent, result);
    return;
  }

  completion->callback = completeWriteForCopy;
  write_partial_metadata_extent(as_vdo_extent(completion), layerStartBlock,
                                getStrideSize(copy));
}

/**
 * Copy a stride from one partition to the new partition.
 *
 * @param copy  The copy_completion
 **/
static void copyPartitionStride(struct copy_completion *copy)
{
  PhysicalBlockNumber layerStartBlock;
  int result = translate_to_pbn(copy->source, copy->currentIndex,
                                &layerStartBlock);
  if (result != VDO_SUCCESS) {
    finishCompletion(&copy->completion, result);
    return;
  }

  prepareCompletion(&copy->extent->completion, completeReadForCopy,
                    finishParentCallback, copy->completion.callbackThreadID,
                    &copy->completion);
  read_partial_metadata_extent(copy->extent, layerStartBlock,
                               getStrideSize(copy));
}

/**
 * Verify that the source can be copied to the target safely.
 *
 * @param source        The source partition
 * @param target        The target partition
 *
 * @return VDO_SUCCESS or an error code
 **/
static int validatePartitionCopy(struct partition *source,
                                 struct partition *target)
{
  BlockCount sourceSize = get_fixed_layout_partition_size(source);
  BlockCount targetSize = get_fixed_layout_partition_size(target);

  PhysicalBlockNumber sourceStart = get_fixed_layout_partition_offset(source);
  PhysicalBlockNumber sourceEnd   = sourceStart + sourceSize;
  PhysicalBlockNumber targetStart = get_fixed_layout_partition_offset(target);
  PhysicalBlockNumber targetEnd   = targetStart + targetSize;

  int result = ASSERT(sourceSize <= targetSize,
                      "target partition must be not smaller than source"
                      " partition");
  if (result != UDS_SUCCESS) {
    return result;
  }

  return ASSERT(((sourceEnd <= targetStart) || (targetEnd <= sourceStart)),
                "target partition must not overlap source partition");
}

/**********************************************************************/
void copyPartitionAsync(struct vdo_completion *completion,
                        struct partition      *source,
                        struct partition      *target,
                        struct vdo_completion *parent)
{
  int result = validatePartitionCopy(source, target);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  struct copy_completion *copy = asCopyCompletion(completion);
  prepareToFinishParent(&copy->completion, parent);
  copy->source       = source;
  copy->target       = target;
  copy->currentIndex = 0;
  copy->endingIndex  = get_fixed_layout_partition_size(source);
  copyPartitionStride(copy);
}
