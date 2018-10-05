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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/partitionCopy.c#2 $
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
typedef struct {
  /** completion header */
  VDOCompletion        completion;
  /** the source partition to copy from */
  Partition           *source;
  /** the target partition to copy to */
  Partition           *target;
  /** the current in-partition PBN the copy is beginning at */
  PhysicalBlockNumber  currentIndex;
  /** the last block to copy */
  PhysicalBlockNumber  endingIndex;
  /** the backing data used by the extent */
  char                *data;
  /** the extent being used to copy */
  VDOExtent           *extent;
} CopyCompletion;

/**
 * Convert a VDOCompletion to a CopyCompletion.
 *
 * @param completion The completion to convert
 *
 * @return the completion as a CopyCompletion
 **/
__attribute__((warn_unused_result))
static inline
CopyCompletion *asCopyCompletion(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(CopyCompletion, completion) == 0);
  assertCompletionType(completion->type, PARTITION_COPY_COMPLETION);
  return (CopyCompletion *) completion;
}

/**********************************************************************/
int makeCopyCompletion(PhysicalLayer *layer, VDOCompletion **completionPtr)
{
  CopyCompletion *copy;
  int result = ALLOCATE(1, CopyCompletion, __func__, &copy);
  if (result != VDO_SUCCESS) {
    return result;
  }
  initializeCompletion(&copy->completion, PARTITION_COPY_COMPLETION, layer);

  result = ALLOCATE((VDO_BLOCK_SIZE * STRIDE_LENGTH), char,
                    "partition copy extent", &copy->data);
  if (result != VDO_SUCCESS) {
    VDOCompletion *completion = &copy->completion;
    freeCopyCompletion(&completion);
    return result;
  }

  result = createExtent(layer, VIO_TYPE_PARTITION_COPY, VIO_PRIORITY_HIGH,
                        STRIDE_LENGTH, copy->data, &copy->extent);
  if (result != VDO_SUCCESS) {
    VDOCompletion *completion = &copy->completion;
    freeCopyCompletion(&completion);
    return result;
  }

  *completionPtr = &copy->completion;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeCopyCompletion(VDOCompletion **completionPtr)
{
  if (*completionPtr == NULL) {
    return;
  }

  CopyCompletion *copy = asCopyCompletion(*completionPtr);
  freeExtent(&copy->extent);
  FREE(copy->data);
  FREE(copy);
  *completionPtr = NULL;
}

/**********************************************************************/
static void copyPartitionStride(CopyCompletion *copy);

/**
 * Determine the number of blocks to copy in the current stride.
 *
 * @param copy  The copy completion
 *
 * @return The number of blocks to copy in the current stride
 **/
static inline BlockCount getStrideSize(CopyCompletion *copy)
{
  return minBlockCount(STRIDE_LENGTH, copy->endingIndex - copy->currentIndex);
}

/**
 * Process a completed write during a partition copy.
 *
 * @param completion  The extent which has just completed writing
 **/
static void completeWriteForCopy(VDOCompletion *completion)
{
  CopyCompletion *copy = asCopyCompletion(completion->parent);
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
static void completeReadForCopy(VDOCompletion *completion)
{
  CopyCompletion *copy = asCopyCompletion(completion->parent);
  PhysicalBlockNumber layerStartBlock;
  int result = translateToPBN(copy->target, copy->currentIndex,
                              &layerStartBlock);
  if (result != VDO_SUCCESS) {
    finishCompletion(completion->parent, result);
    return;
  }

  completion->callback = completeWriteForCopy;
  writePartialMetadataExtent(asVDOExtent(completion), layerStartBlock,
                             getStrideSize(copy));
}

/**
 * Copy a stride from one partition to the new partition.
 *
 * @param copy  The CopyCompletion
 **/
static void copyPartitionStride(CopyCompletion *copy)
{
  PhysicalBlockNumber layerStartBlock;
  int result = translateToPBN(copy->source, copy->currentIndex,
                              &layerStartBlock);
  if (result != VDO_SUCCESS) {
    finishCompletion(&copy->completion, result);
    return;
  }

  prepareCompletion(&copy->extent->completion, completeReadForCopy,
                    finishParentCallback, copy->completion.callbackThreadID,
                    &copy->completion);
  readPartialMetadataExtent(copy->extent, layerStartBlock,
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
static int validatePartitionCopy(Partition *source, Partition *target)
{
  BlockCount sourceSize = getFixedLayoutPartitionSize(source);
  BlockCount targetSize = getFixedLayoutPartitionSize(target);

  PhysicalBlockNumber sourceStart = getFixedLayoutPartitionOffset(source);
  PhysicalBlockNumber sourceEnd   = sourceStart + sourceSize;
  PhysicalBlockNumber targetStart = getFixedLayoutPartitionOffset(target);
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
void copyPartitionAsync(VDOCompletion *completion,
                        Partition     *source,
                        Partition     *target,
                        VDOCompletion *parent)
{
  int result = validatePartitionCopy(source, target);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  CopyCompletion *copy = asCopyCompletion(completion);
  prepareToFinishParent(&copy->completion, parent);
  copy->source       = source;
  copy->target       = target;
  copy->currentIndex = 0;
  copy->endingIndex  = getFixedLayoutPartitionSize(source);
  copyPartitionStride(copy);
}
