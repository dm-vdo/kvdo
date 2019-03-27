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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dataKVIO.c#31 $
 */

#include "dataKVIO.h"

#include <asm/unaligned.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "compressedBlock.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "lz4.h"
#include "physicalLayer.h"

#include "bio.h"
#include "dedupeIndex.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "ioSubmitter.h"
#include "vdoCommon.h"

static void dumpPooledDataKVIO(void *poolData, void *data);

enum {
  WRITE_PROTECT_FREE_POOL = 0,
  WP_DATA_KVIO_SIZE       = (sizeof(struct data_kvio) + PAGE_SIZE - 1
                             - ((sizeof(struct data_kvio) + PAGE_SIZE - 1)
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
static void maybeLogDataKVIOTrace(struct data_kvio *dataKVIO)
{
  if (dataKVIO->kvio.layer->traceLogging) {
    log_kvio_trace(&dataKVIO->kvio);
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
static void kvioCompletionTap1(struct data_kvio *dataKVIO)
{
  /*
   * Ensure that dataKVIO doesn't get optimized out, even under inline
   * expansion. Also, make sure the compiler has to emit debug info
   * for baseTraceLocation, which some of our SystemTap scripts will
   * use here.
   *
   * First, make it look as though all memory could be clobbered; then
   * require that a value be read into a register. That'll force at
   * least one instruction to exist (so SystemTap can hook in) where
   * dataKVIO is live. We use a field that the caller would've
   * accessed recently anyway, so it may be cached.
   */
  barrier();
  __asm__ __volatile__(""
                       :
                       : "m" (dataKVIO), "m" (baseTraceLocation),
                         "r" (dataKVIO->kvio.layer));
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
static void kvioCompletionTap2(struct data_kvio *dataKVIO)
{
  // Hack to ensure variable doesn't get optimized out.
  barrier();
  __asm__ __volatile__("" : : "m" (dataKVIO), "r" (dataKVIO->kvio.layer));
}

/**********************************************************************/
static void kvdoAcknowledgeDataKVIO(struct data_kvio *dataKVIO)
{
  KernelLayer                *layer             = dataKVIO->kvio.layer;
  struct external_io_request *externalIORequest = &dataKVIO->externalIORequest;
  struct bio                 *bio               = externalIORequest->bio;
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

  count_bios(&layer->biosAcknowledged, bio);
  if (dataKVIO->isPartial) {
    count_bios(&layer->biosAcknowledgedPartial, bio);
  }


  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  complete_bio(bio, error);
}

/**********************************************************************/
static noinline void cleanDataKVIO(struct data_kvio            *dataKVIO,
                                   struct free_buffer_pointers *fbp)
{
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  kvdoAcknowledgeDataKVIO(dataKVIO);

  struct kvio *kvio = dataKVIOAsKVIO(dataKVIO);
  kvio->bio = NULL;

  if (unlikely(kvio->vio->trace != NULL)) {
    maybeLogDataKVIOTrace(dataKVIO);
    kvioCompletionTap1(dataKVIO);
    kvioCompletionTap2(dataKVIO);
    free_trace_to_pool(kvio->layer, kvio->vio->trace);
  }

  add_free_buffer_pointer(fbp, dataKVIO);
}

/**********************************************************************/
void returnDataKVIOBatchToPool(struct batch_processor *batch, void *closure)
{
  KernelLayer *layer = closure;
  uint32_t     count = 0;
  ASSERT_LOG_ONLY(batch != NULL, "batch not null");
  ASSERT_LOG_ONLY(layer != NULL, "layer not null");

  struct free_buffer_pointers fbp;
  init_free_buffer_pointers(&fbp, layer->dataKVIOPool);

  struct kvdo_work_item *item;
  while ((item = next_batch_item(batch)) != NULL) {
    cleanDataKVIO(workItemAsDataKVIO(item), &fbp);
    cond_resched_batch_processor(batch);
    count++;
  }

  if (fbp.index > 0) {
    free_buffer_pointers(&fbp);
  }

  completeManyRequests(layer, count);
}

/**********************************************************************/
static void kvdoAcknowledgeThenCompleteDataKVIO(struct kvdo_work_item *item)
{
  struct data_kvio *dataKVIO = workItemAsDataKVIO(item);
  kvdoAcknowledgeDataKVIO(dataKVIO);
  add_to_batch_processor(dataKVIO->kvio.layer->dataKVIOReleaser, item);
}

/**********************************************************************/
void kvdoCompleteDataKVIO(VDOCompletion *completion)
{
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(asDataVIO(completion));
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  KernelLayer *layer = getLayerFromDataKVIO(dataKVIO);
  if (useBioAckQueue(layer) && USE_BIO_ACK_QUEUE_FOR_READ
      && (dataKVIO->externalIORequest.bio != NULL)) {
    launchDataKVIOOnBIOAckQueue(dataKVIO, kvdoAcknowledgeThenCompleteDataKVIO,
                                NULL, BIO_ACK_Q_ACTION_ACK);
  } else {
    add_to_batch_processor(layer->dataKVIOReleaser,
                           workItemFromDataKVIO(dataKVIO));
  }
}

/**
 * Copy the uncompressed data from a compressed block read into the user
 * bio which requested the read.
 *
 * @param workItem  The data_kvio which requested the read
 **/
static void copyReadBlockData(struct kvdo_work_item *workItem)
{
  struct data_kvio *dataKVIO = workItemAsDataKVIO(workItem);

  // For a read-modify-write, copy the data into the dataBlock buffer so it
  // will be set up for the write phase.
  if (isReadModifyWriteVIO(dataKVIO->kvio.vio)) {
    bio_copy_data_out(getBIOFromDataKVIO(dataKVIO), dataKVIO->readBlock.data);
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
  bio_copy_data_out(getBIOFromDataKVIO(dataKVIO), dataKVIO->readBlock.data);
  acknowledgeDataVIO(&dataKVIO->dataVIO);
}

/**
 * Finish reading data for a compressed block.
 *
 * @param dataKVIO  The data_kvio which requested the read
 **/
static void readDataKVIOReadBlockCallback(struct data_kvio *dataKVIO)
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
static void resetUserBio(struct bio *bio)
#else
/**
 * Complete and reset a bio that was supplied by the user and then used for a
 * read (so that we can complete it with the user's callback).
 *
 * @param bio   The bio to complete
 * @param error Possible error from underlying block device
 **/
static void resetUserBio(struct bio *bio, int error)
#endif
{
#if ((LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)) \
     && (LINUX_VERSION_CODE < KERNEL_VERSION(4,2,0)))
  // This is a user bio, and the device just called bio_endio() on it, so
  // we need to re-increment bi_remaining so we too can call bio_endio().
  atomic_inc(&bio->bi_remaining);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  complete_async_bio(bio);
#else
  complete_async_bio(bio, error);
#endif
}

/**
 * Uncompress the data that's just been read and then call back the requesting
 * data_kvio.
 *
 * @param workItem  The data_kvio requesting the data
 **/
static void uncompressReadBlock(struct kvdo_work_item *workItem)
{
  struct data_kvio  *dataKVIO  = workItemAsDataKVIO(workItem);
  struct read_block *readBlock = &dataKVIO->readBlock;
  BlockSize          blockSize = VDO_BLOCK_SIZE;

  // The data_kvio's scratch block will be used to contain the
  // uncompressed data.
  uint16_t fragmentOffset, fragmentSize;
  char *compressedData = readBlock->data;
  int result = getCompressedBlockFragment(readBlock->mappingState,
                                          compressedData, blockSize,
                                          &fragmentOffset,
                                          &fragmentSize);
  if (result != VDO_SUCCESS) {
    logDebug("%s: frag err %d", __func__, result);
    readBlock->status = result;
    readBlock->callback(dataKVIO);
    return;
  }

  char *fragment = compressedData + fragmentOffset;
  int size = LZ4_uncompress_unknownOutputSize(fragment, dataKVIO->scratchBlock,
                                              fragmentSize, blockSize);
  if (size == blockSize) {
    readBlock->data = dataKVIO->scratchBlock;
  } else {
    logDebug("%s: lz4 error", __func__);
    readBlock->status = VDO_INVALID_FRAGMENT;
  }

  readBlock->callback(dataKVIO);
}

/**
 * Now that we have gotten the data from storage, uncompress the data if
 * necessary and then call back the requesting data_kvio.
 *
 * @param dataKVIO  The data_kvio requesting the data
 * @param result    The result of the read operation
 **/
static void completeRead(struct data_kvio *dataKVIO, int result)
{
  struct read_block *readBlock = &dataKVIO->readBlock;
  readBlock->status            = result;

  if ((result == VDO_SUCCESS) && isCompressed(readBlock->mappingState)) {
    launchDataKVIOOnCPUQueue(dataKVIO, uncompressReadBlock, NULL,
                             CPU_Q_ACTION_COMPRESS_BLOCK);
    return;
  }

  readBlock->callback(dataKVIO);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 */
static void readBioCallback(struct bio *bio)
#else
/**
 * Callback for a bio doing a read.
 *
 * @param bio     The bio
 * @param result  The result of the read operation
 */
static void readBioCallback(struct bio *bio, int result)
#endif
{
  struct kvio *kvio = (struct kvio *) bio->bi_private;
  struct data_kvio *dataKVIO = kvioAsDataKVIO(kvio);
  dataKVIO->readBlock.data = dataKVIO->readBlock.buffer;
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  count_completed_bios(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  completeRead(dataKVIO, get_bio_result(bio));
#else
  completeRead(dataKVIO, result);
#endif
}

/**********************************************************************/
void kvdoReadBlock(DataVIO             *dataVIO,
                   PhysicalBlockNumber  location,
                   BlockMappingState    mappingState,
                   BioQAction           action,
                   DataKVIOCallback     callback)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));

  struct data_kvio  *dataKVIO  = dataVIOAsDataKVIO(dataVIO);
  struct read_block *readBlock = &dataKVIO->readBlock;
  KernelLayer       *layer     = getLayerFromDataKVIO(dataKVIO);

  readBlock->callback     = callback;
  readBlock->status       = VDO_SUCCESS;
  readBlock->mappingState = mappingState;

  BUG_ON(getBIOFromDataKVIO(dataKVIO)->bi_private != &dataKVIO->kvio);
  // Read the data directly from the device using the read bio.
  struct bio *bio = readBlock->bio;
  reset_bio(bio, layer);
  set_bio_sector(bio, blockToSector(layer, location));
  set_bio_operation_read(bio);
  bio->bi_end_io = readBioCallback;
  vdo_submit_bio(bio, action);
}

/**********************************************************************/
void readDataVIO(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(!isWriteVIO(dataVIOAsVIO(dataVIO)),
                  "operation set correctly for data read");
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F;io=readData"));

  if (isCompressed(dataVIO->mapped.state)) {
    kvdoReadBlock(dataVIO, dataVIO->mapped.pbn, dataVIO->mapped.state,
                  BIO_Q_ACTION_COMPRESSED_DATA, readDataKVIOReadBlockCallback);
    return;
  }

  struct kvio *kvio = dataVIOAsKVIO(dataVIO);
  struct bio *bio   = kvio->bio;
  bio->bi_end_io    = resetUserBio;
  set_bio_sector(bio, blockToSector(kvio->layer, dataVIO->mapped.pbn));
  vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**********************************************************************/
static void kvdoAcknowledgeDataKVIOThenContinue(struct kvdo_work_item *item)
{
  struct data_kvio *dataKVIO = workItemAsDataKVIO(item);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));
  kvdoAcknowledgeDataKVIO(dataKVIO);
  // Even if we're not using bio-ack threads, we may be in the wrong
  // base-code thread.
  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**********************************************************************/
void acknowledgeDataVIO(DataVIO *dataVIO)
{
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  KernelLayer *layer         = getLayerFromDataKVIO(dataKVIO);

  // If the remaining discard work is not completely processed by this VIO,
  // don't acknowledge it yet.
  if (is_discard_bio(dataKVIO->externalIORequest.bio)
      && (dataKVIO->remainingDiscard
          > (VDO_BLOCK_SIZE - dataKVIO->offset))) {
    invokeCallback(dataVIOAsCompletion(dataVIO));
    return;
  }

  // We've finished with the kvio; acknowledge completion of the bio to the
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
void writeDataVIO(DataVIO *dataVIO)
{
  ASSERT_LOG_ONLY(isWriteVIO(dataVIOAsVIO(dataVIO)),
                  "kvdoWriteDataVIO() called on write DataVIO");
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("$F;io=writeData;j=normal"));

  struct kvio *kvio = dataVIOAsKVIO(dataVIO);
  struct bio *bio   = kvio->bio;
  set_bio_operation_write(bio);
  set_bio_sector(bio, blockToSector(kvio->layer, dataVIO->newMapped.pbn));
  vdo_submit_bio(bio, BIO_Q_ACTION_DATA);
}

/**
 * Determines whether the data block buffer is all zeros.
 *
 * @param dataKVIO  The data_kvio to check
 *
 * @return true is all zeroes, false otherwise
 **/
static inline bool isZeroBlock(struct data_kvio *dataKVIO)
{
  const char *buffer = dataKVIO->dataBlock;
  /*
   * Handle expected common case of even the first word being nonzero,
   * without getting into the more expensive (for one iteration) loop
   * below.
   */
  if (get_unaligned((u64 *) buffer) != 0) {
    return false;
  }

  STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(uint64_t) == 0);
  unsigned int wordCount = VDO_BLOCK_SIZE / sizeof(uint64_t);

  // Unroll to process 64 bytes at a time
  unsigned int chunkCount = wordCount / 8;
  while (chunkCount-- > 0) {
    uint64_t word0 = get_unaligned((u64 *) buffer);
    uint64_t word1 = get_unaligned((u64 *) (buffer + 1 * sizeof(uint64_t)));
    uint64_t word2 = get_unaligned((u64 *) (buffer + 2 * sizeof(uint64_t)));
    uint64_t word3 = get_unaligned((u64 *) (buffer + 3 * sizeof(uint64_t)));
    uint64_t word4 = get_unaligned((u64 *) (buffer + 4 * sizeof(uint64_t)));
    uint64_t word5 = get_unaligned((u64 *) (buffer + 5 * sizeof(uint64_t)));
    uint64_t word6 = get_unaligned((u64 *) (buffer + 6 * sizeof(uint64_t)));
    uint64_t word7 = get_unaligned((u64 *) (buffer + 7 * sizeof(uint64_t)));
    uint64_t or = (word0 | word1 | word2 | word3
                   | word4 | word5 | word6 | word7);
    // Prevent compiler from using 8*(cmp;jne).
    __asm__ __volatile__ ("" : : "g" (or));
    if (or != 0) {
      return false;
    }
    buffer += 8 * sizeof(uint64_t);
  }
  wordCount %= 8;

  // Unroll to process 8 bytes at a time.
  // (Is this still worthwhile?)
  while (wordCount-- > 0) {
    if (get_unaligned((u64 *) buffer) != 0) {
      return false;
    }
    buffer += sizeof(uint64_t);
  }
  return true;
}

/**********************************************************************/
void applyPartialWrite(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  struct bio  *bio           = dataKVIO->externalIORequest.bio;
  KernelLayer *layer         = getLayerFromDataKVIO(dataKVIO);
  reset_bio(dataKVIO->dataBlockBio, layer);

  if (!is_discard_bio(bio)) {
    bio_copy_data_in(bio, dataKVIO->dataBlock + dataKVIO->offset);
  } else {
    memset(dataKVIO->dataBlock + dataKVIO->offset, '\0',
           min(dataKVIO->remainingDiscard,
               (DiscardSize) (VDO_BLOCK_SIZE - dataKVIO->offset)));
  }

  dataVIO->isZeroBlock               = isZeroBlock(dataKVIO);
  dataKVIO->dataBlockBio->bi_private = &dataKVIO->kvio;
  copy_bio_operation_and_flags(dataKVIO->dataBlockBio, bio);
  // Make the bio a write, not (potentially) a discard.
  set_bio_operation_write(dataKVIO->dataBlockBio);
}

/**********************************************************************/
void zeroDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION("zeroDataVIO;io=readData"));
  bio_zero_data(dataVIOAsKVIO(dataVIO)->bio);
}

/**********************************************************************/
void copyData(DataVIO *source, DataVIO *destination)
{
  dataVIOAddTraceRecord(destination, THIS_LOCATION(NULL));
  bio_copy_data_out(dataVIOAsKVIO(destination)->bio,
                    dataVIOAsDataKVIO(source)->dataBlock);
}

/**********************************************************************/
static void kvdoCompressWork(struct kvdo_work_item *item)
{
  struct data_kvio *dataKVIO = workItemAsDataKVIO(item);
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  char *context = get_work_queue_private_data();
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
void compressDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO,
                        THIS_LOCATION("compressDataVIO;"
                                      "io=compress;cb=compress"));

  /*
   * If the orignal bio was a discard, but we got this far because the discard
   * was a partial one (r/m/w), and it is part of a larger discard, we cannot
   * compress this VIO. We need to make sure the VIO completes ASAP.
   */
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  if (is_discard_bio(dataKVIO->externalIORequest.bio)
      && (dataKVIO->remainingDiscard > 0)) {
    dataVIO->compression.size = VDO_BLOCK_SIZE + 1;
    kvdoEnqueueDataVIOCallback(dataKVIO);
    return;
  }

  launchDataKVIOOnCPUQueue(dataKVIO, kvdoCompressWork, NULL,
                           CPU_Q_ACTION_COMPRESS_BLOCK);
}

/**
 * Construct a data_kvio.
 *
 * @param [in]  layer        The physical layer
 * @param [in]  bio          The bio to associate with this data_kvio
 * @param [out] dataKVIOPtr  A pointer to hold the new data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int makeDataKVIO(KernelLayer       *layer,
                        struct bio        *bio,
                        struct data_kvio **dataKVIOPtr)
{
  struct data_kvio *dataKVIO;
  int result = alloc_buffer_from_pool(layer->dataKVIOPool,
                                      (void **) &dataKVIO);
  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result, "data kvio allocation failure");
  }

  if (WRITE_PROTECT_FREE_POOL) {
    setWriteProtect(dataKVIO, WP_DATA_KVIO_SIZE, false);
  }

  struct kvio *kvio = &dataKVIO->kvio;
  kvio->vio = dataVIOAsVIO(&dataKVIO->dataVIO);
  memset(&kvio->enqueueable, 0, sizeof(KvdoEnqueueable));
  memset(&dataKVIO->dedupeContext.pendingList, 0, sizeof(struct list_head));
  memset(&dataKVIO->dataVIO, 0, sizeof(DataVIO));
  kvio->bio_to_submit = NULL;
  bio_list_init(&kvio->bios_merged);

  // The dataBlock is only needed for writes and some partial reads.
  if (is_write_bio(bio) || (get_bio_size(bio) < VDO_BLOCK_SIZE)) {
    reset_bio(dataKVIO->dataBlockBio, layer);
  }

  initialize_kvio(kvio, layer, VIO_TYPE_DATA, VIO_PRIORITY_DATA, NULL, bio);
  *dataKVIOPtr = dataKVIO;
  return VDO_SUCCESS;
}

/**
 * Creates a new DataVIO structure. A DataVIO represents a single logical
 * block of data. It is what most VDO operations work with. This function also
 * creates a wrapping data_kvio structure that is used when we want to
 * physically read or write the data associated with the DataVIO.
 *
 * @param [in]  layer        The physical layer
 * @param [in]  bio          The bio from the request the new data_kvio will
 *                           service
 * @param [in]  arrivalTime  The arrival time of the bio
 * @param [out] dataKVIOPtr  A pointer to hold the new data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int kvdoCreateKVIOFromBio(KernelLayer       *layer,
                                 struct bio        *bio,
                                 Jiffies            arrivalTime,
                                 struct data_kvio **dataKVIOPtr)
{
  struct external_io_request externalIORequest = {
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
  clear_bio_operation_flag_fua(bio);

  struct data_kvio *dataKVIO = NULL;
  int               result   = makeDataKVIO(layer, bio, &dataKVIO);
  if (result != VDO_SUCCESS) {
    return result;
  }

  dataKVIO->externalIORequest = externalIORequest;
  dataKVIO->offset = sectorToBlockOffset(layer, get_bio_sector(bio));
  dataKVIO->isPartial = ((get_bio_size(bio) < VDO_BLOCK_SIZE)
                         || (dataKVIO->offset != 0));

  if (dataKVIO->isPartial) {
    count_bios(&layer->biosInPartial, bio);
  } else {
    /*
     * Note that we unconditionally fill in the dataBlock array for
     * non-read operations. There are places like kvdoCopyVIO that may
     * look at kvio->dataBlock for a zero block (and maybe for
     * discards?). We could skip filling in dataBlock for such cases,
     * but only once we're sure all such places are fixed to check the
     * isZeroBlock flag first.
     */
    if (is_discard_bio(bio)) {
      /*
       * This is a discard/trim operation. This is treated much like the zero
       * block, but we keep different stats and distinguish it in the block
       * map.
       */
      memset(dataKVIO->dataBlock, 0, VDO_BLOCK_SIZE);
    } else if (bio_data_dir(bio) == WRITE) {
      // Copy the bio data to a char array so that we can continue to use
      // the data after we acknowledge the bio.
      bio_copy_data_in(bio, dataKVIO->dataBlock);
      dataKVIO->dataVIO.isZeroBlock = isZeroBlock(dataKVIO);
    }
  }

  if (dataKVIO->isPartial || is_write_bio(bio)) {
    /*
     * dataKVIO->bio will point at kvio->dataBlockBio for all writes and
     * partial block I/O so the rest of the kernel code doesn't need to
     * make a decision as to what to use.
     */
    dataKVIO->dataBlockBio->bi_private = &dataKVIO->kvio;
    if (dataKVIO->isPartial && is_write_bio(bio)) {
      clear_bio_operation_and_flags(dataKVIO->dataBlockBio);
      set_bio_operation_read(dataKVIO->dataBlockBio);
    } else {
      copy_bio_operation_and_flags(dataKVIO->dataBlockBio, bio);
    }
    dataKVIOAsKVIO(dataKVIO)->bio = dataKVIO->dataBlockBio;
    dataKVIO->readBlock.data      = dataKVIO->dataBlock;
  }

  set_bio_block_device(bio, getKernelLayerBdev(layer));
  bio->bi_end_io = complete_async_bio;
  *dataKVIOPtr   = dataKVIO;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void launchDataKVIOWork(struct kvdo_work_item *item)
{
  runCallback(vioAsCompletion(work_item_as_kvio(item)->vio));
}

/**
 * Continue discard processing for requests that span multiple physical blocks.
 * If all have been processed the kvio is completed.  If we have already seen
 * an error, we skip the rest of the discard and fail immediately.
 *
 * <p>Invoked in a request-queue thread after the discard of a block has
 * completed.
 *
 * @param completion  A completion representing the discard kvio
 **/
static void kvdoContinueDiscardKVIO(VDOCompletion *completion)
{
  DataVIO          *dataVIO  = asDataVIO(completion);
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  KernelLayer      *layer    = getLayerFromDataKVIO(dataKVIO);
  dataKVIO->remainingDiscard
    -= min(dataKVIO->remainingDiscard,
           (DiscardSize) (VDO_BLOCK_SIZE - dataKVIO->offset));
  if ((completion->result != VDO_SUCCESS)
      || (dataKVIO->remainingDiscard == 0)) {
    if (dataKVIO->hasDiscardPermit) {
      limiter_release(&layer->discardLimiter);
      dataKVIO->hasDiscardPermit = false;
    }
    kvdoCompleteDataKVIO(completion);
    return;
  }

  struct bio *bio = getBIOFromDataKVIO(dataKVIO);
  reset_bio(bio, layer);
  dataKVIO->isPartial = (dataKVIO->remainingDiscard < VDO_BLOCK_SIZE);
  dataKVIO->offset    = 0;

  VIOOperation operation;
  if (dataKVIO->isPartial) {
    operation  = VIO_READ_MODIFY_WRITE;
    set_bio_operation_read(bio);
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
 * @param completion  The partial read kvio
 **/
static void kvdoCompletePartialRead(VDOCompletion *completion)
{
  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(asDataVIO(completion));
  dataKVIOAddTraceRecord(dataKVIO, THIS_LOCATION(NULL));

  bio_copy_data_out(dataKVIO->externalIORequest.bio,
                    dataKVIO->readBlock.data + dataKVIO->offset);
  kvdoCompleteDataKVIO(completion);
  return;
}

/**********************************************************************/
int kvdoLaunchDataKVIOFromBio(KernelLayer *layer,
                              struct bio  *bio,
                              uint64_t     arrivalTime,
                              bool         hasDiscardPermit)
{

  struct data_kvio *dataKVIO = NULL;
  int result = kvdoCreateKVIOFromBio(layer, bio, arrivalTime, &dataKVIO);
  if (unlikely(result != VDO_SUCCESS)) {
    logInfo("%s: kvio allocation failure", __func__);
    if (hasDiscardPermit) {
      limiter_release(&layer->discardLimiter);
    }
    limiter_release(&layer->requestLimiter);
    return mapToSystemError(result);
  }

  /*
   * Discards behave very differently than other requests when coming
   * in from device-mapper. We have to be able to handle any size discards
   * and with various sector offsets within a block.
   */
  struct kvio  *kvio      = &dataKVIO->kvio;
  VDOAction    *callback  = kvdoCompleteDataKVIO;
  VIOOperation  operation = VIO_WRITE;
  bool          isTrim    = false;
  if (is_discard_bio(bio)) {
    dataKVIO->hasDiscardPermit = hasDiscardPermit;
    dataKVIO->remainingDiscard = get_bio_size(bio);
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
    = sectorToBlock(layer, get_bio_sector(bio) - layer->startingSectorOffset);
  prepareDataVIO(&dataKVIO->dataVIO, lbn, operation, isTrim, callback);
  enqueue_kvio(kvio, launchDataKVIOWork, vioAsCompletion(kvio->vio)->callback,
              REQ_Q_ACTION_MAP_BIO);
  return VDO_SUCCESS;
}

/**
 * Hash a data_kvio and set its chunk name.
 *
 * @param item  The data_kvio to be hashed
 **/
static void kvdoHashDataWork(struct kvdo_work_item *item)
{
  struct data_kvio *dataKVIO = workItemAsDataKVIO(item);
  DataVIO          *dataVIO  = &dataKVIO->dataVIO;
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));

  MurmurHash3_x64_128(dataKVIO->dataBlock, VDO_BLOCK_SIZE, 0x62ea60be,
                      &dataVIO->chunkName);
  dataKVIO->dedupeContext.chunkName = &dataVIO->chunkName;

  kvdoEnqueueDataVIOCallback(dataKVIO);
}

/**********************************************************************/
void hashDataVIO(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO, THIS_LOCATION(NULL));
  launchDataKVIOOnCPUQueue(dataVIOAsDataKVIO(dataVIO), kvdoHashDataWork, NULL,
                           CPU_Q_ACTION_HASH_BLOCK);
}

/**********************************************************************/
void checkForDuplication(DataVIO *dataVIO)
{
  dataVIOAddTraceRecord(dataVIO,
                        THIS_LOCATION("checkForDuplication;dup=post"));
  ASSERT_LOG_ONLY(!dataVIO->isZeroBlock,
                  "zero block not checked for duplication");
  ASSERT_LOG_ONLY(dataVIO->newMapped.state != MAPPING_STATE_UNMAPPED,
                  "discard not checked for duplication");

  struct data_kvio *dataKVIO = dataVIOAsDataKVIO(dataVIO);
  if (hasAllocation(dataVIO)) {
    postDedupeAdvice(dataKVIO);
  } else {
    // This block has not actually been written (presumably because we are
    // full), so attempt to dedupe without posting bogus advice.
    queryDedupeAdvice(dataKVIO);
  }
}

/**********************************************************************/
void updateDedupeIndex(DataVIO *dataVIO)
{
  updateDedupeAdvice(dataVIOAsDataKVIO(dataVIO));
}

/**
 * Implements buffer_free_function.
 **/
static void freePooledDataKVIO(void *poolData, void *data)
{
  if (data == NULL) {
    return;
  }

  struct data_kvio *dataKVIO = (struct data_kvio *) data;
  KernelLayer      *layer    = (KernelLayer *) poolData;
  if (WRITE_PROTECT_FREE_POOL) {
    setWriteProtect(dataKVIO, WP_DATA_KVIO_SIZE, false);
  }

  if (dataKVIO->dataBlockBio != NULL) {
    free_bio(dataKVIO->dataBlockBio, layer);
  }

  if (dataKVIO->readBlock.bio != NULL) {
    free_bio(dataKVIO->readBlock.bio, layer);
  }

  FREE(dataKVIO->readBlock.buffer);
  FREE(dataKVIO->dataBlock);
  FREE(dataKVIO->scratchBlock);
  FREE(dataKVIO);
}

/**
 * Allocate a data_kvio. This function is the internals of
 * makePooledDataKVIO().
 *
 * @param [in]  layer        The layer in which the data_kvio will operate
 * @param [out] dataKVIOPtr  A pointer to hold the newly allocated data_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocatePooledDataKVIO(KernelLayer       *layer,
                                  struct data_kvio **dataKVIOPtr)
{
  struct data_kvio *dataKVIO;
  int result;
  if (WRITE_PROTECT_FREE_POOL) {
    STATIC_ASSERT(WP_DATA_KVIO_SIZE >= sizeof(struct data_kvio));
    result = allocateMemory(WP_DATA_KVIO_SIZE, 0, __func__, &dataKVIO);
    if (result == VDO_SUCCESS) {
      BUG_ON((((size_t) dataKVIO) & (PAGE_SIZE - 1)) != 0);
    }
  } else {
    result = ALLOCATE(1, struct data_kvio, __func__, &dataKVIO);
  }

  if (result != VDO_SUCCESS) {
    return logErrorWithStringError(result, "data_kvio allocation failure");
  }

  STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
  result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio data",
                          &dataKVIO->dataBlock);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(layer, dataKVIO);
    return logErrorWithStringError(result, "data_kvio data allocation failure");
  }

  result = create_bio(layer, dataKVIO->dataBlock, &dataKVIO->dataBlockBio);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(layer, dataKVIO);
    return logErrorWithStringError(result,
                                   "data_kvio data bio allocation failure");
  }

  result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio read buffer",
                          &dataKVIO->readBlock.buffer);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(layer, dataKVIO);
    return logErrorWithStringError(result,
                                   "data_kvio read allocation failure");
  }

  result = create_bio(layer, dataKVIO->readBlock.buffer,
                      &dataKVIO->readBlock.bio);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(layer, dataKVIO);
    return logErrorWithStringError(result,
                                   "data_kvio read bio allocation failure");
  }

  dataKVIO->readBlock.bio->bi_private = &dataKVIO->kvio;

  result = allocateMemory(VDO_BLOCK_SIZE, 0, "kvio scratch",
                          &dataKVIO->scratchBlock);
  if (result != VDO_SUCCESS) {
    freePooledDataKVIO(layer, dataKVIO);
    return logErrorWithStringError(result,
                                   "data_kvio scratch allocation failure");
  }

  *dataKVIOPtr = dataKVIO;
  return VDO_SUCCESS;
}

/**
 * Implements buffer_allocate_function.
 **/
static int makePooledDataKVIO(void *poolData, void **dataPtr)
{
  struct data_kvio *dataKVIO = NULL;
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
  logInfo("      %s is locked. Waited on by: VIO %" PRIptr " pbn %" PRIu64
          " lbn %llu d-pbn %llu lastOp %s",
          waitOn, dataVIO, getDataVIOAllocation(dataVIO),
          dataVIO->logical.lbn, dataVIO->duplicate.pbn,
          getOperationName(dataVIO));

  for (Waiter *waiter = first->nextWaiter;
       waiter != first;
       waiter = waiter->nextWaiter) {
    dataVIO = waiterAsDataVIO(waiter);
    logInfo("     ... and : VIO %" PRIptr " pbn %llu lbn %"
            PRIu64 " d-pbn %llu lastOp %s",
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
 * Dump out info on a data_kvio from the data_kvio pool.
 *
 * <p>Implements buffer_dump_function.
 *
 * @param poolData  The pool data
 * @param data      The data_kvio to dump
 **/
static void dumpPooledDataKVIO(void *poolData __attribute__((unused)),
                               void *data)
{
  struct data_kvio *dataKVIO = (struct data_kvio *) data;
  DataVIO          *dataVIO  = &dataKVIO->dataVIO;

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
  dump_work_item_to_buffer(&dataKVIO->kvio.enqueueable.workItem,
                           vioWorkItemDumpBuffer,
                           sizeof(vioWorkItemDumpBuffer));
  // Another static buffer...
  // log10(256) = 2.408+, round up:
  enum { DECIMAL_DIGITS_PER_UINT64_T = (int) (1 + 2.41 * sizeof(uint64_t)) };
  static char vioBlockNumberDumpBuffer[sizeof("P L D")
                                       + 3 * DECIMAL_DIGITS_PER_UINT64_T];
  if (dataVIO->isDuplicate) {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "P%llu L%llu D%llu",
             getDataVIOAllocation(dataVIO), dataVIO->logical.lbn,
             dataVIO->duplicate.pbn);
  } else if (hasAllocation(dataVIO)) {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "P%llu L%llu",
             getDataVIOAllocation(dataVIO), dataVIO->logical.lbn);
  } else {
    snprintf(vioBlockNumberDumpBuffer, sizeof(vioBlockNumberDumpBuffer),
             "L%llu",
             dataVIO->logical.lbn);
  }

  static char vioFlushGenerationBuffer[sizeof(" FG")
                                       + DECIMAL_DIGITS_PER_UINT64_T] = "";
  if (dataVIO->flushGeneration != 0) {
    snprintf(vioFlushGenerationBuffer, sizeof(vioFlushGenerationBuffer),
             " FG%llu", dataVIO->flushGeneration);
  }

  // Encode VIO attributes as a string of one-character flags, usually empty.
  static char flagsDumpBuffer[8];
  encodeVIODumpFlags(dataVIO, flagsDumpBuffer);

  logInfo("  kvio %" PRIptr " %s%s %s %s%s",
          dataKVIO, vioBlockNumberDumpBuffer, vioFlushGenerationBuffer,
          getOperationName(dataVIO), vioWorkItemDumpBuffer, flagsDumpBuffer);
  // might want info on: wantAlbireoAnswer / operation / status
  // might want info on: bio / bio_to_submit / bios_merged

  dumpVIOWaiters(&dataVIO->logical.waiters, "lbn");

  // might want to dump more info from VIO here
}

/**********************************************************************/
int makeDataKVIOBufferPool(KernelLayer          *layer,
                           uint32_t              poolSize,
                           struct buffer_pool  **bufferPoolPtr)
{
  return make_buffer_pool("data_kvio pool", poolSize,
                          makePooledDataKVIO, freePooledDataKVIO,
                          dumpPooledDataKVIO, layer, bufferPoolPtr);
}

/**********************************************************************/
DataLocation getDedupeAdvice(const struct dedupe_context *context)
{
  struct data_kvio *dataKVIO = container_of(context,
                                            struct data_kvio,
                                            dedupeContext);
  return (DataLocation) {
    .state = dataKVIO->dataVIO.newMapped.state,
    .pbn   = dataKVIO->dataVIO.newMapped.pbn,
  };
}

/**********************************************************************/
void setDedupeAdvice(struct dedupe_context *context,
                     const DataLocation *advice)
{
  struct data_kvio *dataKVIO = container_of(context,
                                            struct data_kvio,
                                            dedupeContext);
  receiveDedupeAdvice(&dataKVIO->dataVIO, advice);
}
