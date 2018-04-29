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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/kernelLayer.c#3 $
 */

#include "kernelLayer.h"

#include <linux/crc32.h>
#include <linux/module.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "lz4.h"
#include "releaseVersions.h"
#include "volumeGeometry.h"
#include "statistics.h"
#include "vdo.h"

#include "bio.h"
#include "dataKVIO.h"
#include "dedupeIndex.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "ioSubmitterInternals.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "murmur/MurmurHash3.h"
#include "poolSysfs.h"
#include "readCache.h"
#include "statusProcfs.h"
#include "stringUtils.h"
#include "verify.h"

enum {
  DEDUPE_TIMEOUT_REPORT_INTERVAL = 1000,
};

static const KvdoWorkQueueType bioAckQType = {
  .actionTable = {
    { .name = "bio_ack",
      .code = BIO_ACK_Q_ACTION_ACK,
      .priority = 0 },
  },
};

static const KvdoWorkQueueType cpuQType = {
  .actionTable = {
    { .name = "cpu_compare_blocks",
      .code = CPU_Q_ACTION_COMPARE_BLOCKS,
      .priority = 0 },
    { .name = "cpu_complete_kvio",
      .code = CPU_Q_ACTION_COMPLETE_KVIO,
      .priority = 0 },
    { .name = "cpu_compress_block",
      .code = CPU_Q_ACTION_COMPRESS_BLOCK,
      .priority = 0 },
    { .name = "cpu_dedupe_shutdown",
      .code = CPU_Q_ACTION_DEDUPE_SHUTDOWN,
      .priority = 1 },
    { .name = "cpu_hash_block",
      .code = CPU_Q_ACTION_HASH_BLOCK,
      .priority = 0 },
    { .name = "cpu_event_reporter",
      .code = CPU_Q_ACTION_EVENT_REPORTER,
      .priority = 0 },
    { .name = "cpu_set_up_verify",
      .code = CPU_Q_ACTION_SET_UP_VERIFY,
      .priority = 0
    },
  },
};

// 2000 is half the number of entries currently in our page cache,
// to allow for each in-progress operation to update two pages.
int defaultMaxRequestsActive = 2000;

/**********************************************************************/
static CRC32Checksum kvdoUpdateCRC32(CRC32Checksum  crc,
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
  return asKernelLayer(header)->blockCount;
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
    logInfo("%s: mapping errno value %d used without negation",
            __func__, error);
    return -error;
  }
  // VDO or UDS error
  char errorName[80], errorMessage[ERRBUF_SIZE];
  switch (sansUnrecoverable(error)) {
  case VDO_NO_SPACE:
    return -ENOSPC;
  case VDO_READ_ONLY:
    return -EIO;
  default:
    logInfo("%s: mapping internal status code %d (%s: %s) to EIO",
            __func__, error,
            stringErrorName(error, errorName, sizeof(errorName)),
            stringError(error, errorMessage, sizeof(errorMessage)));
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
  if (limiterIsIdle(&layer->requestLimiter)) {
    return;
  }

  // We have to make sure to flush the packer before waiting. We do this
  // by turning off compression, which also means no new entries coming in
  // while waiting will end up in the packer.
  bool wasCompressing = setKVDOCompressing(&layer->kvdo, false);
  // Now wait for there to be no active requests
  limiterWaitForIdle(&layer->requestLimiter);
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
                                       BIO         *bio,
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
  if (!limiterPoll(&layer->requestLimiter)) {
    addToDeadlockQueue(&layer->deadlockQueue, bio, arrivalTime);
    logWarning("queued an I/O request to avoid deadlock!");

    return DM_MAPIO_SUBMITTED;
  }

  bool hasDiscardPermit
    = (isDiscardBio(bio) && limiterPoll(&layer->discardLimiter));
  int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                         hasDiscardPermit);
  // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
  if (result != VDO_SUCCESS) {
    return result;
  }

  return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
int kvdoMapBio(KernelLayer *layer, BIO *bio)
{
  Jiffies          arrivalTime = jiffies;
  KernelLayerState state       = getKernelLayerState(layer);
  ASSERT_LOG_ONLY(state == LAYER_RUNNING,
                  "kvdoMapBio should not be called while in state %d", state);

  // Count all incoming bios.
  countBios(&layer->biosIn, bio);

  // Handle empty bios.  Empty flush bios are not associated with a VIO.
  if (isEmptyFlush(bio)) {
    if (shouldProcessFlush(layer)) {
      launchKVDOFlush(layer, bio);
      return DM_MAPIO_SUBMITTED;
    } else {
      // We're not acknowledging this bio now, but we'll never touch it
      // again, so this is the last chance to account for it.
      countBios(&layer->biosAcknowledged, bio);
      atomic64_inc(&layer->flushOut);
      setBioBlockDevice(bio, layer->dev->bdev);
      return DM_MAPIO_REMAPPED;
    }
  }
  if (isDiscardBio(bio) && isReadBio(bio)) {
    // Read and Discard should never occur together
    return -EIO;
  }

  KvdoWorkQueue *currentWorkQueue = getCurrentWorkQueue();
  if ((currentWorkQueue != NULL)
      && (layer == getWorkQueueOwner(currentWorkQueue))) {
    /*
     * This prohibits sleeping during I/O submission to VDO from its own
     * thread.
     */
    return launchDataKVIOFromVDOThread(layer, bio, arrivalTime);
  }
  bool hasDiscardPermit = false;
  if (isDiscardBio(bio)) {
    limiterWaitForOneFree(&layer->discardLimiter);
    hasDiscardPermit = true;
  }
  limiterWaitForOneFree(&layer->requestLimiter);

  int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                         hasDiscardPermit);
  // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
  if (result != VDO_SUCCESS) {
    return result;
  }

  return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
void completeManyRequests(KernelLayer *layer, uint32_t count)
{
  // If we had to buffer some requests to avoid deadlock, release them now.
  while (count > 0) {
    Jiffies arrivalTime = 0;
    BIO *bio = pollDeadlockQueue(&layer->deadlockQueue, &arrivalTime);
    if (likely(bio == NULL)) {
      break;
    }

    bool hasDiscardPermit
      = (isDiscardBio(bio) && limiterPoll(&layer->discardLimiter));
    int result = kvdoLaunchDataKVIOFromBio(layer, bio, arrivalTime,
                                           hasDiscardPermit);
    if (result != VDO_SUCCESS) {
      completeBio(bio, result);
    }
    // Succeed or fail, kvdoLaunchDataKVIOFromBio owns the permit(s) now.
    count--;
  }
  // Notify the limiter, so it can wake any blocked processes.
  if (count > 0) {
    limiterReleaseMany(&layer->requestLimiter, count);
  }
}

/**********************************************************************/
static void reportEvents(PeriodicEventReporter *reporter)
{
  atomic_set(&reporter->workItemQueued, 0);
  uint64_t newValue = atomic64_read(&reporter->value);
  uint64_t difference = newValue - reporter->lastReportedValue;
  if (difference != 0) {
    logDebug(reporter->format, difference);
    reporter->lastReportedValue = newValue;
  }
}

/**********************************************************************/
static void reportEventsWork(KvdoWorkItem *item)
{
  PeriodicEventReporter *reporter = container_of(item, PeriodicEventReporter,
                                                 workItem);
  reportEvents(reporter);
}

/**********************************************************************/
static void initPeriodicEventReporter(PeriodicEventReporter *reporter,
                                      const char            *format,
                                      unsigned long          reportingInterval,
                                      KernelLayer           *layer)
{
  setupWorkItem(&reporter->workItem, reportEventsWork, NULL,
                CPU_Q_ACTION_EVENT_REPORTER);
  reporter->format            = format;
  reporter->reportingInterval = msecs_to_jiffies(reportingInterval);
  reporter->layer             = layer;
}

/**********************************************************************/
static void addEventCount(PeriodicEventReporter *reporter, unsigned int count)
{
  if (count > 0) {
    atomic64_add(count, &reporter->value);
    int oldWorkItemQueued = atomic_xchg(&reporter->workItemQueued, 1);
    if (oldWorkItemQueued == 0) {
      enqueueWorkQueueDelayed(reporter->layer->cpuQueue,
                              &reporter->workItem,
                              jiffies + reporter->reportingInterval);
    }
  }
}

/**********************************************************************/
static void stopPeriodicEventReporter(PeriodicEventReporter *reporter)
{
  reportEvents(reporter);
}

/**********************************************************************/
void kvdoReportDedupeTimeout(KernelLayer *layer, unsigned int expiredCount)
{
  addEventCount(&layer->albireoTimeoutReporter, expiredCount);
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
  KvdoEnqueueable *kvdoEnqueueable = container_of(*enqueueablePtr,
                                                  KvdoEnqueueable,
                                                  enqueueable);
  FREE(kvdoEnqueueable);
  *enqueueablePtr = NULL;
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
/**
 * Callback for a bio which did a read to populate a cache entry.
 *
 * @param bio    The bio
 */
static void endSyncRead(BIO *bio)
#else
/**
 * Callback for a bio which did a read to populate a cache entry.
 *
 * @param bio     The bio
 * @param result  The result of the read operation
 */
static void endSyncRead(BIO *bio, int result)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  int result = bio->bi_error;
#endif

  if (result != 0) {
    logErrorWithStringError(result, "synchronous read failed");
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,4,0)
    clear_bit(BIO_UPTODATE, &bio->bi_flags);
#endif
  }

  struct completion *completion = (struct completion *) bio->bi_private;
  complete(completion);
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

  struct completion bioWait;
  init_completion(&bioWait);
  BIO *bio;
  int result = createBio(kernelLayer, buffer, &bio);
  if (result != VDO_SUCCESS) {
    return result;
  }
  bio->bi_rw      = READ;
  bio->bi_end_io  = endSyncRead;
  bio->bi_private = &bioWait;
  setBioBlockDevice(bio, kernelLayer->dev->bdev);
  setBioSector(bio, blockToSector(kernelLayer, startBlock));
  generic_make_request(bio);
  wait_for_completion(&bioWait);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,4,0)
  if (bio->bi_error != 0) {
#else
  if (!bio_flagged(bio, BIO_UPTODATE)) {
#endif
    result = -EIO;
  }

  freeBio(bio, kernelLayer);

  if (result != VDO_SUCCESS) {
    return result;
  }
  if (blocksRead != NULL) {
    *blocksRead = blockCount;
  }
  return VDO_SUCCESS;
}

/**
 * Implements VIODestructor.
 **/
static void kvdoFreeVIO(VIO **vioPtr)
{
  VIO *vio = *vioPtr;
  if (vio == NULL) {
    return;
  }

  BUG_ON(isDataVIO(vio));

  if (isCompressedWriteVIO(vio)) {
    CompressedWriteKVIO *compressedWriteKVIO
      = allocatingVIOAsCompressedWriteKVIO(vioAsAllocatingVIO(vio));
    freeCompressedWriteKVIO(&compressedWriteKVIO);
  } else {
    MetadataKVIO *metadataKVIO = vioAsMetadataKVIO(vio);
    freeMetadataKVIO(&metadataKVIO);
  }

  *vioPtr = NULL;
}

/**********************************************************************/
static bool isFlushRequired(PhysicalLayer *common)
{
  return shouldProcessFlush(asKernelLayer(common));
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

/**********************************************************************/
int makeKernelLayer(BlockCount      blockCount,
                    uint64_t        startingSector,
                    struct dm_dev  *dev,
                    unsigned int    instance,
                    DeviceConfig   *config,
                    struct kobject *parentKobject,
                    ThreadConfig  **threadConfigPointer,
                    char          **reason,
                    KernelLayer   **layerPtr)
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
    freeDeviceConfig(&config);
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
    freeDeviceConfig(&config);
    FREE(layer);
    return result;
  }

  // After this point, calling kobject_put on kobj will decrement its
  // reference count, and when the count goes to 0 the KernelLayer will
  // be freed.
  kobject_init(&layer->kobj, &kernelLayerKobjType);
  result = kobject_add(&layer->kobj, parentKobject, config->poolName);
  if (result != 0) {
    *reason = "Cannot add sysfs node";
    freeDeviceConfig(&config);
    kobject_put(&layer->kobj);
    return result;
  }
  kobject_init(&layer->wqDirectory, &workQueueDirectoryKobjType);
  result = kobject_add(&layer->wqDirectory, &layer->kobj, "work_queues");
  if (result != 0) {
    *reason = "Cannot add sysfs node";
    freeDeviceConfig(&config);
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

  initializeDeadlockQueue(&layer->deadlockQueue);

  int requestLimit = defaultMaxRequestsActive;
  initializeLimiter(&layer->requestLimiter, requestLimit);
  initializeLimiter(&layer->discardLimiter, requestLimit * 3 / 4);
  layer->readCacheBlocks
    = (config->readCacheEnabled
       ? (requestLimit + config->readCacheExtraBlocks) : 0);

  layer->allocationsAllowed   = true;
  layer->dev                  = dev;
  layer->blockCount           = blockCount;
  layer->instance             = instance;
  layer->logicalBlockSize     = config->logicalBlockSize;
  layer->mdRaid5ModeEnabled   = config->mdRaid5ModeEnabled;
  layer->deviceConfig         = config;
  layer->startingSectorOffset = startingSector;

  layer->common.updateCRC32              = kvdoUpdateCRC32;
  layer->common.getBlockCount            = kvdoGetBlockCount;
  layer->common.isFlushRequired          = isFlushRequired;
  layer->common.createMetadataVIO        = kvdoCreateMetadataVIO;
  layer->common.createCompressedWriteVIO = kvdoCreateCompressedWriteVIO;
  layer->common.freeVIO                  = kvdoFreeVIO;
  layer->common.completeFlush            = kvdoCompleteFlush;
  layer->common.enqueue                  = kvdoEnqueue;
  layer->common.waitForAdminOperation    = waitForSyncOperation;
  layer->common.completeAdminOperation   = kvdoCompleteSyncOperation;
  layer->common.getCurrentThreadID       = kvdoGetCurrentThreadID;
  layer->common.zeroDataVIO              = kvdoZeroDataVIO;
  layer->common.compareDataVIOs          = kvdoCompareDataVIOs;
  layer->common.copyData                 = kvdoCopyDataVIO;
  layer->common.readData                 = kvdoReadDataVIO;
  layer->common.writeData                = kvdoWriteDataVIO;
  layer->common.writeCompressedBlock     = kvdoWriteCompressedBlock;
  layer->common.readMetadata             = kvdoSubmitMetadataVIO;
  layer->common.writeMetadata            = kvdoSubmitMetadataVIO;
  layer->common.applyPartialWrite        = kvdoModifyWriteDataVIO;
  layer->common.flush                    = kvdoFlushVIO;
  layer->common.hashData                 = kvdoHashDataVIO;
  layer->common.checkForDuplication      = kvdoCheckForDuplication;
  layer->common.verifyDuplication        = kvdoVerifyDuplication;
  layer->common.acknowledgeDataVIO       = kvdoAcknowledgeDataVIO;
  layer->common.compressDataVIO          = kvdoCompressDataVIO;
  layer->common.updateAlbireo            = kvdoUpdateDedupeAdvice;

  spin_lock_init(&layer->flushLock);
  mutex_init(&layer->statsMutex);
  bio_list_init(&layer->waitingFlushes);

  result = addLayerToDeviceRegistry(config->poolName, layer);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot add layer to device registry";
    freeKernelLayer(layer);
    return result;
  }

  snprintf(layer->threadNamePrefix, sizeof(layer->threadNamePrefix), "%s%u",
           THIS_MODULE->name, instance);

  result = makeThreadConfig(config->threadCounts.logicalZones,
                            config->threadCounts.physicalZones,
                            config->threadCounts.hashZones,
                            threadConfigPointer);
  if (result != VDO_SUCCESS) {
    *reason = "Cannot create thread configuration";
    freeKernelLayer(layer);
    return result;
  }

  config->threadCounts.baseThreads = (*threadConfigPointer)->baseThreadCount;

  logInfo("zones: %d logical, %d physical, %d hash; base threads: %d",
          config->threadCounts.logicalZones,
          config->threadCounts.physicalZones,
          config->threadCounts.hashZones,
          config->threadCounts.baseThreads);

  result = makeBatchProcessor(layer, returnDataKVIOBatchToPool, layer,
                              &layer->dataKVIOReleaser);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot allocate KVIO-freeing batch processor";
    freeKernelLayer(layer);
    return result;
  }

  // Spare KVDOFlush, so that we will always have at least one available
  result = makeKVDOFlush(&layer->spareKVDOFlush);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot allocate KVDOFlush record";
    freeKernelLayer(layer);
    return result;
  }

  // BIO pool (needed before the geometry block)
  layer->bioset = bioset_create(0, 0);
  if (layer->bioset == NULL) {
    *reason = "Cannot allocate dedupe bioset";
    freeKernelLayer(layer);
    return -ENOMEM;
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

  // Albireo Timeout Reporter
  initPeriodicEventReporter(&layer->albireoTimeoutReporter,
                            "Albireo timeout on %" PRIu64 " requests",
                            DEDUPE_TIMEOUT_REPORT_INTERVAL, layer);

  // Dedupe Index
  BUG_ON(layer->threadNamePrefix[0] == '\0');
  result = makeDedupeIndex(&layer->dedupeIndex, layer);
  if (result != UDS_SUCCESS) {
    *reason = "Cannot initialize dedupe index";
    freeKernelLayer(layer);
    return result;
  }

  // Compression context storage
  result = ALLOCATE(config->threadCounts.cpuThreads, char *, "LZ4 context",
                    &layer->compressionContext);
  if (result != VDO_SUCCESS) {
    *reason = "cannot allocate LZ4 context";
    freeKernelLayer(layer);
    return result;
  }
  for (int i = 0; i < config->threadCounts.cpuThreads; i++) {
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
  BUG_ON(layer->logicalBlockSize <= 0);
  BUG_ON(layer->requestLimiter.limit <= 0);
  BUG_ON(layer->bioset == NULL);
  BUG_ON(layer->dev == NULL);
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
  if (result < 0) {
    freeKernelLayer(layer);
    return result;
  }

  setKernelLayerState(layer, LAYER_REQUEST_QUEUE_INITIALIZED);

  // Bio queue
  result = makeIOSubmitter(layer->threadNamePrefix,
                           config->threadCounts.bioThreads,
                           config->threadCounts.bioRotationInterval,
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
    result = makeWorkQueue(layer->threadNamePrefix, "ackQ",
                           &layer->wqDirectory, layer, layer, &bioAckQType,
                           config->threadCounts.bioAckThreads,
                           &layer->bioAckQueue);
    if (result != VDO_SUCCESS) {
      *reason = "bio ack queue initialization failed";
      freeKernelLayer(layer);
      return result;
    }
  }

  setKernelLayerState(layer, LAYER_BIO_ACK_QUEUE_INITIALIZED);

  // CPU Queues
  result = makeWorkQueue(layer->threadNamePrefix, "cpuQ", &layer->wqDirectory,
                         layer, NULL, &cpuQType,
                         config->threadCounts.cpuThreads, &layer->cpuQueue);
  if (result != VDO_SUCCESS) {
    *reason = "Albireo CPU queue initialization failed";
    freeKernelLayer(layer);
    return result;
  }

  setKernelLayerState(layer, LAYER_CPU_QUEUE_INITIALIZED);

  *layerPtr = layer;
  return VDO_SUCCESS;
}

/**********************************************************************/
int modifyKernelLayer(KernelLayer       *layer,
                      struct dm_target  *ti,
                      DeviceConfig      *config,
                      char             **why)
{
  if (ti->begin != layer->ti->begin) {
    *why = "Starting sector cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  DeviceConfig *extantConfig = layer->deviceConfig;

  if (strcmp(config->parentDeviceName, extantConfig->parentDeviceName) != 0) {
    *why = "Underlying device cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->logicalBlockSize != extantConfig->logicalBlockSize) {
    *why = "Logical block size cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->cacheSize != extantConfig->cacheSize) {
    *why = "Block map cache size cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->blockMapMaximumAge != extantConfig->blockMapMaximumAge) {
    *why = "Block map maximum age cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->mdRaid5ModeEnabled != extantConfig->mdRaid5ModeEnabled) {
    *why = "mdRaid5Mode cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->readCacheEnabled != extantConfig->readCacheEnabled) {
    *why = "Read cache enabled cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (config->readCacheExtraBlocks != extantConfig->readCacheExtraBlocks) {
    *why = "Read cache size cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  if (strcmp(config->threadConfigString, extantConfig->threadConfigString)
      != 0) {
    *why = "Thread configuration cannot change";
    return VDO_PARAMETER_MISMATCH;
  }

  // Below here are the actions to take when a non-immutable property changes.

  if (config->writePolicy != layer->deviceConfig->writePolicy) {
    /*
     * Ordinarily, when going from async to sync, we must flush any metadata
     * written. However, because the underlying storage must have gone into
     * sync mode before we suspend VDO, and suspending VDO concludes by
     * issuing a flush, all metadata written before the suspend is flushed
     * by the suspend and all metadata between the suspend and the write
     * policy change is written to synchronous storage.
     */
    if (getKernelLayerState(layer) != LAYER_SUSPENDED) {
      *why = "Device must be suspended before changing write policy";
      return VDO_COMPONENT_BUSY;
    }

    logInfo("Modifying device '%s' write policy from %s to %s",
            config->poolName, getConfigWritePolicyString(layer->deviceConfig),
            getConfigWritePolicyString(config));
    setWritePolicy(layer->kvdo.vdo, config->writePolicy);
    return VDO_SUCCESS;
  }

  if (ti->len != layer->ti->len) {
    if (getKernelLayerState(layer) != LAYER_SUSPENDED) {
      *why = "Device must be suspended before changing logical size";
      return VDO_COMPONENT_BUSY;
    }

    size_t logicalBytes = to_bytes(ti->len);
    if ((logicalBytes % VDO_BLOCK_SIZE) != 0) {
      *why = "Logical size must be a multiple of 4096";
      return VDO_PARAMETER_MISMATCH;
    }

    int result = resizeLogical(layer, logicalBytes / VDO_BLOCK_SIZE);
    if (result != VDO_SUCCESS) {
      *why = "Device growLogical failed";
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

  case LAYER_SUSPENDING:
  case LAYER_RESUMING:
    logWarning("shutting down while still supending or resuming");
    // fall through

  case LAYER_RUNNING:
  case LAYER_SUSPENDED:
    stopKernelLayer(layer);
    // fall through

  case LAYER_STOPPED:
  case LAYER_CPU_QUEUE_INITIALIZED:
    finishWorkQueue(layer->cpuQueue);
    usedCpuQueue = true;
    releaseInstance = true;
    // fall through

  case LAYER_BIO_ACK_QUEUE_INITIALIZED:
    if (useBioAckQueue(layer)) {
      finishWorkQueue(layer->bioAckQueue);
      usedBioAckQueue = true;
    }
    // fall through

  case LAYER_BIO_DATA_INITIALIZED:
    cleanupIOSubmitter(layer->ioSubmitter);
    // fall through

  case LAYER_REQUEST_QUEUE_INITIALIZED:
    finishKVDO(&layer->kvdo);
    usedKVDO = true;
    // fall through

  case LAYER_BUFFER_POOLS_INITIALIZED:
    freeBufferPool(&layer->dataKVIOPool);
    freeBufferPool(&layer->traceBufferPool);
    // fall through

  case LAYER_SIMPLE_THINGS_INITIALIZED:
    if (layer->compressionContext != NULL) {
      for (int i = 0; i < layer->deviceConfig->threadCounts.cpuThreads; i++) {
        FREE(layer->compressionContext[i]);
      }
      FREE(layer->compressionContext);
    }
    if (layer->dedupeIndex != NULL) {
      finishDedupeIndex(layer->dedupeIndex);
    }
    FREE(layer->spareKVDOFlush);
    layer->spareKVDOFlush = NULL;
    freeBatchProcessor(&layer->dataKVIOReleaser);
    removeLayerFromDeviceRegistry(layer->deviceConfig->poolName);
    break;

  default:
    logError("Unknown Kernel Layer state: %d", state);
  }

  // Late deallocation of resources in work queues.
  if (usedCpuQueue) {
    freeWorkQueue(&layer->cpuQueue);
  }
  if (usedBioAckQueue) {
    freeWorkQueue(&layer->bioAckQueue);
  }
  if (layer->ioSubmitter) {
    freeIOSubmitter(layer->ioSubmitter);
  }
  if (usedKVDO) {
    destroyKVDO(&layer->kvdo);
  }
  if (layer->bioset != NULL) {
    bioset_free(layer->bioset);
    layer->bioset = NULL;
  }

  freeDedupeIndex(&layer->dedupeIndex);
  freeDeviceConfig(&layer->deviceConfig);

  stopPeriodicEventReporter(&layer->albireoTimeoutReporter);
  if (releaseInstance) {
    releaseKVDOInstance(layer->instance);
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
    .sysfs_ops     = &poolStatsSysfsOps,
    .default_attrs = poolStatsAttrs,
  };
  kobject_init(&layer->statsDirectory, &statsDirectoryKobjType);
  result = kobject_add(&layer->statsDirectory, &layer->kobj, "statistics");
  if (result != 0) {
    *reason = "Cannot add sysfs statistics node";
    stopKernelLayer(layer);
    return result;
  }
  layer->statsAdded = true;

  startDedupeIndex(layer->dedupeIndex);
  result = vdoCreateProcfsEntry(layer, layer->deviceConfig->poolName,
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
  vdoDestroyProcfsEntry(layer->deviceConfig->poolName, layer->procfsPrivate);

  int result = stopKVDO(&layer->kvdo);
  if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
    logError("%s: Close device failed %d (%s: %s)",
             __func__, result,
             stringErrorName(result, errorName, sizeof(errorName)),
             stringError(result, errorMessage, sizeof(errorMessage)));
  }

  switch (getKernelLayerState(layer)) {
  case LAYER_SUSPENDING:
  case LAYER_SUSPENDED:
  case LAYER_RESUMING:
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

  setKernelLayerState(layer, LAYER_SUSPENDING);
  suspendKVDO(&layer->kvdo);

  // Make sure we restart the heartbeat after it is stopped.
  setKernelLayerState(layer, LAYER_SUSPENDED);

  // Attempt to flush all I/O before completing post suspend work.
  waitForNoRequestsActive(layer);
  return synchronousFlush(layer);
}

/**********************************************************************/
int resumeKernelLayer(KernelLayer *layer)
{
  if (getKernelLayerState(layer) == LAYER_SUSPENDED) {
    setKernelLayerState(layer, LAYER_RESUMING);
    resumeKVDO(&layer->kvdo);
    setKernelLayerState(layer, LAYER_RUNNING);
  }
  return VDO_SUCCESS;
}

/***********************************************************************/
int prepareToResizePhysical(KernelLayer *layer, BlockCount physicalCount)
{
  logInfo("Preparing to resize physical to %" PRIu64, physicalCount);
  // Allow allocations for the duration of resize, but no longer.
  layer->allocationsAllowed = true;
  int result = kvdoPrepareToGrowPhysical(&layer->kvdo, physicalCount);
  layer->allocationsAllowed = false;
  if (result != VDO_SUCCESS) {
    // kvdoPrepareToGrowPhysical logs errors.
    return result;
  }

  logInfo("Done preparing to resize physical");
  return VDO_SUCCESS;
}

/***********************************************************************/
int resizePhysical(KernelLayer *layer, BlockCount physicalCount)
{
  if (physicalCount <= layer->blockCount) {
    logWarning("Requested physical block count %" PRIu64
               " not greater than %" PRIu64,
             (uint64_t) physicalCount, (uint64_t) layer->blockCount);
    return -EINVAL;
  } else {
    // Allow allocations for the duration of resize, but no longer.
    layer->allocationsAllowed = true;
    int result = kvdoResizePhysical(&layer->kvdo, physicalCount);
    layer->allocationsAllowed = false;
    if (result != VDO_SUCCESS) {
      // kvdoResizePhysical logs errors
      return result;
    }
    logInfo("Physical block count was %" PRIu64 ", now %" PRIu64,
            (uint64_t) layer->blockCount, (uint64_t) physicalCount);
    layer->blockCount = physicalCount;
  }

  return VDO_SUCCESS;
}

/***********************************************************************/
int prepareToResizeLogical(KernelLayer *layer, BlockCount logicalCount)
{
  logInfo("Preparing to resize logical to %" PRIu64, logicalCount);
  // Allow allocations for the duration of resize, but no longer.
  layer->allocationsAllowed = true;
  int result = kvdoPrepareToGrowLogical(&layer->kvdo, logicalCount);
  layer->allocationsAllowed = false;
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
  logInfo("Resizing logical to %" PRIu64, logicalCount);
  int result = kvdoResizeLogical(&layer->kvdo, logicalCount);
  if (result != VDO_SUCCESS) {
    // kvdoResizeLogical logs errors
    return result;
  }

  logInfo("Logical blocks now %" PRIu64, logicalCount);
  return VDO_SUCCESS;
}

