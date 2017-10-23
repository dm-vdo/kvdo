/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/partitionCopy.c#1 $
 */

#include "partitionCopy.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "extent.h"

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

/**
 * Free the copy completion now that we've finished copying.
 *
 * @param completion  The copy completion
 **/
static void finishCopy(VDOCompletion *completion)
{
  VDOCompletion *parent = completion->parent;
  int            result = completion->result;

  CopyCompletion *copy = asCopyCompletion(completion);
  freeExtent(&copy->extent);
  FREE(copy->data);
  FREE(copy);

  finishCompletion(parent, result);
}

/**********************************************************************/
static void copyPartitionStride(CopyCompletion *copy);

/**
 * Process a completed write during a partition copy.
 *
 * @param completion  The extent which has just completed writing
 **/
static void completeWriteForCopy(VDOCompletion *completion)
{
  CopyCompletion *copy = asCopyCompletion(completion->parent);
  copy->currentIndex += copy->extent->count;
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
  writeMetadataExtent(asVDOExtent(completion), layerStartBlock);
}

/**
 * Copy a stride from one partition to the new partition.
 *
 * @param copy  The CopyCompletion
 **/
static void copyPartitionStride(CopyCompletion *copy)
{
  PhysicalBlockNumber blocksRemaining
    = (copy->endingIndex - copy->currentIndex);
  if (blocksRemaining < STRIDE_LENGTH) {
    // There is less than a whole stride left to copy, so the extent must be
    // resized down to the remaining length.
    freeExtent(&copy->extent);
    int result = createExtent(copy->completion.layer, VIO_TYPE_PARTITION_COPY,
                              VIO_PRIORITY_HIGH, blocksRemaining, copy->data,
                              &copy->extent);
    if (result != VDO_SUCCESS) {
      finishCompletion(&copy->completion, result);
      return;
    }
  }

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
  readMetadataExtent(copy->extent, layerStartBlock);
}

/**
 * Initialize a copy completion.
 *
 * @param layer        The layer in question
 * @param source       The partition to copy from
 * @param target       The partition to copy to
 * @param parent       The parent to finish when the copy is complete
 * @param copy         The copy completion to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int initializeCopyCompletion(PhysicalLayer  *layer,
                                    Partition      *source,
                                    Partition      *target,
                                    VDOCompletion  *parent,
                                    CopyCompletion *copy)
{
  initializeCompletion(&copy->completion, PARTITION_COPY_COMPLETION, layer);
  prepareCompletion(&copy->completion, finishCopy, finishCopy,
                    parent->callbackThreadID, parent);

  int result = ALLOCATE((VDO_BLOCK_SIZE * STRIDE_LENGTH), char,
                        "partition copy extent", &copy->data);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = createExtent(layer, VIO_TYPE_PARTITION_COPY, VIO_PRIORITY_HIGH,
                        STRIDE_LENGTH, copy->data, &copy->extent);
  if (result != VDO_SUCCESS) {
    return result;
  }

  copy->source       = source;
  copy->target       = target;
  copy->currentIndex = 0;
  copy->endingIndex  = getFixedLayoutPartitionSize(source);
  return VDO_SUCCESS;
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
void copyPartitionAsync(PhysicalLayer *layer,
                        Partition     *source,
                        Partition     *target,
                        VDOCompletion *parent)
{
  int result = validatePartitionCopy(source, target);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  CopyCompletion *copy;
  result = ALLOCATE(1, CopyCompletion, __func__, &copy);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  result = initializeCopyCompletion(layer, source, target, parent, copy);
  if (result != VDO_SUCCESS) {
    finishCompletion(&copy->completion, result);
    return;
  }

  copyPartitionStride(copy);
}
