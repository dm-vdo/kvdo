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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioRead.c#9 $
 */

#include "vioRead.h"

#include "logger.h"

#include "blockMap.h"
#include "dataVIO.h"
#include "vdoInternal.h"
#include "vioWrite.h"

/**
 * Do the modify-write part of a read-modify-write cycle. This callback is
 * registered in readBlock().
 *
 * @param completion  The data_vio which has just finished its read
 **/
static void modifyForPartialWrite(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);

  if (completion->result != VDO_SUCCESS) {
    completeDataVIO(completion);
    return;
  }

  applyPartialWrite(dataVIO);
  struct vio *vio = dataVIOAsVIO(dataVIO);
  vio->operation = VIO_WRITE | (vio->operation & ~VIO_READ_WRITE_MASK);
  dataVIO->isPartialWrite  = true;
  launchWriteDataVIO(dataVIO);
}

/**
 * Read a block asynchronously. This is the callback registered in
 * readBlockMapping().
 *
 * @param completion  The data_vio to read
 **/
static void readBlock(struct vdo_completion *completion)
{
  if (completion->result != VDO_SUCCESS) {
    completeDataVIO(completion);
    return;
  }

  struct data_vio *dataVIO = asDataVIO(completion);
  struct vio      *vio     = asVIO(completion);
  completion->callback
    = (isReadVIO(vio) ? completeDataVIO : modifyForPartialWrite);

  if (dataVIO->mapped.pbn == ZERO_BLOCK) {
    zeroDataVIO(dataVIO);
    invoke_callback(completion);
    return;
  }

  vio->physical = dataVIO->mapped.pbn;
  dataVIO->lastAsyncOperation = READ_DATA;
  readDataVIO(dataVIO);
}

/**
 * Read the data_vio's mapping from the block map. This callback is registered
 * in launchReadDataVIO().
 *
 * @param completion  The data_vio to be read
 **/
static void readBlockMapping(struct vdo_completion *completion)
{
  if (completion->result != VDO_SUCCESS) {
    completeDataVIO(completion);
    return;
  }

  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  setLogicalCallback(dataVIO, readBlock, THIS_LOCATION("$F;cb=readBlock"));
  dataVIO->lastAsyncOperation = GET_MAPPED_BLOCK;
  get_mapped_block_async(dataVIO);
}

/**********************************************************************/
void launchReadDataVIO(struct data_vio *dataVIO)
{
  assertInLogicalZone(dataVIO);
  dataVIO->lastAsyncOperation = FIND_BLOCK_MAP_SLOT;
  // Go find the block map slot for the LBN mapping.
  find_block_map_slot_async(dataVIO, readBlockMapping,
                            get_logical_zone_thread_id(dataVIO->logical.zone));
}

/**
 * Release the logical block lock which a read data_vio obtained now that it
 * is done.
 *
 * @param completion  The data_vio
 **/
static void releaseLogicalLock(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = asDataVIO(completion);
  assertInLogicalZone(dataVIO);
  releaseLogicalBlockLock(dataVIO);
  vioDoneCallback(completion);
}

/**
 * Clean up a data_vio which has finished processing a read.
 *
 * @param dataVIO  The data_vio to clean up
 **/
void cleanupReadDataVIO(struct data_vio *dataVIO)
{
  launchLogicalCallback(dataVIO, releaseLogicalLock,
                        THIS_LOCATION("$F;cb=releaseLL"));
}
