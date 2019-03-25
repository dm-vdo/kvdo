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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.c#44 $
 */

#include "kernelLayer.h"

#include <linux/crc32.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/backing-dev.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "lz4.h"
#include "releaseVersions.h"
#include "volumeGeometry.h"
#include "statistics.h"
#include "vdo.h"

#include "bio.h"
#include "dataKVIO.h"
#include "dedupeIndex.h"
#include "deviceConfig.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "poolSysfs.h"
#include "statusProcfs.h"
#include "stringUtils.h"

static const KvdoWorkQueueType bioAckQType = {
  .actionTable = {
    { .name = "bio_ack",
      .code = BIO_ACK_Q_ACTION_ACK,
      .priority = 0 },
  },
};

static const KvdoWorkQueueType cpuQType = {
  .actionTable = {
    { .name = "cpu_complete_kvio",
      .code = CPU_Q_ACTION_COMPLETE_KVIO,
      .priority = 0 },
    { .name = "cpu_compress_block",
      .code = CPU_Q_ACTION_COMPRESS_BLOCK,
      .priority = 0 },
    { .name = "cpu_hash_block",
      .code = CPU_Q_ACTION_HASH_BLOCK,
      .priority = 0 },
    { .name = "cpu_event_reporter",
      .code = CPU_Q_ACTION_EVENT_REPORTER,
      .priority = 0 },
  },
};

// 2000 is half the number of entries currently in our page cache,
// to allow for each in-progress operation to update two pages.
int default_max_requests_active = 2000;

/**********************************************************************/
CRC32Checksum updateCRC32(CRC32Checksum  crc,
                          const byte    *buffer,
                          size_t         length)
{
  /*
   * The kernel's CRC 32 implementation does not do pre- and post-
   * conditioning, so do it ourselves.
   */
  return crc32(crc ^ 0xffffffff, buffer, length) ^ 0xffffffff;
}

/**********************************************************************/
static BlockCount kvdoGetBlockCount(PhysicalLayer *header)
{
  return asKernelLayer(header)->deviceConfig->physical_blocks;
}

/**********************************************************************/
int mapToSystemError(int error)
{
  // 0 is success, negative a system error code
  if (likely(error <= 0)) {
    return error;
  }
  if (error < 1024) {
    // errno macro used without negating - may be a minor bug
    return -error;
  }
  // VDO or UDS error
  char errorName[80], errorMessage[ERRBUF_SIZE];
  switch (sans_unrecoverable(error)) {
  case VDO_NO_SPACE:
    return -ENOSPC;
  case VDO_READ_ONLY:
    return -EIO;
  default:
    logInfo("%s: mapping internal status code %d (%s: %s) to EIO",
            __func__, error,
            string_error_name(error, errorName, sizeof(errorName)),
            string_error(error, errorMessage, sizeof(errorMessage)));
    return -EIO;
  }
}

/**********************************************************************/
static void setKernelLayerState(KernelLayer *layer, KernelLayerState newState)
{
  relaxedStore32(&layer->state, newState);
}

/**********************************************************************/
void waitForNoRequestsActive(KernelLayer *layer)
{
  // Do nothing if there are no requests active.  This check is not necessary
  // for correctness but does reduce log message traffic.
  if (limiter_is_idle(&layer->requestLimiter)) {
    return;
  }

  // We have to make sure to flush the packer before waiting. We do this
  // by turning off compression, which also means no new entries coming in
  // while waiting will end up in the packer.
  bool wasCompressing = setKVDOCompressing(&layer->kvdo, false);
  // Now wait for there to be no active requests
  limiter_wait_for_idle(&layer->requestLimiter);
  // Reset the compression state after all requests are done
  if (wasCompressing) {
    setKVDOCompressing(&layer->kvdo, true);
  }
}

/**
 * Start processing a new data KVIO based on the supplied bio, but from within
 * a VDO thread context, when we're not allowed to block. Using this path at
 * all suggests a bug or erroneous usage, but we special-case it to avoid a
 * deadlock that can apparently result. Message will be logged to alert the
 * administrator that something has gone wrong, while we attempt to continue
 * processing other requests.
 *
 * If a request permit can be acquired immediately, kvdoLaunchDataKVIOFromBio
 * will be called. (If the bio is a discard operation, a permit from the
 * discard limiter will be requested but the call will be made with or without
 * it.) If the request permit is not available, the bio will be saved on a list
 * to be launched later. Either way, this function will not block, and will
 * take responsibility for processing the bio.
 *
 * @param layer        The kernel layer
 * @param bio          The bio to launch
 * @param arrivalTime  The arrival time of the bio
 *
 * @return  DM_MAPIO_SUBMITTED or a system error code
 **/
static int launchDataKVIOFromVDOThread(KernelLayer *layer,
                                       struct bio  *bio,
                                       Jiffies      arrivalTime)
{
  logWarning("kvdoMapBio called from within a VDO thread!");
  /*
   * We're not yet entirely sure what circumstances are causing this situation
   * in [ESC-638], but it does appear to be happening and causing VDO to
   * deadlock.
   *
   * Somehow kvdoMapBio is being called from generic_make_request which is
   * being called from the VDO code to pass a flush on down to the underlying
   * storage system; we've got 2000 requests in progress, so we have to wait
   * for one to complete, but none can complete while the bio thread is blocked
   * from passing more I/O requests down. Near as we can tell, the flush bio
   * should always have gotten updated to point to the storage system, so we
   * shouldn't be calling back into VDO unless something's gotten messed up
   * somewhere.
   *
   * To side-step this case, if the limiter says we're busy *and* we're running
   * on one of VDO's own threads, we'll drop the I/O request in a special queue
   * for processing as soon as KVIOs become free.
   *
   * We don't want to do this in general because it leads to unbounded
   * buffering, arbitrarily high latencies, inability to push back in a way the
   * caller can take advantage of, etc. If someone wants huge amounts of
   * buffering on top of VDO, they're welcome to access it through the kernel
   * page cache or roll their own.
   */
  if (!limiter_poll(&layer->requestLimiter)) {
    add_to_deadlock_queue(&layer->deadlockQueue, bio, arrivalTime);
    logWarning("queued an I/O request to avoid deadlock!");

    return DM_MAPIO_SUBMITTED;
  }

  bool hasDiscardPermit
    = (is_discard_bio(bio) && limiter_poll(&layer->discardLimiter));
  int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                         hasDiscardPermit);
  // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
  if (result != VDO_SUCCESS) {
    return result;
  }

  return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
int kvdoMapBio(KernelLayer *layer, struct bio *bio)
{
  Jiffies          arrivalTime = jiffies;
  KernelLayerState state       = getKernelLayerState(layer);
  ASSERT_LOG_ONLY(state == LAYER_RUNNING,
                  "kvdoMapBio should not be called while in state %d", state);

  // Count all incoming bios.
  count_bios(&layer->biosIn, bio);

  // Handle empty bios.  Empty flush bios are not associated with a VIO.
  if (is_flush_bio(bio)) {
    if (ASSERT(get_bio_size(bio) == 0, "Flush bio is size 0") != VDO_SUCCESS) {
      // We expect flushes to be of size 0.
      return -EINVAL;
    }
    if (should_process_flush(layer)) {
      launch_kvdo_flush(layer, bio);
      return DM_MAPIO_SUBMITTED;
    } else {
      // We're not acknowledging this bio now, but we'll never touch it
      // again, so this is the last chance to account for it.
      count_bios(&layer->biosAcknowledged, bio);
      atomic64_inc(&layer->flushOut);
      set_bio_block_device(bio, getKernelLayerBdev(layer));
      return DM_MAPIO_REMAPPED;
    }
  }

  if (ASSERT(get_bio_size(bio) != 0, "Data bio is not size 0")
      != VDO_SUCCESS) {
    // We expect non-flushes to be non-zero in size.
    return -EINVAL;
  }

  if (is_discard_bio(bio) && is_read_bio(bio)) {
    // Read and Discard should never occur together
    return -EIO;
  }

  struct kvdo_work_queue *currentWorkQueue = get_current_work_queue();
  if ((currentWorkQueue != NULL)
      && (layer == get_work_queue_owner(currentWorkQueue))) {
    /*
     * This prohibits sleeping during I/O submission to VDO from its own
     * thread.
     */
    return launchDataKVIOFromVDOThread(layer, bio, arrivalTime);
  }
  bool hasDiscardPermit = false;
  if (is_discard_bio(bio)) {
    limiter_wait_for_one_free(&layer->discardLimiter);
    hasDiscardPermit = true;
  }
  limiter_wait_for_one_free(&layer->requestLimiter);

  int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                         hasDiscardPermit);
  // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
  if (result != VDO_SUCCESS) {
    return result;
  }

  return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
struct block_device *getKernelLayerBdev(const KernelLayer *layer)
{
  return layer->deviceConfig->owned_device->bdev;
}

/**********************************************************************/
void completeManyRequests(KernelLayer *layer, uint32_t count)
{
  // If we had to buffer some requests to avoid deadlock, release them now.
  while (count > 0) {
    Jiffies arrivalTime = 0;
    struct bio *bio = poll_deadlock_queue(&layer->deadlockQueue, &arrivalTime);
    if (likely(bio == NULL)) {
      break;
    }

    bool hasDiscardPermit
      = (is_discard_bio(bio) && limiter_poll(&layer->discardLimiter));
    int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                           hasDiscardPermit);
    if (result != VDO_SUCCESS) {
      complete_bio(bio, result);
    }
    // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
    count--;
  }
  // Notify the limiter, so it can wake any blocked processes.
  if (count > 0) {
    limiter_release_many(&layer->requestLimiter, count);
  }
}

/**********************************************************************/
static int kvdoCreateEnqueueable(VDOCompletion *completion)
{
  KvdoEnqueueable *kvdoEnqueueable;
  int result = ALLOCATE(1, KvdoEnqueueable, "kvdoEnqueueable",
                        &kvdoEnqueueable);
  if (result != VDO_SUCCESS) {
    logError("kvdoEnqueueable allocation failure %d", result);
    return result;
  }
  kvdoEnqueueable->enqueueable.completion = completion;
  completion->enqueueable                 = &kvdoEnqueueable->enqueueable;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void kvdoDestroyEnqueueable(Enqueueable **enqueueablePtr)
{
  Enqueueable *enqueueable = *enqueueablePtr;
  if (enqueueable != NULL) {
    KvdoEnqueueable *kvdoEnqueueable
      = container_of(enqueueable, KvdoEnqueueable, enqueueable);
    FREE(kvdoEnqueueable);
    *enqueueablePtr = NULL;
  }
}

/**
 * Implements BufferAllocator.
 **/
static int kvdoAllocateIOBuffer(PhysicalLayer  *layer __attribute__((unused)),
                                size_t          bytes,
                                const char     *why,
                                char          **bufferPtr)
{
  return ALLOCATE(bytes, char, why, bufferPtr);
}

/**
 * Implements ExtentReader. Exists only for the geometry block; is unset after
 * it is read.
 **/
static int kvdoSynchronousRead(PhysicalLayer       *layer,
                               PhysicalBlockNumber  startBlock,
                               size_t               blockCount,
                               char                *buffer,
                               size_t              *blocksRead)
{
  if (blockCount != 1) {
    return VDO_NOT_IMPLEMENTED;
  }

  KernelLayer *kernelLayer = asKernelLayer(layer);

  struct bio *bio;
  int result = create_bio(kernelLayer, buffer, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }
  set_bio_block_device(bio, getKernelLayerBdev(kernelLayer));
  set_bio_sector(bio, blockToSector(kernelLayer, startBlock));
  set_bio_operation_read(bio);
  result = submit_bio_and_wait(bio);
  if (result != 0) {
    logErrorWithStringError(result, "synchronous read failed");
    result = -EIO;
  }
  free_bio(bio, kernelLayer);

  if (result != VDO_SUCCESS) {
    return result;
  }
  if (blocksRead != NULL) {
    *blocksRead = blockCount;
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
void destroyVIO(VIO **vioPtr)
{
  VIO *vio = *vioPtr;
  if (vio == NULL) {
    return;
  }

  BUG_ON(isDataVIO(vio));

  if (isCompressedWriteVIO(vio)) {
    struct compressed_write_kvio *compressedWriteKVIO
      = allocating_vio_as_compressed_write_kvio(vioAsAllocatingVIO(vio));
    free_compressed_write_kvio(&compressedWriteKVIO);
  } else {
    struct metadata_kvio *metadataKVIO = vio_as_metadata_kvio(vio);
    free_metadata_kvio(&metadataKVIO);
  }

  *vioPtr = NULL;
}

/**********************************************************************/
static bool isFlushRequired(PhysicalLayer *common)
{
  return should_process_flush(asKernelLayer(common));
}

/**
 * Function that is called when a synchronous operation is completed. We let
 * the waiting thread know it can continue.
 *
 * <p>Implements OperationComplete.
 *
 * @param common  The kernel layer
 **/
static void kvdoCompleteSyncOperation(PhysicalLayer *common)
{
  KernelLayer *layer = asKernelLayer(common);
  complete(&layer->callbackSync);
}

/**
 * Wait for a synchronous operation to complete.
 *
 * <p>Implements OperationWaiter.
 *
 * @param common  The kernel layer
 **/
static void waitForSyncOperation(PhysicalLayer *common)
{
  KernelLayer *layer = asKernelLayer(common);
  // Using the "interruptible" interface means that Linux will not log a
  // message when we wait for more than 120 seconds.
  while (wait_for_completion_interruptible(&layer->callbackSync) != 0) {
  }
}

/**
 * Check whether a VDO, or its backing storage, is congested.
 *
 * @param callbacks  The callbacks structure inside the kernel layer
 * @param bdi_bits   Some info things to pass through.
 *
 * @return true if VDO or one of its backing storage stack is congested.
 **/
static int kvdoIsCongested(struct dm_target_callbacks *callbacks,
                           int                         bdi_bits)
{
  KernelLayer *layer = container_of(callbacks, KernelLayer, callbacks);
  if (!limiter_has_one_free(&layer->requestLimiter)) {
    return 1;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
  struct backing_dev_info *backing
   = bdev_get_queue(getKernelLayerBdev(layer))->backing_dev_info;
#else
  struct backing_dev_info *backing
   = &bdev_get_queue(getKernelLayerBdev(layer))->backing_dev_info;
#endif
  return bdi_congested(backing, bdi_bits);
}

/**********************************************************************/
int makeKernelLayer(uint64_t               startingSector,
                    unsigned int           instance,
                    struct device_config  *config,
                    struct kobject        *parentKobject,
                    ThreadConfig         **threadConfigPointer,
                    char                 **reason,
                    KernelLayer          **layerPtr)
{
  // VDO-3769 - Set a generic reason so we don't ever return garbage.
  *reason = "Unspecified error";

  /*
   * Part 1 - Allocate the kernel layer, its essential parts, and setup up the
   * sysfs node.  These must come first so that the sysfs node works correctly
   * through the freeing of the kernel layer.  After this part you must use
   * freeKernelLayer.
   */
  KernelLayer *layer;
  int result = ALLOCATE(1, KernelLayer, "VDO configuration", &layer);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot allocate VDO configuration";
    return result;
  }

  // Allow the base VDO to allocate buffers and construct or destroy
  // enqueuables as part of its allocation.
  layer->common.allocateIOBuffer   = kvdoAllocateIOBuffer;
  layer->common.createEnqueueable  = kvdoCreateEnqueueable;
  layer->common.destroyEnqueueable = kvdoDestroyEnqueueable;

  result = allocateVDO(&layer->common, &layer->kvdo.vdo);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot allocate VDO";
    FREE(layer);
    return result;
  }

  // After this point, calling kobject_put on kobj will decrement its
  // reference count, and when the count goes to 0 the KernelLayer will
  // be freed.
  kobject_init(&layer->kobj, &kernel_layer_kobj_type);
  result = kobject_add(&layer->kobj, parentKobject, config->pool_name);
  if (result != 0) {
    *reason = "Cannot add sysfs node";
    kobject_put(&layer->kobj);
    return result;
  }
  kobject_init(&layer->wqDirectory, &work_queue_directory_kobj_type);
  result = kobject_add(&layer->wqDirectory, &layer->kobj, "work_queues");
  if (result != 0) {
    *reason = "Cannot add sysfs node";
    kobject_put(&layer->wqDirectory);
    kobject_put(&layer->kobj);
    return result;
  }

  /*
   * Part 2 - Do all the simple initialization.  These initializations have no
   * order dependencies and can be done in any order, but freeKernelLayer()
   * cannot be called until all the simple layer properties are set.
   *
   * The KernelLayer structure starts as all zeros.  Pointer initializations
   * consist of replacing a NULL pointer with a non-NULL pointer, which can be
   * easily undone by freeing all of the non-NULL pointers (using the proper
   * free routine).
   */
  setKernelLayerState(layer, LAYER_SIMPLE_THINGS_INITIALIZED);

  initialize_deadlock_queue(&layer->deadlockQueue);

  int requestLimit = default_max_requests_active;
  initialize_limiter(&layer->requestLimiter, requestLimit);
  initialize_limiter(&layer->discardLimiter, requestLimit * 3 / 4);

  layer->allocationsAllowed   = true;
  layer->instance             = instance;
  layer->deviceConfig         = config;
  layer->startingSectorOffset = startingSector;

  layer->common.getBlockCount            = kvdoGetBlockCount;
  layer->common.isFlushRequired          = isFlushRequired;
  layer->common.createMetadataVIO        = kvdo_create_metadata_vio;
  layer->common.createCompressedWriteVIO = kvdo_create_compressed_write_vio;
  layer->common.completeFlush            = kvdo_complete_flush;
  layer->common.enqueue                  = kvdoEnqueue;
  layer->common.waitForAdminOperation    = waitForSyncOperation;
  layer->common.completeAdminOperation   = kvdoCompleteSyncOperation;
  layer->common.flush                    = kvdo_flush_vio;
  spin_lock_init(&layer->flushLock);
  mutex_init(&layer->statsMutex);
  bio_list_init(&layer->waitingFlushes);
  // This can go anywhere, as long as it is not registered until the layer
  // is fully allocated.
  layer->callbacks.congested_fn          = kvdoIsCongested;

  result = add_layer_to_device_registry(config->pool_name, layer);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot add layer to device registry";
    freeKernelLayer(layer);
    return result;
  }

  snprintf(layer->threadNamePrefix, sizeof(layer->threadNamePrefix), "%s%u",
           THIS_MODULE->name, instance);

  result    = makeThreadConfig(config->thread_counts.logical_zones,
                            config->thread_counts.physical_zones,
                            config->thread_counts.hash_zones,
                            threadConfigPointer);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot create thread configuration";
    freeKernelLayer(layer);
    return result;
  }

  logInfo("zones: %d logical, %d physical, %d hash; base threads: %d",
          config->thread_counts.logical_zones,
          config->thread_counts.physical_zones,
          config->thread_counts.hash_zones,
          (*threadConfigPointer)->baseThreadCount);

  result = make_batch_processor(layer, returnDataKVIOBatchToPool, layer,
                                &layer->dataKVIOReleaser);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot allocate KVIO-freeing batch processor";
    freeKernelLayer(layer);
    return result;
  }

  // Spare KVDOFlush, so that we will always have at least one available
  result = make_kvdo_flush(&layer->spareKVDOFlush);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot allocate KVDOFlush record";
    freeKernelLayer(layer);
    return result;
  }

  // Read the geometry block so we know how to set up the index. Allow it to
  // do synchronous reads.
  layer->common.reader = kvdoSynchronousRead;
  result = loadVolumeGeometry(&layer->common, &layer->geometry);
  layer->common.reader = NULL;
  if (result != VDO_SUCCESS) {
    *reason = "Could not load geometry block";
    freeKernelLayer(layer);
    return result;
  }

  // Dedupe Index
  BUG_ON(layer->threadNamePrefix[0] == '\0');
  result = makeDedupeIndex(&layer->dedupeIndex, layer);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot initialize dedupe index";
    freeKernelLayer(layer);
    return result;
  }

  // Compression context storage
  result = ALLOCATE(config->thread_counts.cpu_threads, char *, "LZ4 context",
                    &layer->compressionContext);
  if (result != VDO_SUCCESS) {
    *reason = "cannot allocate LZ4 context";
    freeKernelLayer(layer);
    return result;
  }
  for (int i = 0; i < config->thread_counts.cpu_threads; i++) {
    result = ALLOCATE(LZ4_context_size(), char, "LZ4 context",
                      &layer->compressionContext[i]);
    if (result != VDO_SUCCESS) {
      *reason = "cannot allocate LZ4 context";
      freeKernelLayer(layer);
      return result;
    }
  }


  /*
   * Part 3 - Do initializations that depend upon other previous
   * initializations, but have no order dependencies at freeing time.
   * Order dependencies for initialization are identified using BUG_ON.
   */
  setKernelLayerState(layer, LAYER_BUFFER_POOLS_INITIALIZED);

  // Trace pool
  BUG_ON(layer->requestLimiter.limit <= 0);
  result = traceKernelLayerInit(layer);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot initialize trace data";
    freeKernelLayer(layer);
    return result;
  }

  // KVIO and VIO pool
  BUG_ON(layer->deviceConfig->logical_block_size <= 0);
  BUG_ON(layer->requestLimiter.limit <= 0);
  BUG_ON(layer->deviceConfig->owned_device == NULL);
  result = makeDataKVIOBufferPool(layer, layer->requestLimiter.limit,
                                  &layer->dataKVIOPool);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot allocate vio data";
    freeKernelLayer(layer);
    return result;
  }

  /*
   * Part 4 - Do initializations that depend upon other previous
   * initialization, that may have order dependencies at freeing time.
   * These are mostly starting up the workqueue threads.
   */

  // Base-code thread, etc
  result = initializeKVDO(&layer->kvdo, *threadConfigPointer, reason);
  if (result != VDO_SUCCESS) {
    freeKernelLayer(layer);
    return result;
  }

  setKernelLayerState(layer, LAYER_REQUEST_QUEUE_INITIALIZED);

  // Bio queue
  result    = make_io_submitter(layer->threadNamePrefix,
                                config->thread_counts.bio_threads,
                                config->thread_counts.bio_rotation_interval,
                                layer->requestLimiter.limit,
                                layer,
                                &layer->ioSubmitter);
  if (result != VDO_SUCCESS) {
    // If initialization of the bio-queues failed, they are cleaned
    // up already, so just free the rest of the kernel layer.
    freeKernelLayer(layer);
    *reason = "bio submission initialization failed";
    return result;
  }
  setKernelLayerState(layer, LAYER_BIO_DATA_INITIALIZED);

  // Bio ack queue
  if (useBioAckQueue(layer)) {
    result = make_work_queue(layer->threadNamePrefix, "ackQ",
                             &layer->wqDirectory, layer, layer, &bioAckQType,
                             config->thread_counts.bio_ack_threads, NULL,
                             &layer->bioAckQueue);
    if (result != VDO_SUCCESS) {
      *reason = "bio ack queue initialization failed";
      freeKernelLayer(layer);
      return result;
    }
  }

  setKernelLayerState(layer, LAYER_BIO_ACK_QUEUE_INITIALIZED);

  // CPU Queues
  result = make_work_queue(layer->threadNamePrefix, "cpuQ",
                           &layer->wqDirectory,
                           layer, layer, &cpuQType,
                           config->thread_counts.cpu_threads,
                           (void **) layer->compressionContext,
                           &layer->cpuQueue);
  if (result != VDO_SUCCESS) {
    *reason = "CPU queue initialization failed";
    freeKernelLayer(layer);
    return result;
  }

  setKernelLayerState(layer, LAYER_CPU_QUEUE_INITIALIZED);

  *layerPtr = layer;
  return VDO_SUCCESS;
}

/**********************************************************************/
int prepareToModifyKernelLayer(KernelLayer           *layer,
                               struct device_config  *config,
                               char                 **errorPtr)
{
  struct device_config *extantConfig = layer->deviceConfig;
  if (config->owning_target->begin != extantConfig->owning_target->begin) {
    *errorPtr = "Starting sector cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (strcmp(config->parent_device_name,
             extantConfig->parent_device_name) != 0) {
    *errorPtr = "Underlying device cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->logical_block_size != extantConfig->logical_block_size) {
    *errorPtr = "Logical block size cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->cache_size != extantConfig->cache_size) {
    *errorPtr = "Block map cache size cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->block_map_maximum_age != extantConfig->block_map_maximum_age) {
    *errorPtr = "Block map maximum age cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->md_raid5_mode_enabled != extantConfig->md_raid5_mode_enabled) {
    *errorPtr = "mdRaid5Mode cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (memcmp(&config->thread_counts, &extantConfig->thread_counts,
	     sizeof(struct thread_count_config)) != 0) {
    *errorPtr = "Thread configuration cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  // Below here are the actions to take when a non-immutable property changes.

  if (config->write_policy != extantConfig->write_policy) {
    // Nothing needs doing right now for a write policy change.
  }

  if (config->owning_target->len != extantConfig->owning_target->len) {
    size_t logicalBytes = to_bytes(config->owning_target->len);
    if ((logicalBytes % VDO_BLOCK_SIZE) != 0) {
      *errorPtr = "Logical size must be a multiple of 4096";
      return VDO_PARAMETER_MISMATCH;
    }

    int result = prepareToResizeLogical(layer, logicalBytes / VDO_BLOCK_SIZE);
    if (result != VDO_SUCCESS) {
      *errorPtr = "Device prepareToGrowLogical failed";
      return result;
    }
  }

  if (config->physical_blocks != extantConfig->physical_blocks) {
    int result = prepareToResizePhysical(layer, config->physical_blocks);
    if (result != VDO_SUCCESS) {
      *errorPtr = "Device prepareToGrowPhysical failed";
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
int modifyKernelLayer(KernelLayer          *layer,
                      struct device_config *config)
{
  struct device_config *extantConfig = layer->deviceConfig;

  // A failure here is unrecoverable. So there is no problem if it happens.

  if (config->write_policy != extantConfig->write_policy) {
    /*
     * Ordinarily, when going from async to sync, we must flush any metadata
     * written. However, because the underlying storage must have gone into
     * sync mode before we suspend VDO, and suspending VDO concludes by
     * issuing a flush, all metadata written before the suspend is flushed
     * by the suspend and all metadata between the suspend and the write
     * policy change is written to synchronous storage.
     */
    logInfo("Modifying device '%s' write policy from %s to %s",
            config->pool_name, get_config_write_policy_string(extantConfig),
            get_config_write_policy_string(config));
    setWritePolicy(layer->kvdo.vdo, config->write_policy);
  }

  if (config->owning_target->len != extantConfig->owning_target->len) {
    size_t logicalBytes = to_bytes(config->owning_target->len);
    int result = resizeLogical(layer, logicalBytes / VDO_BLOCK_SIZE);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  // Grow physical if the version is 0, so we can't tell if we
  // got an old-style growPhysical command, or if size changed.
  if ((config->physical_blocks != extantConfig->physical_blocks)
      || (config->version == 0)) {
    int result = resizePhysical(layer, config->physical_blocks);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
void freeKernelLayer(KernelLayer *layer)
{
  // This is not the cleanest implementation, but given the current timing
  // uncertainties in the shutdown process for work queues, we need to
  // store information to enable a late-in-process deallocation of
  // funnel-queue data structures in work queues.
  bool usedBioAckQueue = false;
  bool usedCpuQueue    = false;
  bool usedKVDO        = false;
  bool releaseInstance = false;

  KernelLayerState state = getKernelLayerState(layer);
  switch (state) {
  case LAYER_STOPPING:
    logError("re-entered freeKernelLayer while stopping");
    break;

  case LAYER_RUNNING:
  case LAYER_SUSPENDED:
    stopKernelLayer(layer);
    // fall through

  case LAYER_STOPPED:
  case LAYER_CPU_QUEUE_INITIALIZED:
    finish_work_queue(layer->cpuQueue);
    usedCpuQueue = true;
    releaseInstance = true;
    // fall through

  case LAYER_BIO_ACK_QUEUE_INITIALIZED:
    if (useBioAckQueue(layer)) {
      finish_work_queue(layer->bioAckQueue);
      usedBioAckQueue = true;
    }
    // fall through

  case LAYER_BIO_DATA_INITIALIZED:
    cleanup_io_submitter(layer->ioSubmitter);
    // fall through

  case LAYER_REQUEST_QUEUE_INITIALIZED:
    finishKVDO(&layer->kvdo);
    usedKVDO = true;
    // fall through

  case LAYER_BUFFER_POOLS_INITIALIZED:
    free_buffer_pool(&layer->dataKVIOPool);
    free_buffer_pool(&layer->traceBufferPool);
    // fall through

  case LAYER_SIMPLE_THINGS_INITIALIZED:
    if (layer->compressionContext != NULL) {
      for (int i = 0; i < layer->deviceConfig->thread_counts.cpu_threads; i++) {
        FREE(layer->compressionContext[i]);
      }
      FREE(layer->compressionContext);
    }
    if (layer->dedupeIndex != NULL) {
      finishDedupeIndex(layer->dedupeIndex);
    }
    FREE(layer->spareKVDOFlush);
    layer->spareKVDOFlush = NULL;
    free_batch_processor(&layer->dataKVIOReleaser);
    remove_layer_from_device_registry(layer->deviceConfig->pool_name);
    break;

  default:
    logError("Unknown Kernel Layer state: %d", state);
  }

  // Late deallocation of resources in work queues.
  if (usedCpuQueue) {
    free_work_queue(&layer->cpuQueue);
  }
  if (usedBioAckQueue) {
    free_work_queue(&layer->bioAckQueue);
  }
  if (layer->ioSubmitter) {
    free_io_submitter(layer->ioSubmitter);
  }
  if (usedKVDO) {
    destroyKVDO(&layer->kvdo);
  }

  freeDedupeIndex(&layer->dedupeIndex);

  if (releaseInstance) {
    release_kvdo_instance(layer->instance);
  }

  // The call to kobject_put on the kobj sysfs node will decrement its
  // reference count; when the count goes to zero the VDO object and
  // the kernel layer object will be freed as a side effect.
  kobject_put(&layer->wqDirectory);
  kobject_put(&layer->kobj);
}

/**********************************************************************/
static void poolStatsRelease(struct kobject *kobj)
{
  KernelLayer *layer = container_of(kobj, KernelLayer, statsDirectory);
  complete(&layer->statsShutdown);
}

/**********************************************************************/
int startKernelLayer(KernelLayer          *layer,
                     const VDOLoadConfig  *loadConfig,
                     char                **reason)
{
  ASSERT_LOG_ONLY(getKernelLayerState(layer) == LAYER_CPU_QUEUE_INITIALIZED,
                  "startKernelLayer may only be invoked after initialization");
  setKernelLayerState(layer, LAYER_RUNNING);

  int result = startKVDO(&layer->kvdo, &layer->common, loadConfig,
                         layer->vioTraceRecording, reason);
  if (result != VDO_SUCCESS) {
    stopKernelLayer(layer);
    return result;
  }

  static struct kobj_type statsDirectoryKobjType = {
    .release       = poolStatsRelease,
    .sysfs_ops     = &pool_stats_sysfs_ops,
    .default_attrs = pool_stats_attrs,
  };
  kobject_init(&layer->statsDirectory, &statsDirectoryKobjType);
  result = kobject_add(&layer->statsDirectory, &layer->kobj, "statistics");
  if (result != 0) {
    *reason = "Cannot add sysfs statistics node";
    stopKernelLayer(layer);
    return result;
  }
  layer->statsAdded = true;

  // Don't try to load or rebuild the index first (and log scary error
  // messages) if this is known to be a newly-formatted volume.
  startDedupeIndex(layer->dedupeIndex, wasNew(layer->kvdo.vdo));

  result = vdo_create_procfs_entry(layer, layer->deviceConfig->pool_name,
                                   &layer->procfsPrivate);
  if (result != VDO_SUCCESS) {
    *reason = "Could not create proc filesystem entry";
    stopKernelLayer(layer);
    return result;
  }

  layer->allocationsAllowed = false;

  return VDO_SUCCESS;
}

/**********************************************************************/
int stopKernelLayer(KernelLayer *layer)
{
  char errorName[80] = "";
  char errorMessage[ERRBUF_SIZE] = "";

  layer->allocationsAllowed = true;

  // Stop services that need to gather VDO statistics from the worker threads.
  if (layer->statsAdded) {
    layer->statsAdded = false;
    init_completion(&layer->statsShutdown);
    kobject_put(&layer->statsDirectory);
    wait_for_completion(&layer->statsShutdown);
  }
  vdo_destroy_procfs_entry(layer->deviceConfig->pool_name,
                           layer->procfsPrivate);

  int result = stopKVDO(&layer->kvdo);
  if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
    logError("%s: Close device failed %d (%s: %s)",
             __func__, result,
             string_error_name(result, errorName, sizeof(errorName)),
             string_error(result, errorMessage, sizeof(errorMessage)));
  }

  switch (getKernelLayerState(layer)) {
  case LAYER_SUSPENDED:
  case LAYER_RUNNING:
    setKernelLayerState(layer, LAYER_STOPPING);
    stopDedupeIndex(layer->dedupeIndex);
    // fall through

  case LAYER_STOPPING:
  case LAYER_STOPPED:
  default:
    setKernelLayerState(layer, LAYER_STOPPED);
  }

  return result;
}

/**********************************************************************/
int suspendKernelLayer(KernelLayer *layer)
{
  // It's important to note any error here does not actually stop device-mapper
  // from suspending the device. All this work is done post suspend.
  KernelLayerState  state = getKernelLayerState(layer);
  if (state == LAYER_SUSPENDED) {
    return VDO_SUCCESS;
  }
  if (state != LAYER_RUNNING) {
    logError("Suspend invoked while in unexpected kernel layer state %d",
             state);
    return -EINVAL;
  }

  setKernelLayerState(layer, LAYER_SUSPENDED);

  /*
   * Attempt to flush all I/O before completing post suspend work. This is
   * needed so that changing write policy upon resume is safe. Also, we think
   * a suspended device is expected to have persisted all data written before
   * the suspend, even if it hasn't been flushed yet.
   */
  waitForNoRequestsActive(layer);
  int result = synchronous_flush(layer);
  if (result != VDO_SUCCESS) {
    setKVDOReadOnly(&layer->kvdo, result);
  }
  suspendDedupeIndex(layer->dedupeIndex, !layer->noFlushSuspend);
  return result;
}

/**********************************************************************/
int resumeKernelLayer(KernelLayer *layer)
{
  if (getKernelLayerState(layer) == LAYER_SUSPENDED) {
    setKernelLayerState(layer, LAYER_RUNNING);
  }
  return VDO_SUCCESS;
}

/***********************************************************************/
int prepareToResizePhysical(KernelLayer *layer, BlockCount physicalCount)
{
  logInfo("Preparing to resize physical to %llu", physicalCount);
  // Allocations are allowed and permissible through this non-VDO thread,
  // since IO triggered by this allocation to VDO can finish just fine.
  int result = kvdoPrepareToGrowPhysical(&layer->kvdo, physicalCount);
  if (result != VDO_SUCCESS) {
    // kvdoPrepareToGrowPhysical logs errors.
    if (result == VDO_PARAMETER_MISMATCH) {
      // If we don't trap this case, mapToSystemError() will remap it to -EIO,
      // which is misleading and ahistorical.
      return -EINVAL;
    } else {
      return result;
    }
  }

  logInfo("Done preparing to resize physical");
  return VDO_SUCCESS;
}

/***********************************************************************/
int resizePhysical(KernelLayer *layer, BlockCount physicalCount)
{
  // We must not mark the layer as allowing allocations when it is suspended
  // lest an allocation attempt block on writing IO to the suspended VDO.
  int result = kvdoResizePhysical(&layer->kvdo, physicalCount);
  if (result != VDO_SUCCESS) {
    // kvdoResizePhysical logs errors
    return result;
  }
  return VDO_SUCCESS;
}

/***********************************************************************/
int prepareToResizeLogical(KernelLayer *layer, BlockCount logicalCount)
{
  logInfo("Preparing to resize logical to %llu", logicalCount);
  // Allocations are allowed and permissible through this non-VDO thread,
  // since IO triggered by this allocation to VDO can finish just fine.
  int result = kvdoPrepareToGrowLogical(&layer->kvdo, logicalCount);
  if (result != VDO_SUCCESS) {
    // kvdoPrepareToGrowLogical logs errors
    return result;
  }

  logInfo("Done preparing to resize logical");
  return VDO_SUCCESS;
}

/***********************************************************************/
int resizeLogical(KernelLayer *layer, BlockCount logicalCount)
{
  logInfo("Resizing logical to %llu", logicalCount);
  // We must not mark the layer as allowing allocations when it is suspended
  // lest an allocation attempt block on writing IO to the suspended VDO.
  int result = kvdoResizeLogical(&layer->kvdo, logicalCount);
  if (result != VDO_SUCCESS) {
    // kvdoResizeLogical logs errors
    return result;
  }

  logInfo("Logical blocks now %llu", logicalCount);
  return VDO_SUCCESS;
}

