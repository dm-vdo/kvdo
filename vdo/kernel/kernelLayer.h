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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/kernelLayer.h#8 $
 */

#ifndef KERNELLAYER_H
#define KERNELLAYER_H

#include <linux/device-mapper.h>

#include "atomic.h"
#include "constants.h"
#include "flush.h"
#include "intMap.h"
#include "physicalLayer.h"
#include "volumeGeometry.h"
#include "waitQueue.h"

#include "batchProcessor.h"
#include "bufferPool.h"
#include "deadlockQueue.h"
#include "deviceConfig.h"
#include "histogram.h"
#include "kernelStatistics.h"
#include "kernelTypes.h"
#include "kernelVDO.h"
#include "ktrace.h"
#include "limiter.h"
#include "statistics.h"
#include "workQueue.h"

enum {
  VDO_SECTORS_PER_BLOCK = (VDO_BLOCK_SIZE >> SECTOR_SHIFT)
};

typedef enum {
  LAYER_SIMPLE_THINGS_INITIALIZED,
  LAYER_BUFFER_POOLS_INITIALIZED,
  LAYER_REQUEST_QUEUE_INITIALIZED,
  LAYER_CPU_QUEUE_INITIALIZED,
  LAYER_BIO_ACK_QUEUE_INITIALIZED,
  LAYER_BIO_DATA_INITIALIZED,
  LAYER_RUNNING,
  LAYER_SUSPENDED,
  LAYER_STOPPING,
  LAYER_STOPPED,
} KernelLayerState;

/* Keep BIO statistics atomically */
struct atomicBioStats {
  atomic64_t read;              // Number of not REQ_WRITE bios
  atomic64_t write;             // Number of REQ_WRITE bios
  atomic64_t discard;           // Number of REQ_DISCARD bios
  atomic64_t flush;             // Number of REQ_FLUSH bios
  atomic64_t fua;               // Number of REQ_FUA bios
};

// Data managing the reporting of Albireo timeouts
typedef struct periodicEventReporter {
  uint64_t             lastReportedValue;
  const char          *format;
  atomic64_t           value;
  Jiffies              reportingInterval; // jiffies
  /*
   * Just an approximation.  If nonzero, then either the work item has
   * been queued to run, or some other thread currently has
   * responsibility for enqueueing it, or the reporter function is
   * running but hasn't looked at the current value yet.
   *
   * If this is set, don't set the timer again, because we don't want
   * the work item queued twice.  Use an atomic xchg or cmpxchg to
   * test-and-set it, and an atomic store to clear it.
   */
  atomic_t             workItemQueued;
  KvdoWorkItem         workItem;
  KernelLayer         *layer;
} PeriodicEventReporter;

static inline uint64_t getEventCount(PeriodicEventReporter *reporter)
{
  return atomic64_read(&reporter->value);
}

/**
 * The VDO representation of the target device
 **/
struct kernelLayer {
  PhysicalLayer           common;
  // Layer specific info
  DeviceConfig           *deviceConfig;
  char                    threadNamePrefix[MAX_QUEUE_NAME_LEN];
  struct kobject          kobj;
  struct kobject          wqDirectory;
  struct kobject          statsDirectory;
  /**
   * A counter value to attach to thread names and log messages to
   * identify the individual device.
   **/
  unsigned int            instance;
  /** Contains the current KernelLayerState, which rarely changes */
  Atomic32                state;
  bool                    allocationsAllowed;
  /** The current dm target for this layer */
  struct dm_target       *ti;
  /** The previous dm target and device for this layer */
  struct dm_target       *oldTI;
  struct dm_dev          *oldDev;
  AtomicBool              processingMessage;

  /** Limit the number of requests that are being processed. */
  Limiter                 requestLimiter;
  Limiter                 discardLimiter;
  KVDO                    kvdo;
  /** Incoming bios we've had to buffer to avoid deadlock. */
  DeadlockQueue           deadlockQueue;
  // for REQ_FLUSH processing
  struct bio_list         waitingFlushes;
  KVDOFlush              *spareKVDOFlush;
  spinlock_t              flushLock;
  Jiffies                 flushArrivalTime;
  /**
   * Completion used for synchronizing the flush operation issued
   * during suspend.
   **/
  struct completion       flushWait;
  bool                    mdRaid5ModeEnabled;
  /**
   * Bio submission manager used for sending bios to the storage
   * device.
   **/
  IOSubmitter            *ioSubmitter;
  /**
   * Work queue (possibly with multiple threads) for miscellaneous
   * CPU-intensive, non-blocking work.
   **/
  KvdoWorkQueue          *cpuQueue;
  /** N blobs of context data for LZ4 code, one per CPU thread. */
  char                  **compressionContext;
  Atomic32                compressionContextIndex;
  /** Optional work queue for calling bio_endio. */
  KvdoWorkQueue          *bioAckQueue;
  /** Underlying block device info. */
  struct dm_dev          *dev;
  BlockCount              blockCount;
  BlockSize               logicalBlockSize;
  uint64_t                startingSectorOffset;
  VolumeGeometry          geometry;
  // Read cache
  unsigned int            readCacheBlocks;
  // Memory allocation
  BufferPool             *dataKVIOPool;
  struct bio_set         *bioset;
  // Albireo specific info
  DedupeIndex            *dedupeIndex;
  // Statistics
  atomic64_t              biosSubmitted;
  atomic64_t              biosCompleted;
  atomic64_t              dedupeContextBusy;
  atomic64_t              flushOut;
  AtomicBioStats          biosIn;
  AtomicBioStats          biosInPartial;
  AtomicBioStats          biosOut;
  AtomicBioStats          biosOutCompleted;
  AtomicBioStats          biosAcknowledged;
  AtomicBioStats          biosAcknowledgedPartial;
  AtomicBioStats          biosMeta;
  AtomicBioStats          biosMetaCompleted;
  AtomicBioStats          biosJournal;
  AtomicBioStats          biosPageCache;
  AtomicBioStats          biosJournalCompleted;
  AtomicBioStats          biosPageCacheCompleted;
  // for reporting Albireo timeouts
  PeriodicEventReporter   albireoTimeoutReporter;
  // Debugging
  /* Whether to dump VDO state on shutdown */
  bool                    dumpOnShutdown;
  /**
   * Whether we should collect tracing info. (Actually, this controls
   * allocations; non-null record pointers cause recording.)
   **/
  bool                    vioTraceRecording;
  SampleCounter           traceSampleCounter;
  /* Should we log tracing info? */
  bool                    traceLogging;
  /* Storage for trace data. */
  BufferPool             *traceBufferPool;
  /* Private storage for procfs. */
  void                   *procfsPrivate;
  /* For returning batches of DataKVIOs to their pool */
  BatchProcessor         *dataKVIOReleaser;

  // Administrative operations
  /* The object used to wait for administrative operations to complete */
  struct completion       callbackSync;

  // Statistics reporting
  /* Protects the *statsStorage structs */
  struct mutex            statsMutex;
  /* Used when shutting down the sysfs statistics */
  struct completion       statsShutdown;;
  /* true if sysfs statistics directory is set up */
  bool                    statsAdded;
  /* Used to gather statistics without allocating memory */
  VDOStatistics           vdoStatsStorage;
  KernelStatistics        kernelStatsStorage;
};

typedef enum bioQAction {
  BIO_Q_ACTION_COMPRESSED_DATA,
  BIO_Q_ACTION_DATA,
  BIO_Q_ACTION_FLUSH,
  BIO_Q_ACTION_HIGH,
  BIO_Q_ACTION_METADATA,
  BIO_Q_ACTION_READCACHE,
  BIO_Q_ACTION_VERIFY
} BioQAction;

typedef enum cpuQAction {
  CPU_Q_ACTION_COMPARE_BLOCKS,
  CPU_Q_ACTION_COMPLETE_KVIO,
  CPU_Q_ACTION_COMPRESS_BLOCK,
  CPU_Q_ACTION_DEDUPE_SHUTDOWN,
  CPU_Q_ACTION_EVENT_REPORTER,
  CPU_Q_ACTION_HASH_BLOCK,
  CPU_Q_ACTION_SET_UP_VERIFY,
} CPUQAction;

typedef enum bioAckQAction {
  BIO_ACK_Q_ACTION_ACK,
} BioAckQAction;

typedef void (*DedupeShutdownCallbackFunction)(KernelLayer *layer);

/*
 * Wrapper for the Enqueueable object, to associate it with a kernel
 * layer work item.
 */
typedef struct kvdoEnqueueable {
  KvdoWorkItem workItem;
  Enqueueable  enqueueable;
} KvdoEnqueueable;

/**
 * Creates a kernel specific physical layer to be used by VDO
 *
 * @param blockCount            The number of blocks supported by the layer
 * @param startingSector        The sector offset of our table entry in the
 *                              DM device
 * @param dev                   The underlying device
 * @param instance              Device instantiation counter
 * @param parentKobject         The parent sysfs node
 * @param config                The device configuration
 * @param threadConfigPointer   Where to store the new threadConfig handle
 * @param reason                The reason for any failure during this call
 * @param layerPtr              A pointer to hold the created layer
 *
 * @return VDO_SUCCESS or an error
 **/
int makeKernelLayer(BlockCount      blockCount,
                    uint64_t        startingSector,
                    struct dm_dev  *dev,
                    unsigned int    instance,
                    DeviceConfig   *config,
                    struct kobject *parentKobject,
                    ThreadConfig  **threadConfigPointer,
                    char          **reason,
                    KernelLayer   **layerPtr)
  __attribute__((warn_unused_result));

/**
 * Modify a kernel physical layer.
 *
 * @param layer     The layer to modify
 * @param ti        The new dm_target structure
 * @param config    The device configuration
 * @param why       The reason for any failure during this call
 *
 * @return VDO_SUCCESS or an error
 **/
int modifyKernelLayer(KernelLayer       *layer,
                      struct dm_target  *ti,
                      DeviceConfig      *config,
                      char             **why)
  __attribute__((warn_unused_result));

/**
 * Free a kernel physical layer.
 *
 * @param layer    The layer, which must have been created by
 *                 makeKernelLayer
 **/
void freeKernelLayer(KernelLayer *layer);

/**
 * Start the kernel layer.
 *
 * @param layer       The kernel layer
 * @param loadConfig  Load-time parameters for the VDO
 * @param reason      The reason for any failure during this call
 *
 * @return VDO_SUCCESS or an error
 *
 * @note redundant starts are silently ignored
 **/
int startKernelLayer(KernelLayer          *layer,
                     const VDOLoadConfig  *loadConfig,
                     char                **reason);

/**
 * Stop the kernel layer.
 *
 * @param layer  The kernel layer
 *
 * @return VDO_SUCCESS or an error
 **/
int stopKernelLayer(KernelLayer *layer);

/**
 * Suspend the kernel layer.
 *
 * @param layer  The kernel layer
 *
 * @return VDO_SUCCESS or an error
 **/
int suspendKernelLayer(KernelLayer *layer);

/**
 * Resume the kernel layer.
 *
 * @param layer  The kernel layer
 *
 * @return VDO_SUCCESS or an error
 **/
int resumeKernelLayer(KernelLayer *layer);

/**
 * Get the kernel layer state.
 *
 * @param layer  The kernel layer
 *
 * @return the instantaneously correct kernel layer state
 **/
static inline KernelLayerState getKernelLayerState(const KernelLayer *layer)
{
  return relaxedLoad32(&layer->state);
}

/**
 * Function call to begin processing a bio passed in from the block layer
 *
 * @param layer  The physical layer
 * @param bio    The bio from the block layer
 *
 * @return value to return from the VDO map function.  Either an error code
 *         or DM_MAPIO_REMAPPED or DM_MAPPED_SUBMITTED (see vdoMapBio for
 *         details).
 **/
int kvdoMapBio(KernelLayer *layer, BIO *bio);

/**
 * Convert a generic PhysicalLayer to a kernelLayer.
 *
 * @param layer The PhysicalLayer to convert
 *
 * @return The PhysicalLayer as a KernelLayer
 **/
static inline KernelLayer *asKernelLayer(PhysicalLayer *layer)
{
  return container_of(layer, KernelLayer, common);
}

/**
 * Convert a block number (or count) to a (512-byte-)sector number.
 *
 * The argument type is sector_t to force conversion to the type we
 * want, although the actual values passed are of various integral
 * types.  It's just too easy to forget and do the multiplication
 * without casting, resulting in 32-bit arithmetic that accidentally
 * produces wrong results in devices over 2TB (2**32 sectors).
 *
 * @param [in] layer        the physical layer
 * @param [in] blockNumber  the block number/count
 *
 * @return      the sector number/count
 **/
static inline sector_t blockToSector(KernelLayer *layer, sector_t blockNumber)
{
  return (blockNumber * VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number (or count) to a block number. Does not
 * check to make sure the sector number is an integral number of
 * blocks.
 *
 * @param [in] layer         the physical layer
 * @param [in] sectorNumber  the sector number/count
 *
 * @return      the block number/count
 **/
static inline sector_t sectorToBlock(KernelLayer *layer, sector_t sectorNumber)
{
  return (sectorNumber / VDO_SECTORS_PER_BLOCK);
}

/**
 * Convert a sector number to an offset within a block.
 *
 * @param [in] layer         the physical layer
 * @param [in] sectorNumber  the sector number
 *
 * @return      the offset within the block
 **/
static inline BlockSize sectorToBlockOffset(KernelLayer *layer,
                                            sector_t     sectorNumber)
{
  unsigned int sectorsPerBlockMask = VDO_SECTORS_PER_BLOCK - 1;
  return to_bytes(sectorNumber & sectorsPerBlockMask);
}

/**
 * Given an error code, return a value we can return to the OS.  The
 * input error code may be a system-generated value (such as -EIO), an
 * errno macro used in our code (such as EIO), or a UDS or VDO status
 * code; the result must be something the rest of the OS can consume
 * (negative errno values such as -EIO, in the case of the kernel).
 *
 * @param error    the error code to convert
 *
 * @return   a system error code value
 **/
int mapToSystemError(int error);

/**
 * Record and eventually report that some number of dedupe requests
 * reached their expiration time without getting an answer, so we
 * timed out on them.
 *
 * This is called in a timer context, so it shouldn't do the reporting
 * directly.
 *
 * @param layer          The kernel layer for the device
 * @param expiredCount   The number of expired requests we timed out on
 **/
void kvdoReportDedupeTimeout(KernelLayer *layer, unsigned int expiredCount);

/**
 * Wait until there are no requests in progress.
 *
 * @param layer  The kernel layer for the device
 **/
void waitForNoRequestsActive(KernelLayer *layer);

/**
 * Enqueues an item on our internal "cpu queues". Since there is more than
 * one, we rotate through them in hopes of creating some general balance.
 *
 * @param layer The kernel layer
 * @param item  The work item to enqueue
 */
static inline void enqueueCPUWorkQueue(KernelLayer *layer, KvdoWorkItem *item)
{
  enqueueWorkQueue(layer->cpuQueue, item);
}

/**
 * Adjust parameters to prepare to use a larger physical space.
 * The size must be larger than the current size.
 *
 * @param layer          the kernel layer
 * @param physicalCount  the new physical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int prepareToResizePhysical(KernelLayer *layer, BlockCount physicalCount);

/**
 * Adjusts parameters to reflect resizing the underlying device.
 * The size must be larger than the current size.
 *
 * @param layer            the kernel layer
 * @param physicalCount    the new physical count in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int resizePhysical(KernelLayer *layer, BlockCount physicalCount);

/**
 * Adjust parameters to prepare to present a larger logical space.
 * The size must be larger than the current size.
 *
 * @param layer         the kernel layer
 * @param logicalCount  the new logical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int prepareToResizeLogical(KernelLayer *layer, BlockCount logicalCount);

/**
 * Adjust parameters to present a larger logical space.
 * The size must be larger than the current size.
 *
 * @param layer         the kernel layer
 * @param logicalCount  the new logical size in blocks
 *
 * @return VDO_SUCCESS or an error
 */
int resizeLogical(KernelLayer *layer, BlockCount logicalCount);

/**
 * Indicate whether the kernel layer is configured to use a separate
 * work queue for acknowledging received and processed bios.
 *
 * Note that this directly controls handling of write operations, but
 * the compile-time flag USE_BIO_ACK_QUEUE_FOR_READ is also checked
 * for read operations.
 *
 * @param  layer  The kernel layer
 *
 * @return   Whether a bio-acknowledgement work queue is in use
 **/
static inline bool useBioAckQueue(KernelLayer *layer)
{
  return layer->deviceConfig->threadCounts.bioAckThreads > 0;
}

/**
 * Update bookkeeping for the completion of some number of requests, so that
 * more incoming requests can be accepted.
 *
 * @param layer  The kernel layer
 * @param count  The number of completed requests
 **/
void completeManyRequests(KernelLayer *layer, uint32_t count);

#endif /* KERNELLAYER_H */
