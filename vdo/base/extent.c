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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/base/extent.c#1 $
 */

#include "extent.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "constants.h"
#include "logger.h"
#include "physicalLayer.h"
#include "types.h"
#include "vdo.h"
#include "vioRead.h"
#include "vioWrite.h"

/**********************************************************************/
int createExtent(PhysicalLayer  *layer,
                 VIOType         vioType,
                 VIOPriority     priority,
                 BlockCount      blockCount,
                 char           *data,
                 VDOExtent     **extentPtr)
{
  int result = ASSERT(isMetadataVIOType(vioType),
                      "createExtent() called for metadata");
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDOExtent *extent;
  result = ALLOCATE_EXTENDED(VDOExtent, blockCount, VIO *, __func__, &extent);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = initializeEnqueueableCompletion(&extent->completion,
                                           VDO_EXTENT_COMPLETION, layer);

  for (; extent->count < blockCount; extent->count++) {
    result = layer->createMetadataVIO(layer, vioType, priority, extent, data,
                                      &extent->vios[extent->count]);
    if (result != VDO_SUCCESS) {
      freeExtent(&extent);
      return result;
    }

    data += VDO_BLOCK_SIZE;
  }

  *extentPtr = extent;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeExtent(VDOExtent **extentPtr)
{
  VDOExtent *extent = *extentPtr;
  if (extent == NULL) {
    return;
  }

  for (BlockCount i = 0; i < extent->count; i++) {
    freeVIO(&extent->vios[i]);
  }

  destroyEnqueueable(&extent->completion);
  FREE(extent);
  *extentPtr = NULL;
}

/**
 * Launch a metadata extent.
 *
 * @param extent      The extent
 * @param startBlock  The absolute physical block at which the extent should
 *                    begin its I/O
 * @param operation   The operation to perform on the extent
 **/
static void launchMetadataExtent(VDOExtent           *extent,
                                 PhysicalBlockNumber  startBlock,
                                 VIOOperation         operation)
{
  resetCompletion(&extent->completion);
  extent->completeCount = 0;
  BlockCount vioCount = extent->count;
  for (BlockCount i = 0; i < vioCount; i++) {
    VIO *vio = extent->vios[i];
    vio->completion.callbackThreadID = extent->completion.callbackThreadID;
    launchMetadataVIO(vio, startBlock++, handleVIOCompletion,
                      handleVIOCompletion, operation);
  }
}

/**********************************************************************/
void readMetadataExtent(VDOExtent *extent, PhysicalBlockNumber startBlock)
{
  launchMetadataExtent(extent, startBlock, VIO_READ);
}

/**********************************************************************/
void writeMetadataExtent(VDOExtent *extent, PhysicalBlockNumber startBlock)
{
  launchMetadataExtent(extent, startBlock, VIO_WRITE);
}

/**********************************************************************/
void handleVIOCompletion(VDOCompletion *completion)
{
  VDOExtent *extent = asVDOExtent(completion->parent);
  if (++extent->completeCount != extent->count) {
    setCompletionResult(extentAsCompletion(extent), completion->result);
    return;
  }

  finishCompletion(extentAsCompletion(extent), completion->result);
}
