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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/dataKVIO.c#9 $
 */

#include "dataKVIO.h"


#include "logger.h"
#include "memoryAlloc.h"

#include "dataVIO.h"
#include "hashLock.h"
#include "lz4.h"
#include "murmur/MurmurHash3.h"

#include "bio.h"
#include "dedupeIndex.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "readCache.h"
#include "vdoCommon.h"
#include "verify.h"

static void dumpPooledDataKVIO(void *poolData, void *data);

enum {
  WRITE_PROTECT_FREE_POOL = 0,
  WP_DATA_KVIO_SIZE       = (sizeof(DataKVIO) + PAGE_SIZE - 1
                             - ((sizeof(DataKVIO) + PAGE_SIZE - 1)
                                % PAGE_SIZE))
};

/**
 * Alter the write-access permission to a page of memory, so that
 * objects in the free pool may no longer be modified.
 *
 * To do: Deny read access as well.
 *
 * @param address    The starting address to protect, which must be on a
 *                   page boundary
 * @param byteCount  The number of bytes to protect, which must be a multiple
 *                   of the page size
 * @param mode       The write protection mode (true means read-only)
 **/
static __always_inline void
setWriteProtect(void   *address,
                size_t  byteCount,
                bool    mode __attribute__((unused)))
{
  BUG_ON((((long) address) % PAGE_SIZE) != 0);
  BUG_ON((byteCount % PAGE_SIZE) != 0);
  BUG(); // only works in internal code, sorry
}

/**********************************************************************/
static void maybeLogDataKVIOTrace(DataKVIO *dataKVIO)
{
  if (dataKVIO->kvio.layer->traceLogging) {
    logKvioTrace(&dataKVIO->kvio);
  }
}

/**
 * First tracing hook for VIO completion.
 *
 * If the SystemTap script vdotrace.stp is in use, it does stage 1 of
 * its processing here. We must not call addTraceRecord between the
 * two tap functions.
 *
 * @param dataKVIO  The VIO we're finishing up
 **/
static void kvioCompletionTap1(DataKVIO *dataKVIO)
{
  /*
   * Ensure that dataKVIO doesn't get optimized out, even under inline
   * expansion. Also, make sure the compiler has to emit debug info
   * for baseTraceLocation, which some of our SystemTap scripts will
   * use here.
   */
  __asm__ __volatile__("nop" : : "g" (dataKVIO), "g" (baseTraceLocation));
}

/**
 * Second tracing hook for VIO completion.
 *
 * The SystemTap script vdotrace.stp splits its VIO-completion work
 * into two stages, to reduce lock contention for script variables.
 * Hence, it needs two hooks in the code.
 *
 * @param dataKVIO  The VIO we're finishing up
 **/
static void kvioCompletionTap2(DataKVIO *dataKVIO)
{
  // Hack to ensure variable doesn't get optimized out.
  __asm__ __volatile__("nop" : : "g" (dataKVIO));
}

/**********************************************************************/
static void kvdoAcknowledgeDataKVIO(DataKVIO *dataKVIO)
{
  KernelLayer       *layer             = dataKVIO->kvio.layer;
  ExternalIORequest *externalIORequest = &dataKVIO->externalIORequest;
  BIO               *bio               = externalIORequest->bio;
  if (bio == NULL) {
    return;
  }

  externalIORequest->bio = NULL;

  int error
    = mapToSystemError(dataVIOAsCompletion(&dataKVIO->dataVIO)->result);
  bio->bi_end_io  = externalIORequest->endIO;
  bio->bi_private = externalIORequest->private;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
  bio->bi_opf     = externalIORequest->rw;
#else
  bio->bi_rw      = externalIORequest->rw;
#endif

  countBios(&layer->biosAcknowledged, bio);
  if (getBioSize(bio) < VDO_BLOCK_SIZE) {
    countBios(&layer->biosAcknowledgedPartial, bio);
  }


  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  completeBio(bio, error);
}

/**********************************************************************/
static noinline void cleanDataKVIO(DataKVIO *dataKVIO, FreeBufferPointers *fbp)
{
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  kvdoAcknowledgeDataKVIO(dataKVIO);

  KVIO *kvio = dataKVIOAsKVIO(dataKVIO);
  kvio->bio  = NULL;

  runReadCacheReleaseBlock(kvio->layer, &dataKVIO->readBlock);
  if (unlikely(kvio->vio->trace != NULL)) {
    maybeLogDataKVIOTrace(dataKVIO);
    kvioCompletionTap1(dataKVIO);
    kvioCompletionTap2(dataKVIO);
    freeTraceToPool(kvio->layer, kvio->vio->trace);
  }

  addFreeBufferPointer(fbp, dataKVIO);
}

/**********************************************************************/
void returnDataKVIOBatchToPool(BatchProcessor *batch, void *closure)
{
  KernelLayer *layer = closure;
  uint32_t     count = 0;
  ASSERT_LOG_ONLY(batch != NULL, "batch not null");
  ASSERT_LOG_ONLY(layer != NULL, "layer not null");

  FreeBufferPointers fbp;
  initFreeBufferPointers(&fbp, layer->dataKVIOPool);

  KvdoWorkItem *item;
  while ((item = nextBatchItem(batch)) != NULL) {
    cleanDataKVIO(workItemAsDataKVIO(item), &fbp);
    condReschedBatchProcessor(batch);
    count++;
  }

  if (fbp.index > 0) {
    freeBufferPointers(&fbp);
  }

  completeManyRequests(layer, count);
}

/**********************************************************************/
static void kvdoAcknowledgeThenCompleteDataKVIO(KvdoWorkItem *item)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(item);
  kvdoAcknowledgeDataKVIO(dataKVIO);
  addToBatchProcessor(dataKVIO->kvio.layer->dataKVIOReleaser, item);
}

/**********************************************************************/
void kvdoCompleteDataKVIO(VDOCompletion *completion)
{
  DataKVIO *dataKVIO = dataVIOAsDataKVIO(asDataVIO(completion));
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  KernelLayer *layer = getLayerFromDataKVIO(dataKVIO);
  if (useBioAckQueue(layer) && USE_BIO_ACK_QUEUE_FOR_READ
      && (dataKVIO->externalIORequest.bio != NULL)) {
    launchDataKVIOOnBIOAckQueue(dataKVIO, kvdoAcknowledgeThenCompleteDataKVIO,
                                NULL, BIO_ACK_Q_ACTION_ACK);
  } else {
    addToBatchProcessor(layer->dataKVIOReleaser,
                        workItemFromDataKVIO(dataKVIO));
  }
}

/**
 * Copy the uncompressed data from a compressed block read into the user
 * bio which requested the read.
 *
 * @param workItem  The DataKVIO which requested the read
 **/
static void copyReadBlockData(KvdoWorkItem *workItem)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(workItem);

  // For a read-modify-write, copy the data into the dataBlock buffer so it
  // will be set up for the write phase.
  if (isReadModifyWriteVIO(dataKVIO->kvio.vio)) {
    bioCopyDataOut(getBIOFromDataKVIO(dataKVIO), dataKVIO->readBlock.data);
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  // For a partial read, the callback will copy the requested data from the
  // read block.
  if (dataKVIO->isPartial) {
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  // For a full block read, copy the data to the bio and acknowledge.
  bioCopyDataOut(getBIOFromDataKVIO(dataKVIO), dataKVIO->readBlock.data);
  kvdoAcknowledgeDataVIO(&dataKVIO->dataVIO);
}

/**
 * Finish reading data for a compressed block.
 *
 * @param dataKVIO  The DataKVIO which requested the read
 **/
static void readDataKVIOReadBlockCallback(DataKVIO *dataKVIO)
{
  if (dataKVIO->readBlock.status != VDO_SUCCESS) {
    setCompletionResult(dataVIOAsCompletion(&dataKVIO->dataVIO),
                        dataKVIO->readBlock.status);
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  launchDataKVIOOnCPUQueue(dataKVIO, copyReadBlockData, NULL,
                           CPU_Q_ACTION_COMPRESS_BLOCK);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Complete and reset a bio that was supplied by the user and then used for a
 * read (so that we can complete it with the user's callback).
 *
 * @param bio   The bio to complete
 **/
static void resetUserBio(BIO *bio)
#else
/**
 * Complete and reset a bio that was supplied by the user and then used for a
 * read (so that we can complete it with the user's callback).
 *
 * @param bio   The bio to complete
 * @param error Possible error from underlying block device
 **/
static void resetUserBio(BIO *bio, int error)
#endif
{
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)) \
     && (LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)))
  // This is a user bio, and the device just called bio_endio() on it, so
  // we need to re-increment bi_remaining so we too can call bio_endio().
  atomic_inc(&bio->bi_remaining);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  completeAsyncBio(bio);
#else
  completeAsyncBio(bio, error);
#endif
}

/**********************************************************************/
void kvdoReadDataVIO(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(!isWriteVIO(dataVIOAsVIO(dataVIO)),
                  "operation set correctly for data read");
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F;io=readData"));

  if (isCompressed(dataVIO->mapped.state)) {
    kvdoReadBlock(dataVIO, dataVIO->mapped.pbn, dataVIO->mapped.state,
                  READ_COMPRESSED_DATA, readDataKVIOReadBlockCallback);
    return;
  }

  KVIO *kvio = dataVIOAsKVIO(dataVIO);
  BIO  *bio  = kvio->bio;
  bio->bi_end_io = resetUserBio;
  setBioSector(bio, blockToSector(kvio->layer, dataVIO->mapped.pbn));
  submitBio(bio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
static void kvdoAcknowledgeDataKVIOThenContinue(KvdoWorkItem *item)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(item);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  kvdoAcknowledgeDataKVIO(dataKVIO);
  // Even if we're not using bio-ack threads, we may be in the wrong
  // base-code thread.
  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**********************************************************************/
void kvdoAcknowledgeDataVIO(DataVIO *dataVIO)
{
  DataKVIO    *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  KernelLayer *layer    = getLayerFromDataKVIO(dataKVIO);

  // If the remaining discard work is not completely processed by this VIO,
  // don't acknowledge it yet.
  if (isDiscardBio(dataKVIO->externalIORequest.bio)
      && (dataKVIO->remainingDiscard
          > (VDO_BLOCK_SIZE - dataKVIO->offset))) {
    invokeCallback(dataVIOAsCompletion(dataVIO));
    return;
  }

  // We've finished with the KVIO; acknowledge completion of the bio to the
  // kernel.
  if (useBioAckQueue(layer)) {
    dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
    launchDataKVIOOnBIOAckQueue(dataKVIO, kvdoAcknowledgeDataKVIOThenContinue,
                                NULL, BIO_ACK_Q_ACTION_ACK);
  } else {
    kvdoAcknowledgeDataKVIOThenContinue(workItemFromDataKVIO(dataKVIO));
  }
}

/**********************************************************************/
void kvdoWriteDataVIO(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(isWriteVIO(dataVIOAsVIO(dataVIO)),
                  "kvdoWriteDataVIO() called on write DataVIO");
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F;io=writeData;j=normal"));

  KVIO *kvio  = dataVIOAsKVIO(dataVIO);
  BIO  *bio   = kvio->bio;
  setBioOperationWrite(bio);
  setBioSector(bio, blockToSector(kvio->layer, dataVIO->newMapped.pbn));
  invalidateCacheAndSubmitBio(kvio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
void kvdoModifyWriteDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  DataKVIO    *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  BIO         *bio      = dataKVIO->externalIORequest.bio;
  KernelLayer *layer    = getLayerFromDataKVIO(dataKVIO);
  resetBio(dataKVIO->dataBlockBio, layer);

  if (!isDiscardBio(bio)) {
    bioCopyDataIn(bio, dataKVIO->dataBlock + dataKVIO->offset);
  } else {
    memset(dataKVIO->dataBlock + dataKVIO->offset, '\0',
           min(dataKVIO->remainingDiscard,
               (DiscardSize) (VDO_BLOCK_SIZE - dataKVIO->offset)));
  }

  dataVIO->isZeroBlock               = bioIsZeroData(dataKVIO->dataBlockBio);
  dataKVIO->dataBlockBio->bi_private = &dataKVIO->kvio;
  copyBioOperationAndFlags(dataKVIO->dataBlockBio, bio);
  // Make the bio a write, not (potentially) a discard.
  setBioOperationWrite(dataKVIO->dataBlockBio);
}

/**********************************************************************/
void kvdoZeroDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("zeroDataVIO;io=readData"));
  bioZeroData(dataVIOAsKVIO(dataVIO)->bio);
}

/**********************************************************************/
void kvdoCopyDataVIO(DataVIO *source, DataVIO *destination)
{
  dataVIOAddTraceRecord(destination, THIS_LOCATION(NULL));
  bioCopyDataOut(dataVIOAsKVIO(destination)->bio,
                 dataVIOAsDataKVIO(source)->dataBlock);
}

/**********************************************************************/
static void kvdoCompressWork(KvdoWorkItem *item)
{
  DataKVIO    *dataKVIO = workItemAsDataKVIO(item);
  KernelLayer *layer    = getLayerFromDataKVIO(dataKVIO);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  char *context = getWorkQueuePrivateData();
  if (unlikely(context == NULL)) {
    uint32_t index = atomicAdd32(&layer->compressionContextIndex, 1) - 1;
    BUG_ON(index >= layer->deviceConfig->threadCounts.cpuThreads);
    context = layer->compressionContext[index];
    setWorkQueuePrivateData(context);
  }

  int size = LZ4_compress_ctx_limitedOutput(context, dataKVIO->dataBlock,
                                            dataKVIO->scratchBlock,
                                            VDO_BLOCK_SIZE,
                                            VDO_BLOCK_SIZE);
  DataVIO *dataVIO = &dataKVIO->dataVIO;
  if (size > 0) {
    // The scratch block will be used to contain the compressed data.
    dataVIO->compression.data = dataKVIO->scratchBlock;
    dataVIO->compression.size = size;
  } else {
    // Use block size plus one as an indicator for uncompressible data.
    dataVIO->compression.size = VDO_BLOCK_SIZE + 1;
  }

  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**********************************************************************/
void kvdoCompressDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO,
                        THIS_LOCATION("compressDataVIO;"
                                      "io=compress;cb=compress"));

  /*
   * If the orignal bio was a discard, but we got this far because the discard
   * was a partial one (r/m/w), and it is part of a larger discard, we cannot
   * compress this VIO. We need to make sure the VIO completes ASAP.
   */
  DataKVIO *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  if (isDiscardBio(dataKVIO->externalIORequest.bio)
      && (dataKVIO->remainingDiscard > 0)) {
    dataVIO->compression.size = VDO_BLOCK_SIZE + 1;
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  launchDataKVIOOnCPUQueue(dataKVIO, kvdoCompressWork, NULL,
                           CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Construct a DataKVIO.
 *
 * @param [in]  layer        The physical layer
 * @param [in]  bio          The bio to associate with this DataKVIO
 * @param [out] dataKVIOPtr  A pointer to hold the new DataKVIO
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeDataKVIO(KernelLayer *layer, BIO *bio, DataKVIO **dataKVIOPtr)
{
  DataKVIO *dataKVIO;
  int result = allocBufferFromPool(layer->dataKVIOPool, (void **) &dataKVIO);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result, "data kvio allocation failure");
  }

  if (WRITE_PROTECT_FREE_POOL) {
    setWriteProtect(dataKVIO, WP_DATA_KVIO_SIZE, false);
  }

  KVIO *kvio = &dataKVIO->kvio;
  kvio->vio = dataVIOAsVIO(&dataKVIO->dataVIO);
  memset(&kvio->enqueueable, 0, sizeof(KvdoEnqueueable));
  memset(&dataKVIO->dedupeContext.pendingList, 0, sizeof(struct list_head));
  memset(&dataKVIO->dataVIO, 0, sizeof(DataVIO));
  kvio->bioToSubmit = NULL;
  bio_list_init(&kvio->biosMerged);

  // The dataBlock is only needed for writes and some partial reads.
  if (isWriteBio(bio) || (getBioSize(bio) < VDO_BLOCK_SIZE)) {
    resetBio(dataKVIO->dataBlockBio, layer);
  }

  initializeKVIO(kvio, layer, VIO_TYPE_DATA, VIO_PRIORITY_DATA, NULL, bio);
  *dataKVIOPtr = dataKVIO;
  return VDO_SUCCESS;
}

/**
 * Creates a new DataVIO structure. A DataVIO represents a single logical
 * block of data. It is what most VDO operations work with. This function also
 * creates a wrapping DataKVIO structure that is used when we want to
 * physically read or write the data associated with the DataVIO.
 *
 * @param [in]  layer        The physical layer
 * @param [in]  bio          The BIO from the request the new DataKVIO will
 *                           service
 * @param [in]  arrivalTime  The arrival time of the BIO
 * @param [out] dataKVIOPtr  A pointer to hold the new DataKVIO
 *
 * @return VDO_SUCCESS or an error
 **/
static int kvdoCreateKVIOFromBio(KernelLayer  *layer,
                                 BIO          *bio,
                                 Jiffies       arrivalTime,
                                 DataKVIO    **dataKVIOPtr)
{
  ExternalIORequest externalIORequest = {
    .bio         = bio,
    .private     = bio->bi_private,
    .endIO       = bio->bi_end_io,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0)
    .rw          = bio->bi_opf,
#else
    .rw          = bio->bi_rw,
#endif
  };

  // We will handle FUA at the end of the request (after we restore the
  // bi_rw field from externalIORequest.rw).
  clearBioOperationFlagFua(bio);

  DataKVIO *dataKVIO = NULL;
  int       result   = makeDataKVIO(layer, bio, &dataKVIO);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dataKVIO->externalIORequest = externalIORequest;
  dataKVIO->offset = sectorToBlockOffset(layer, getBioSector(bio));
  dataKVIO->isPartial = ((getBioSize(bio) < VDO_BLOCK_SIZE)
                         || (dataKVIO->offset != 0));

  DataVIO *dataVIO = &dataKVIO->dataVIO;
  if (!dataKVIO->isPartial) {
    /*
     * Note that we unconditionally fill in the dataBlock array for
     * non-read operations. There are places like kvdoCopyVIO that may
     * look at kvio->dataBlock for a zero block (and maybe for
     * discards?). We could skip filling in dataBlock for such cases,
     * but only once we're sure all such places are fixed to check the
     * isZeroBlock flag first.
     */
    if (isDiscardBio(bio)) {
      /*
       * This is a discard/trim operation. This is treated much like the zero
       * block, but we keep different stats and distinguish it in the block
       * map.
       */
      memset(dataKVIO->dataBlock, 0, VDO_BLOCK_SIZE);
    } else if (bio_data_dir(bio) == WRITE) {
      dataVIO->isZeroBlock = bioIsZeroData(bio);
      // Copy the bio data to a char array so that we can continue to use
      // the data after we acknowledge the bio.
      bioCopyDataIn(bio, dataKVIO->dataBlock);
    }
  }

  if (dataKVIO->isPartial || isWriteBio(bio)) {
    /*
     * dataKVIO->bio will point at kvio->dataBlockBio for all writes and
     * partial block I/O so the rest of the kernel code doesn't need to
     * make a decision as to what to use.
     */
    dataKVIO->dataBlockBio->bi_private = &dataKVIO->kvio;
    if (dataKVIO->isPartial && isWriteBio(bio)) {
      clearBioOperationAndFlags(dataKVIO->dataBlockBio);
      setBioOperationRead(dataKVIO->dataBlockBio);
    } else {
      copyBioOperationAndFlags(dataKVIO->dataBlockBio, bio);
    }
    dataKVIOAsKVIO(dataKVIO)->bio = dataKVIO->dataBlockBio;
    dataKVIO->readBlock.data      = dataKVIO->dataBlock;
  }

  setBioBlockDevice(bio, layer->dev->bdev);
  bio->bi_end_io = completeAsyncBio;
  *dataKVIOPtr   = dataKVIO;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void launchDataKVIOWork(KvdoWorkItem *item)
{
  runCallback(vioAsCompletion(workItemAsKVIO(item)->vio));
}

/**
 * Continue discard processing for requests that span multiple physical blocks.
 * If all have been processed the KVIO is completed.  If we have already seen
 * an error, we skip the rest of the discard and fail immediately.
 *
 * <p>Invoked in a request-queue thread after the discard of a block has
 * completed.
 *
 * @param completion  A completion representing the discard KVIO
 **/
static void kvdoContinueDiscardKVIO(VDOCompletion *completion)
{
  DataVIO     *dataVIO  = asDataVIO(completion);
  DataKVIO    *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  KernelLayer *layer    = getLayerFromDataKVIO(dataKVIO);
  dataKVIO->remainingDiscard
    -= min(dataKVIO->remainingDiscard,
           (DiscardSize) (VDO_BLOCK_SIZE - dataKVIO->offset));
  if ((completion->result != VDO_SUCCESS)
      || (dataKVIO->remainingDiscard == 0)) {
    if (dataKVIO->hasDiscardPermit) {
      limiterRelease(&layer->discardLimiter);
      dataKVIO->hasDiscardPermit = false;
    }
    kvdoCompleteDataKVIO(completion);
    return;
  }

  BIO *bio = getBIOFromDataKVIO(dataKVIO);
  resetBio(bio, layer);
  dataKVIO->isPartial = (dataKVIO->remainingDiscard < VDO_BLOCK_SIZE);
  dataKVIO->offset    = 0;

  VIOOperation operation;
  if (dataKVIO->isPartial) {
    operation  = VIO_READ_MODIFY_WRITE;
    setBioOperationRead(bio);
  } else {
    operation  = VIO_WRITE;
  }

  if (requestorSetFUA(dataKVIO)) {
    operation |= VIO_FLUSH_AFTER;
  }

  prepareDataVIO(dataVIO, dataVIO->logical.lbn + 1, operation,
                 !dataKVIO->isPartial, kvdoContinueDiscardKVIO);
  enqueueDataKVIO(dataKVIO, launchDataKVIOWork, completion->callback,
                  REQ_Q_ACTION_MAP_BIO);
}

/**
 * Finish a partial read.
 *
 * @param completion  The partial read KVIO
 **/
static void kvdoCompletePartialRead(VDOCompletion *completion)
{
  DataKVIO *dataKVIO = dataVIOAsDataKVIO(asDataVIO(completion));
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  bioCopyDataOut(dataKVIO->externalIORequest.bio,
                 dataKVIO->readBlock.data + dataKVIO->offset);
  kvdoCompleteDataKVIO(completion);
  return;
}

/**********************************************************************/
int kvdoLaunchDataKVIOFromBio(KernelLayer *layer,
                              BIO         *bio,
                              uint64_t     arrivalTime,
                              bool         hasDiscardPermit)
{
  if (getBioSize(bio) < VDO_BLOCK_SIZE) {
    countBios(&layer->biosInPartial, bio);
  }


  DataKVIO *dataKVIO = NULL;
  int result = kvdoCreateKVIOFromBio(layer, bio, arrivalTime, &dataKVIO);
  if (unlikely(result != VDO_SUCCESS)) {
    logInfo("%s: KVIO allocation failure", __func__);
    if (hasDiscardPermit) {
      limiterRelease(&layer->discardLimiter);
    }
    limiterRelease(&layer->requestLimiter);
    return mapToSystemError(result);
  }

  /*
   * Discards behave very differently than other requests when coming
   * in from device-mapper. We have to be able to handle any size discards
   * and with various sector offsets within a block.
   */
  KVIO         *kvio      = &dataKVIO->kvio;
  VDOAction    *callback  = kvdoCompleteDataKVIO;
  VIOOperation  operation = VIO_WRITE;
  bool          isTrim    = false;
  if (isDiscardBio(bio)) {
    dataKVIO->hasDiscardPermit = hasDiscardPermit;
    dataKVIO->remainingDiscard = getBioSize(bio);
    callback                   = kvdoContinueDiscardKVIO;
    if (dataKVIO->isPartial) {
      operation = VIO_READ_MODIFY_WRITE;
    } else {
      isTrim = true;
    }
  } else if (dataKVIO->isPartial) {
    if (bio_data_dir(bio) == READ) {
      callback  = kvdoCompletePartialRead;
      operation = VIO_READ;
    } else {
      operation = VIO_READ_MODIFY_WRITE;
    }
  } else if (bio_data_dir(bio) == READ) {
    operation = VIO_READ;
  }

  if (requestorSetFUA(dataKVIO)) {
    operation |= VIO_FLUSH_AFTER;
  }

  LogicalBlockNumber lbn
    = sectorToBlock(layer, getBioSector(bio) - layer->startingSectorOffset);
  prepareDataVIO(&dataKVIO->dataVIO, lbn, operation, isTrim, callback);
  enqueueKVIO(kvio, launchDataKVIOWork, vioAsCompletion(kvio->vio)->callback,
              REQ_Q_ACTION_MAP_BIO);
  return VDO_SUCCESS;
}

/**
 * Hash a DataKVIO and set its chunk name.
 *
 * @param item  The DataKVIO to be hashed
 **/
static void kvdoHashDataWork(KvdoWorkItem *item)
{
  DataKVIO *dataKVIO = workItemAsDataKVIO(item);
  DataVIO  *dataVIO  = &dataKVIO->dataVIO;
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));

  if (sizeof(dataVIO->chunkName) == 32) {
    MurmurHash3_x64_128_double(dataKVIO->dataBlock, VDO_BLOCK_SIZE,
                               0x62ea60be, 0x3eeb36cd, &dataVIO->chunkName);
  } else {
    MurmurHash3_x64_128(dataKVIO->dataBlock, VDO_BLOCK_SIZE, 0x62ea60be,
                        &dataVIO->chunkName);
  }
  dataKVIO->dedupeContext.chunkName = &dataVIO->chunkName;

  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**********************************************************************/
void kvdoHashDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  launchDataKVIOOnCPUQueue(dataVIOAsDataKVIO(dataVIO), kvdoHashDataWork, NULL,
                           CPU_Q_ACTION_HASH_BLOCK);
}

/**********************************************************************/
void kvdoCheckForDuplication(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO,
                        THIS_LOCATION("checkForDuplication;dup=post"));
  ASSERT_LOG_ONLY(!dataVIO->isZeroBlock,
                  "zero block not checked for duplication");
  ASSERT_LOG_ONLY(dataVIO->newMapped.state != MAPPING_STATE_UNMAPPED,
                  "discard not checked for duplication");

  DataKVIO *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  if (hasAllocation(dataVIO)) {
    postDedupeAdvice(dataKVIO);
  } else {
    // This block has not actually been written (presumably because we are
    // full), so attempt to dedupe without posting bogus advice.
    queryDedupeAdvice(dataKVIO);
  }
}

/**
 * Update Albireo metadata for compressed DataVIOs.
 *
 * @param dataVIO  The DataVIO to update albireo metadata for
 **/
void kvdoUpdateDedupeAdvice(DataVIO *dataVIO)
{
  updateDedupeAdvice(dataVIOAsDataKVIO(dataVIO));
}

/**
 * Implements BufferFreeFunction.
 **/
static void freePooledDataKVIO(void *poolData, void *data)
{
  if (data == NULL) {
    return;
  }

  DataKVIO    *dataKVIO = (DataKVIO *) data;
  KernelLayer *layer    = (KernelLayer *) poolData;
  if (WRITE_PROTECT_FREE_POOL) {
    setWriteProtect(dataKVIO, WP_DATA_KVIO_SIZE, false);
  }

  if (dataKVIO->dataBlockBio != NULL) {
    freeBio(dataKVIO->dataBlockBio, layer);
  }

  if (dataKVIO->readBlock.bio != NULL) {
    freeBio(dataKVIO->readBlock.bio, layer);
  }

  FREE(dataKVIO->readBlock.buffer);
  FREE(dataKVIO->dataBlock);
  FREE(dataKVIO->scratchBlock);
  FREE(dataKVIO);
}

/**
 * Allocate a DataKVIO. This function is the internals of makePooledDataKVIO().
 *
 * @param [in]  layer        The layer in which the DataKVIO will operate
 * @param [out] dataKVIOPtr  A pointer to hold the newly allocated DataKVIO
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocatePooledDataKVIO(KernelLayer *layer, DataKVIO **dataKVIOPtr)
{
  DataKVIO *dataKVIO;
  int result;
  if (WRITE_PROTECT_FREE_POOL) {
    STATIC_ASSERT(WP_DATA_KVIO_SIZE >= sizeof(DataKVIO));
    result = allocateMemory(WP_DATA_KVIO_SIZE, 0, __func__, &dataKVIO);
    if (result == VDO_SUCCESS) {
      BUG_ON((((size_t) dataKVIO) & (PAGE_SIZE - 1)) != 0);
    }
  } else {
    result = ALLOCATE(1, DataKVIO, __func__, &dataKVIO);
  }

  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result, "DataKVIO allocation failure");
  }

  STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
  result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio data",
                          &dataKVIO->dataBlock);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result, "DataKVIO data allocation failure");
  }

  result = createBio(layer, dataKVIO->dataBlock, &dataKVIO->dataBlockBio);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result,
                                   "DataKVIO data bio allocation failure");
  }

  if (layer->readCacheBlocks == 0) {
    result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio read buffer",
                            &dataKVIO->readBlock.buffer);
    if (result != VDO_SUCCESS) {
      return logErrorWithStringError(result,
                                     "DataKVIO read allocation failure");
    }

    result = createBio(layer, dataKVIO->readBlock.buffer,
                       &dataKVIO->readBlock.bio);
    if (result != VDO_SUCCESS) {
      return logErrorWithStringError(result,
                                     "DataKVIO read bio allocation failure");
    }

    dataKVIO->readBlock.bio->bi_private = &dataKVIO->kvio;
  }

  result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio scratch",
                          &dataKVIO->scratchBlock);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result,
                                   "DataKVIO scratch allocation failure");
  }

  *dataKVIOPtr = dataKVIO;
  return VDO_SUCCESS;
}

/**
 * Implements BufferAllocateFunction.
 **/
static int makePooledDataKVIO(void *poolData, void **dataPtr)
{
  DataKVIO *dataKVIO = NULL;
  int result = allocatePooledDataKVIO((KernelLayer *) poolData, &dataKVIO);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(poolData, dataKVIO);
    return result;
  }

  *dataPtr = dataKVIO;
  return VDO_SUCCESS;
}

/**
 * Dump out the waiters on each DataVIO in the DataVIO buffer pool.
 *
 * @param queue   The queue to check (logical or physical)
 * @param waitOn  The label to print for queue (logical or physical)
 **/
static void dumpVIOWaiters(WaitQueue *queue, char *waitOn)
{
  Waiter *first = getFirstWaiter(queue);
  if (first == NULL) {
    return;
  }

  DataVIO *dataVIO = waiterAsDataVIO(first);
  logInfo("      %s is locked. Waited on by: VIO %p pbn %" PRIu64 " lbn %"
          PRIu64 " d-pbn %" PRIu64 " lastOp %s",
          waitOn, dataVIO, getDataVIOAllocation(dataVIO),
          dataVIO->logical.lbn, dataVIO->duplicate.pbn,
          getOperationName(dataVIO));

  for (Waiter *waiter = first->nextWaiter;
       waiter != first;
       waiter = waiter->nextWaiter) {
    dataVIO = waiterAsDataVIO(waiter);
    logInfo("     ... and : VIO %p pbn %" PRIu64 " lbn %"
            PRIu64 " d-pbn %" PRIu64 " lastOp %s",
            dataVIO, getDataVIOAllocation(dataVIO), dataVIO->logical.lbn,
            dataVIO->duplicate.pbn, getOperationName(dataVIO));
  }
}

/**
 * Encode various attributes of a VIO as a string of one-character flags for
 * dump logging. This encoding is for logging brevity:
 *
 * R => VIO completion result not VDO_SUCCESS
 * W => VIO is on a wait queue
 * D => VIO is a duplicate
 *
 * <p>The common case of no flags set will result in an empty, null-terminated
 * buffer. If any flags are encoded, the first character in the string will be
 * a space character.
 *
 * @param dataVIO  The VIO to encode
 * @param buffer   The buffer to receive a null-terminated string of encoded
 *                 flag character
 **/
static void encodeVIODumpFlags(DataVIO *dataVIO, char buffer[8])
{
  char *pFlag = buffer;
  *pFlag++ = ' ';
  if (dataVIOAsCompletion(dataVIO)->result != VDO_SUCCESS) {
    *pFlag++ = 'R';
  }
  if (dataVIOAsAllocatingVIO(dataVIO)->waiter.nextWaiter != NULL) {
    *pFlag++ = 'W';
  }
  if (dataVIO->isDuplicate) {
    *pFlag++ = 'D';
  }
  if (pFlag == &buffer[1]) {
    // No flags, so remove the blank space.
    pFlag = buffer;
  }
  *pFlag = '\0';
}

/**
 * Dump out info on a DataKVIO from the DataKVIO pool.
 *
 * <p>Implements BufferDumpFunction.
 *
 * @param poolData  The pool data
 * @param data      The DataKVIO to dump
 **/
static void dumpPooledDataKVIO(void *poolData __attribute__((unused)),
                               void *data)
{
  DataKVIO *dataKVIO = (DataKVIO *) data;
  DataVIO  *dataVIO  = &dataKVIO->dataVIO;

  /*
   * This just needs to be big enough to hold a queue (thread) name
   * and a function name (plus a separator character and NUL). The
   * latter is limited only by taste.
   *
   * In making this static, we're assuming only one "dump" will run at
   * a time. If more than one does run, the log output will be garbled
   * anyway.
   */
  static char vioWorkItemDumpBuffer[100 + MAX_QUEUE_NAME_LEN];
  /*
   * We're likely to be logging a couple thousand of these lines, and
   * in some circumstances syslogd may have trouble keeping up, so
   * keep it BRIEF rather than user-friendly.
   */
  dumpWorkItemToBuffer(&dataKVIO->kvio.enqueueable.workItem,
                       vioWorkItemDumpBuffer, sizeof(vioWorkItemDumpBuffer));
  // Another static buffer...
  // log10(256) = 2.408+, round up:
  enum { DECIMAL_DIGITS_PER_UINT64_T = (int) (1 + 2.41 * sizeof(uint64_t)) };
  static char vioBlockNumberDumpBuffer[sizeof("P L D")
                                       + 3 * DECIMAL_DIGITS_PER_UINT64_T];
  if (dataVIO->isDuplicate) {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "P%" PRIu64 " L%" PRIu64 " D%" PRIu64,
             getDataVIOAllocation(dataVIO), dataVIO->logical.lbn,
             dataVIO->duplicate.pbn);
  } else if (hasAllocation(dataVIO)) {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "P%" PRIu64 " L%" PRIu64,
             getDataVIOAllocation(dataVIO), dataVIO->logical.lbn);
  } else {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "L%" PRIu64,
             dataVIO->logical.lbn);
  }

  static char vioFlushGenerationBuffer[sizeof(" FG")
                                       + DECIMAL_DIGITS_PER_UINT64_T] = "";
  if (dataVIO->flushGeneration != 0) {
    snprintf(vioFlushGenerationBuffer, sizeof(vioFlushGenerationBuffer),
             " FG%" PRIu64, dataVIO->flushGeneration);
  }

  // Encode VIO attributes as a string of one-character flags, usually empty.
  static char flagsDumpBuffer[8];
  encodeVIODumpFlags(dataVIO, flagsDumpBuffer);

  logInfo("  kvio %p %s%s %s %s%s",
          dataKVIO, vioBlockNumberDumpBuffer, vioFlushGenerationBuffer,
          getOperationName(dataVIO), vioWorkItemDumpBuffer, flagsDumpBuffer);
  // might want info on: wantAlbireoAnswer / operation / status
  // might want info on: bio / bioToSubmit / biosMerged

  dumpVIOWaiters(&dataVIO->logical.waiters, "lbn");

  // might want to dump more info from VIO here
}

/**********************************************************************/
int makeDataKVIOBufferPool(KernelLayer  *layer,
                           uint32_t      poolSize,
                           BufferPool  **bufferPoolPtr)
{
  return makeBufferPool("DataKVIO Pool", poolSize,
                        makePooledDataKVIO, freePooledDataKVIO,
                        dumpPooledDataKVIO, layer, bufferPoolPtr);
}

/**********************************************************************/
DataLocation getDedupeAdvice(const DedupeContext *context)
{
  DataKVIO *dataKVIO = container_of(context, DataKVIO, dedupeContext);
  return (DataLocation) {
    .state = dataKVIO->dataVIO.newMapped.state,
    .pbn   = dataKVIO->dataVIO.newMapped.pbn,
  };
}

/**********************************************************************/
void setDedupeAdvice(DedupeContext *context, const DataLocation *advice)
{
  DataKVIO *dataKVIO = container_of(context, DataKVIO, dedupeContext);
  receiveDedupeAdvice(&dataKVIO->dataVIO, advice);
}
