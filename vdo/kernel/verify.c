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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/verify.c#3 $
 */

#include "verify.h"

#include "logger.h"

#include "dataKVIO.h"
#include "numeric.h"

/**
 * Compare blocks of memory for equality.
 *
 * This assumes the blocks are likely to be large; it's not well
 * optimized for comparing just a few bytes.  This is desirable
 * because the Linux kernel memcmp() routine on x86 is not well
 * optimized for large blocks, and the performance penalty turns out
 * to be significant if you're doing lots of 4KB comparisons.
 *
 * @param pointerArgument1  first data block
 * @param pointerArgument2  second data block
 * @param length            length of the data block
 *
 * @return   true iff the two blocks are equal
 **/
__attribute__((warn_unused_result))
static bool memoryEqual(void   *pointerArgument1,
                        void   *pointerArgument2,
                        size_t  length)
{
  byte *pointer1 = pointerArgument1;
  byte *pointer2 = pointerArgument2;
  while (length >= sizeof(uint64_t)) {
    /*
     * GET_UNALIGNED is just for paranoia.  (1) On x86_64 it is
     * treated the same as an aligned access.  (2) In this use case,
     * one or both of the inputs will almost(?) always be aligned.
     */
    if (GET_UNALIGNED(uint64_t, pointer1)
        != GET_UNALIGNED(uint64_t, pointer2)) {
      return false;
    }
    pointer1 += sizeof(uint64_t);
    pointer2 += sizeof(uint64_t);
    length -= sizeof(uint64_t);
  }
  while (length > 0) {
    if (*pointer1 != *pointer2) {
      return false;
    }
    pointer1++;
    pointer2++;
    length--;
  }
  return true;
}

/**
 * Verify the Albireo-provided deduplication advice, and invoke a
 * callback once the answer is available.
 *
 * After we've compared the stored data with the data to be written,
 * or after we've failed to be able to do so, the stored VIO callback
 * is queued to be run in the main (kvdoReqQ) thread.
 *
 * If the advice turns out to be stale and the deduplication session
 * is still active, submit a correction.  (Currently the correction
 * must be sent before the callback can be invoked, if the dedupe
 * session is still live.)
 *
 * @param item  The workitem from the queue
 **/
static void verifyDuplicationWork(KvdoWorkItem *item)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(item);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION("$F;j=dedupe;cb=verify"));

  if (likely(memoryEqual(dataKVIO->dataBlock, dataKVIO->readBlock.data,
                         VDO_BLOCK_SIZE))) {
    // Leave dataKVIO->dataVIO.isDuplicate set to true.
  } else {
    dataKVIO->dataVIO.isDuplicate = false;
  }

  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**
 * Verify the Albireo-provided deduplication advice, and invoke a
 * callback once the answer is available.
 *
 * @param dataKVIO  The DataKVIO that we are looking to dedupe.
 **/
static void verifyReadBlockCallback(DataKVIO *dataKVIO)
{
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  int err = dataKVIO->readBlock.status;
  if (unlikely(err != 0)) {
    logDebug("%s: err %d", __func__, err);
    dataKVIO->dataVIO.isDuplicate = false;
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  launchDataKVIOOnCPUQueue(dataKVIO, verifyDuplicationWork, NULL,
                           CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**********************************************************************/
void kvdoVerifyDuplication(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(dataVIO->isDuplicate, "advice to verify must be valid");
  ASSERT_LOG_ONLY(dataVIO->duplicate.state != MAPPING_STATE_UNMAPPED,
                  "advice to verify must not be a discard");
  ASSERT_LOG_ONLY(dataVIO->duplicate.pbn != ZERO_BLOCK,
                  "advice to verify must not point to the zero block");
  ASSERT_LOG_ONLY(!dataVIO->isZeroBlock,
                  "zeroed block should not have advice to verify");

  TraceLocation location
    = THIS_LOCATION("verifyDuplication;dup=update(verify);io=verify");
  dataVIOAddTraceRecord(dataVIO, location);
  kvdoReadBlock(dataVIO, dataVIO->duplicate.pbn, dataVIO->duplicate.state,
                BIO_Q_ACTION_VERIFY, verifyReadBlockCallback);
}

/**********************************************************************/
bool kvdoCompareDataVIOs(DataVIO *first, DataVIO *second)
{
  dataVIOAddTraceRecord(second, THIS_LOCATION(NULL));
  DataKVIO *a = dataVIOAsDataKVIO(first);
  DataKVIO *b = dataVIOAsDataKVIO(second);
  return memoryEqual(a->dataBlock, b->dataBlock, VDO_BLOCK_SIZE);
}
