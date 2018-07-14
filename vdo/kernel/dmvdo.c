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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/dmvdo.c#16 $
 */

#include "dmvdo.h"

#include <linux/module.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "constants.h"
#include "threadConfig.h"
#include "vdo.h"

#include "dedupeIndex.h"
#include "deviceRegistry.h"
#include "dump.h"
#include "instanceNumber.h"
#include "kernelLayer.h"
#include "kvdoFlush.h"
#include "memoryUsage.h"
#include "readCache.h"
#include "statusProcfs.h"
#include "stringUtils.h"
#include "sysfs.h"
#include "threadDevice.h"
#include "threadRegistry.h"

struct kvdoDevice kvdoDevice;   // global driver state (poorly named)

/*
 * Set the default maximum number of sectors for a single discard bio.
 *
 * The value 1024 is the largest usable value on HD systems.  A 2048 sector
 * discard on a busy HD system takes 31 seconds.  We should use a value no
 * higher than 1024, which takes 15 to 16 seconds on a busy HD system.
 *
 * But using large values results in 120 second blocked task warnings in
 * /var/log/kern.log.  In order to avoid these warnings, we choose to use the
 * smallest reasonable value.  See VDO-3062 and VDO-3087.
 *
 * Pre kernel version 4.3, we use the functionality in blkdev_issue_discard
 * and the value in max_discard_sectors to split large discards into smaller
 * ones. 4.3 and beyond kernels have removed the code in blkdev_issue_discard
 * and so after that point, we use the code in device mapper itself to
 * split the discards. Unfortunately, it uses the same value to split large
 * discards as it does to split large data bios. As such, we should never
 * change the value of max_discard_sectors in kernel versions beyond that.
 * We continue to set the value for max_discard_sectors as it is used in other
 * code, like sysfs display of queue limits and since we are actually splitting
 * discards it makes sense to show the correct value there.
 *
 * DO NOT CHANGE VALUE for kernel versions 4.3 and beyond to anything other
 * than what is set to the max_io_len field below. We mimic to_sector here
 * as a non const, otherwise compile would fail.
 */
unsigned int maxDiscardSectors = VDO_SECTORS_PER_BLOCK;

/*
 * We want to support discard requests, but early device mapper versions
 * did not give us any help.  Define HAS_DISCARDS_SUPPORTED if the
 * dm_target contains the discards_supported member.  Note that this is not
 * a trivial determination.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
#define HAS_DISCARDS_SUPPORTED 1
#else
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,32) && defined(RHEL_RELEASE_CODE)
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(6,2)
#define HAS_DISCARDS_SUPPORTED 1
#else
#define HAS_DISCARDS_SUPPORTED 0
#endif /* RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(6,2) */
#else
#define HAS_DISCARDS_SUPPORTED 0
#endif
#endif

/*
 * We want to support flush requests in async mode, but early device mapper
 * versions got in the way if the underlying device did not also support
 * flush requests.  Define HAS_FLUSH_SUPPORTED if the dm_target contains
 * the flush_supported member.  We support flush in either case, but the
 * flush_supported member makes it trivial (and safer).
 */
#ifdef RHEL_RELEASE_CODE
#define HAS_FLUSH_SUPPORTED (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(6,6))
#else
#define HAS_FLUSH_SUPPORTED (LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0))
#endif

/**********************************************************************/

/**
 * Begin VDO processing of a bio.  This is called by the device mapper
 * through the "map" function, and has resulted from a call to either
 * submit_bio or generic_make_request.
 *
 * @param ti      The dm_target.  We only need the "private" member to give
 *                us the KernelLayer.
 * @param bio     The bio.
 * @param unused  An additional parameter that we do not use.  This
 *                argument went away in Linux 3.8.
 *
 * @return One of these values:
 *
 *         negative            A negative value is an error code.
 *                             Usually -EIO.
 *
 *         DM_MAPIO_SUBMITTED  VDO will take care of this I/O, either
 *                             processing it completely and calling
 *                             bio_endio, or forwarding it onward by
 *                             calling generic_make_request.
 *
 *         DM_MAPIO_REMAPPED   VDO has modified the bio and the device
 *                             mapper will immediately forward the bio
 *                             onward using generic_make_request.
 *
 *         DM_MAPIO_REQUEUE    We do not use this.  It is used by device
 *                             mapper devices to defer an I/O request
 *                             during suspend/resume processing.
 **/
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,8,0)
static int vdoMapBio(struct dm_target *ti, BIO *bio, union map_info *unused)
#else
static int vdoMapBio(struct dm_target *ti, BIO *bio)
#endif
{
  KernelLayer *layer = ti->private;
  return kvdoMapBio(layer, bio);
}

/**********************************************************************/
static void vdoIoHints(struct dm_target *ti, struct queue_limits *limits)
{
  KernelLayer *layer = ti->private;

  limits->logical_block_size  = layer->logicalBlockSize;
  limits->physical_block_size = VDO_BLOCK_SIZE;

  // The minimum io size for random io
  blk_limits_io_min(limits, VDO_BLOCK_SIZE);
  // The optimal io size for streamed/sequential io
  blk_limits_io_opt(limits, VDO_BLOCK_SIZE);

  // Discard hints
  limits->max_discard_sectors = maxDiscardSectors;
  limits->discard_granularity = VDO_BLOCK_SIZE;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
  limits->discard_zeroes_data = 1;
#endif
}

/**********************************************************************/
static int vdoIterateDevices(struct dm_target           *ti,
                             iterate_devices_callout_fn  fn,
                             void                       *data)
{
  KernelLayer *layer = ti->private;
  sector_t len = blockToSector(layer, layer->blockCount);

#if HAS_FLUSH_SUPPORTED
  return fn(ti, layer->dev, 0, len, data);
#else
  if (!shouldProcessFlush(layer)) {
    // In sync mode, if the underlying device needs flushes, accept flushes.
    return fn(ti, layer->dev, 0, len, data);
  }

  /*
   * We need to get flush requests in async mode, but the device mapper
   * will only let us get them if the underlying device gets them.  We
   * must tell a little white lie.
   */
  struct request_queue *q = bdev_get_queue(layer->dev->bdev);
  unsigned int flush_flags = q->flush_flags;
  q->flush_flags = REQ_FLUSH | REQ_FUA;

  int result = fn(ti, layer->dev, 0, len, data);

  q->flush_flags = flush_flags;
  return result;
#endif /* HAS_FLUSH_SUPPORTED */
}

/*
 * Device-mapper status method return types:
 *
 * Debian Squeeze backport of 3.2.0-0.bpo.3-amd64: int
 * Ubuntu Precise 3.2.0-49-generic: void
 * Linus git repo 3.2.0: int
 * 3.8+: void
 *
 * Distinguishing versions:
 *
 * Our current version of Ubuntu "3.2.0" adds config option
 * VERSION_SIGNATURE to the Debian kernel, and has LINUX_VERSION_CODE
 * indicating 3.2.46; it's entirely possible the latter may be updated
 * over time.
 */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0))       \
    && (LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0))     \
    && defined(CONFIG_VERSION_SIGNATURE)
// Probably Ubuntu.
#  define DMSTATUS_RETURNS_VOID 1
#else
#  define DMSTATUS_RETURNS_VOID (LINUX_VERSION_CODE > KERNEL_VERSION(3,8,0))
#endif

#if DMSTATUS_RETURNS_VOID
typedef void DMStatusReturnType;
#else
typedef int DMStatusReturnType;
#endif

#define STATUS_TAKES_FLAGS_ARG (LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0))

/*
 * Status line is:
 *    <device> <operating mode> <in recovery> <index state> 
 *    <compression state> <used physical blocks> <total physical blocks>
 */

/**********************************************************************/
static DMStatusReturnType vdoStatus(struct dm_target *ti,
                                    status_type_t     status_type,
#if STATUS_TAKES_FLAGS_ARG
                                    unsigned int      status_flags,
#endif
                                    char             *result,
                                    unsigned int      maxlen)
{
  KernelLayer *layer = ti->private;
  char nameBuffer[BDEVNAME_SIZE];
  // N.B.: The DMEMIT macro uses the variables named "sz", "result", "maxlen".
  int sz = 0;

  switch (status_type) {
  case STATUSTYPE_INFO:
    // Report info for dmsetup status
    mutex_lock(&layer->statsMutex);
    getKVDOStatistics(&layer->kvdo, &layer->vdoStatsStorage);
    VDOStatistics *stats = &layer->vdoStatsStorage;
    DMEMIT("/dev/%s %s %s %s %s %" PRIu64 " %" PRIu64,
           bdevname(layer->dev->bdev, nameBuffer),
	   stats->mode,
	   stats->inRecoveryMode ? "recovering" : "-",
	   getDedupeStateName(layer->dedupeIndex),
	   getKVDOCompressing(&layer->kvdo) ? "online" : "offline",
	   stats->dataBlocksUsed + stats->overheadBlocksUsed,
	   stats->physicalBlocks);
    mutex_unlock(&layer->statsMutex);
    break;

  case STATUSTYPE_TABLE:
    // Report the string actually specified in the beginning.
    DMEMIT("%s", layer->deviceConfig->originalString);
    break;
  }

//  spin_unlock_irqrestore(&layer->lock, flags);

#if !DMSTATUS_RETURNS_VOID
  return 0;
#endif
}


/**
 * Get the size of a device, in blocks.
 *
 * @param [in]  dev     The device object.
 *
 * @return The size in blocks
 **/
static BlockCount getDeviceBlockCount(struct dm_dev *dev)
{
  uint64_t physicalSize = i_size_read(dev->bdev->bd_inode);
  return physicalSize / VDO_BLOCK_SIZE;
}

/**********************************************************************/
static int vdoPrepareToGrowLogical(KernelLayer *layer, char *sizeString)
{
  BlockCount logicalCount;
  if (sscanf(sizeString, "%llu", &logicalCount) != 1) {
    logWarning("Logical block count \"%s\" is not a number", sizeString);
    return -EINVAL;
  }

  if (logicalCount > MAXIMUM_LOGICAL_BLOCKS) {
    logWarning("Logical block count \"%" PRIu64 "\" exceeds the maximum (%"
               PRIu64 ")", logicalCount, MAXIMUM_LOGICAL_BLOCKS);
    return -EINVAL;
  }

  return prepareToResizeLogical(layer, logicalCount);
}

/**
 * Process a dmsetup message now that we know no other message is being
 * processed.
 *
 * @param layer The layer to which the message was sent
 * @param argc  The argument count of the message
 * @param argv  The arguments to the message
 *
 * @return -EINVAL if the message is unrecognized or the result of processing
 *                 the message
 **/
__attribute__((warn_unused_result))
static int processVDOMessageLocked(KernelLayer   *layer,
                                   unsigned int   argc,
                                   char         **argv)
{
  // Messages with variable numbers of arguments.
  if (strncasecmp(argv[0], "x-", 2) == 0) {
    int result = performKVDOExtendedCommand(&layer->kvdo, argc, argv);
    if (result == VDO_UNKNOWN_COMMAND) {
      logWarning("unknown extended command '%s' to dmsetup message", argv[0]);
      result = -EINVAL;
    }

    return result;
  }

  // Messages with fixed numbers of arguments.
  switch (argc) {
  case 1:
    if (strcasecmp(argv[0], "sync-dedupe") == 0) {
      waitForNoRequestsActive(layer);
      return 0;
    }

    if (strcasecmp(argv[0], "trace-on") == 0) {
      logInfo("Tracing on");
      layer->traceLogging = true;
      return 0;
    }

    if (strcasecmp(argv[0], "trace-off") == 0) {
      logInfo("Tracing off");
      layer->traceLogging = false;
      return 0;
    }

    if (strcasecmp(argv[0], "prepareToGrowPhysical") == 0) {
      return prepareToResizePhysical(layer, getDeviceBlockCount(layer->dev));
    }

    if (strcasecmp(argv[0], "growPhysical") == 0) {
      return resizePhysical(layer, getDeviceBlockCount(layer->dev));
    }

    break;

  case 2:
    if (strcasecmp(argv[0], "compression") == 0) {
      if (strcasecmp(argv[1], "on") == 0) {
        setKVDOCompressing(&layer->kvdo, true);
        return 0;
      }

      if (strcasecmp(argv[1], "off") == 0) {
        setKVDOCompressing(&layer->kvdo, false);
        return 0;
      }

      logWarning("invalid argument '%s' to dmsetup compression message",
                 argv[1]);
      return -EINVAL;
    }

    if (strcasecmp(argv[0], "prepareToGrowLogical") == 0) {
      return vdoPrepareToGrowLogical(layer, argv[1]);
    }

    break;


  default:
    break;
  }

  logWarning("unrecognized dmsetup message '%s' received", argv[0]);
  return -EINVAL;
}

/**
 * Process a dmsetup message. If the message is a dump, just do it. Otherwise,
 * check that no other message is being processed, and only proceed if so.
 *
 * @param layer The layer to which the message was sent
 * @param argc  The argument count of the message
 * @param argv  The arguments to the message
 *
 * @return -EBUSY if another message is being processed or the result of
 *                processsing the message
 **/
__attribute__((warn_unused_result))
static int processVDOMessage(KernelLayer   *layer,
                             unsigned int   argc,
                             char         **argv)
{
  /*
   * All messages which may be processed in parallel with other messages should
   * be handled here before the atomic check below. Messages which should be
   * exclusive should be processed in processVDOMessageLocked().
   */

  // Dump messages should always be processed
  if (strcasecmp(argv[0], "dump") == 0) {
    return vdoDump(layer, argc, argv, "dmsetup message");
  }

  if (argc == 1) {
    if (strcasecmp(argv[0], "dump-on-shutdown") == 0) {
      layer->dumpOnShutdown = true;
      return 0;
    }

    // Index messages should always be processed
    if ((strcasecmp(argv[0], "index-create") == 0)
        || (strcasecmp(argv[0], "index-disable") == 0)
        || (strcasecmp(argv[0], "index-enable") == 0)) {
      return messageDedupeIndex(layer->dedupeIndex, argv[0]);
    }

    // XXX - the "connect" messages are misnamed for the kernel index.  These
    //       messages should go away when all callers have been fixed to use
    //       "index-enable" or "index-disable".
    if (strcasecmp(argv[0], "reconnect") == 0) {
      return messageDedupeIndex(layer->dedupeIndex, "index-enable");
    }

    if (strcasecmp(argv[0], "connect") == 0) {
      return messageDedupeIndex(layer->dedupeIndex, "index-enable");
    }

    if (strcasecmp(argv[0], "disconnect") == 0) {
      return messageDedupeIndex(layer->dedupeIndex, "index-disable");
    }
  }

  if (!compareAndSwapBool(&layer->processingMessage, false, true)) {
    return -EBUSY;
  }

  int result = processVDOMessageLocked(layer, argc, argv);
  atomicStoreBool(&layer->processingMessage, false);
  return result;
}

/**********************************************************************/
static int vdoMessage(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc == 0) {
    logWarning("unspecified dmsetup message");
    return -EINVAL;
  }

  KernelLayer *layer = (KernelLayer *) ti->private;
  RegisteredThread allocatingThread, instanceThread;
  registerAllocatingThread(&allocatingThread, NULL);
  registerThreadDevice(&instanceThread, layer);
  int result = processVDOMessage(layer, argc, argv);
  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return mapToSystemError(result);
}

/**
 * Get the device beneath this device mapper target, given its name and
 * this target.
 *
 * @param [in]  ti      This device mapper target
 * @param [in]  name    The name of the device beneath ti
 * @param [out] devPtr  A pointer to return the device structure
 *
 * @return a system error code
 **/
static int getUnderlyingDevice(struct dm_target  *ti,
                               const char        *name,
                               struct dm_dev    **devPtr)
{
  int result;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,34)
  result = dm_get_device(ti, name, dm_table_get_mode(ti->table), devPtr);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,32)
// The signature of dm_get_device differs between kernel 2.6.32 on
// debian squeeze and kernel 2.6.32 on RHEL (CentOS)
#if defined(RHEL_RELEASE_CODE)
  result = dm_get_device(ti, name, dm_table_get_mode(ti->table), devPtr);
#else
  result = dm_get_device(ti, name, 0, 0, dm_table_get_mode(ti->table), devPtr);
#endif /* RHEL */
#else
#error "unsupported linux kernel version"
#endif /* kernel 2.6.32 */
#endif /* kernel version > 2.6.34 */

  if (result != 0) {
    logError("couldn't open device \"%s\": error %d", name, result);
    return -EINVAL;
  }
  return result;
}

/**
 * Configure the dm_target with our capabilities.
 *
 * @param ti    The device mapper target representing our device
 * @param layer The kernel layer to get the write policy from
 **/
static void configureTargetCapabilities(struct dm_target *ti,
                                        KernelLayer      *layer)
{
#if HAS_DISCARDS_SUPPORTED
  ti->discards_supported = 1;
#endif

#if HAS_FLUSH_SUPPORTED
  /**
   * This may appear to indicate we don't support flushes in sync mode.
   * However, dm will set up the request queue to accept flushes if any
   * device in the stack accepts flushes. Hence if the device under VDO
   * accepts flushes, we will receive flushes.
   **/
  ti->flush_supported = shouldProcessFlush(layer);
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
  ti->num_discard_requests = 1;
  ti->num_flush_requests = 1;
#else
  ti->num_discard_bios = 1;
  ti->num_flush_bios = 1;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
  // If this value changes, please make sure to update the
  // value for maxDiscardSectors accordingly.
  ti->max_io_len = VDO_SECTORS_PER_BLOCK;
#else
  ti->split_io = VDO_SECTORS_PER_BLOCK;
#endif

/*
 * As of linux kernel version 4.3, support for splitting discards
 * was removed from blkdev_issue_discard. Luckily, device mapper
 * added its own support for splitting discards in kernel version
 * 3.6 and beyond. We will begin using this support from 4.3 on.
 * Please keep in mind the device mapper support uses the same value
 * for splitting discards as it does for splitting regular data i/os.
 * See the max_io_len setting above.
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
  ti->split_discard_bios = 1;
#endif
}

/**
 * Handle a vdoInitialize failure, freeing all appropriate structures.
 *
 * @param ti            The device mapper target representing our device
 * @param config        The parsed config for the instance
 * @param dev           The device under our device (possibly NULL)
 * @param threadConfig  The thread config (possibly NULL)
 * @param layer         The kernel layer (possibly NULL)
 * @param instance      The instance number to be released
 * @param why           The reason for failure
 **/
static void cleanupInitialize(struct dm_target *ti,
                              DeviceConfig     *config,
                              struct dm_dev    *dev,
                              ThreadConfig     *threadConfig,
                              KernelLayer      *layer,
                              unsigned int      instance,
                              char             *why)
{
  freeDeviceConfig(&config);
  if (threadConfig != NULL) {
    freeThreadConfig(&threadConfig);
  }
  if (layer != NULL) {
    // This releases the instance number too.
    freeKernelLayer(layer);
  } else {
    // With no KernelLayer taking ownership we have to release explicitly.
    releaseKVDOInstance(instance);
  }
  if (dev != NULL) {
    dm_put_device(ti, dev);
  }

  ti->error = why;
}

/**
 * Initializes a single VDO instance and loads the data from disk
 *
 * @param ti        The device mapper target representing our device
 * @param instance  The device instantiation counter
 * @param config    The parsed config for the instance
 *
 * @return  VDO_SUCCESS or an error code
 *
 **/
static int vdoInitialize(struct dm_target *ti,
                         unsigned int      instance,
                         DeviceConfig     *config)
{
  logInfo("starting device '%s'", config->poolName);

  struct dm_dev *dev;
  int result = getUnderlyingDevice(ti, config->parentDeviceName, &dev);
  if (result != 0) {
    cleanupInitialize(ti, config, NULL, NULL, NULL, instance,
                      "Device lookup failed");
    return result;
  }

  struct request_queue *requestQueue = bdev_get_queue(dev->bdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
  bool flushSupported
    = ((requestQueue->queue_flags & (1ULL << QUEUE_FLAG_WC)) != 0);
  bool fuaSupported
    = ((requestQueue->queue_flags & (1ULL << QUEUE_FLAG_FUA)) != 0);
#else
  bool flushSupported = ((requestQueue->flush_flags & REQ_FLUSH) == REQ_FLUSH);
  bool fuaSupported   = ((requestQueue->flush_flags & REQ_FUA) == REQ_FUA);
#endif
  logInfo("underlying device, REQ_FLUSH: %s, REQ_FUA: %s",
          (flushSupported ? "supported" : "not supported"),
          (fuaSupported ? "supported" : "not supported"));

  resolveConfigWithFlushSupport(config, flushSupported);

  uint64_t   blockSize      = VDO_BLOCK_SIZE;
  uint64_t   logicalSize    = to_bytes(ti->len);
  BlockCount logicalBlocks  = logicalSize / blockSize;
  BlockCount physicalBlocks = getDeviceBlockCount(dev);

  logDebug("Logical block size      = %" PRIu64,
           (uint64_t) config->logicalBlockSize);
  logDebug("Logical blocks          = %" PRIu64, logicalBlocks);
  logDebug("Physical block size     = %" PRIu64, (uint64_t) blockSize);
  logDebug("Physical blocks         = %" PRIu64, physicalBlocks);
  logDebug("Block map cache blocks  = %u", config->cacheSize);
  logDebug("Block map maximum age   = %u", config->blockMapMaximumAge);
  logDebug("MD RAID5 mode           = %s", (config->mdRaid5ModeEnabled
                                            ? "on" : "off"));
  logDebug("Read cache mode         = %s", (config->readCacheEnabled
                                            ? "enabled" : "disabled"));
  logDebug("Read cache extra blocks = %u", config->readCacheExtraBlocks);
  logDebug("Write policy            = %s", getConfigWritePolicyString(config));

  // The threadConfig will be copied by the VDO if it's successfully
  // created.
  VDOLoadConfig loadConfig = {
    .cacheSize    = config->cacheSize,
    .threadConfig = NULL,
    .writePolicy  = config->writePolicy,
    .maximumAge   = config->blockMapMaximumAge,
  };

  // Henceforth it is the kernel layer's responsibility to clean up the
  // DeviceConfig in case of error.
  char        *failureReason;
  KernelLayer *layer;
  result = makeKernelLayer(physicalBlocks, ti->begin, dev, instance,
                           config, &kvdoDevice.kobj, &loadConfig.threadConfig,
                           &failureReason, &layer);
  if (result != VDO_SUCCESS) {
    logError("Could not create kernel physical layer. (VDO error %d,"
             " message %s)", result, failureReason);
    cleanupInitialize(ti, NULL, dev, loadConfig.threadConfig, NULL, instance,
                      failureReason);
    return result;
  }

  // Now that we have read the geometry, we can finish setting up the
  // VDOLoadConfig.
  loadConfig.firstBlockOffset = getDataRegionOffset(layer->geometry);
  loadConfig.nonce            = layer->geometry.nonce;

  if (config->cacheSize < (2 * MAXIMUM_USER_VIOS
                   * loadConfig.threadConfig->logicalZoneCount)) {
    logWarning("Insufficient block map cache for logical zones");
    cleanupInitialize(ti, NULL, dev, loadConfig.threadConfig, layer, instance,
                      "Insufficient block map cache for logical zones");
    return VDO_BAD_CONFIGURATION;
  }

  result = startKernelLayer(layer, &loadConfig, &failureReason);
  freeThreadConfig(&loadConfig.threadConfig);
  if (result != VDO_SUCCESS) {
    logError("Could not start kernel physical layer. (VDO error %d,"
             " message %s)", result, failureReason);
    cleanupInitialize(ti, NULL, dev, NULL, layer, instance, failureReason);
    return result;
  }

  layer->ti   = ti;
  ti->private = layer;
  configureTargetCapabilities(ti, layer);

  logInfo("device '%s' started", config->poolName);
  return VDO_SUCCESS;
}

/**
 * Release our reference to the old device and swap in the new dm target
 * structure and underlying device.  If there is no old reference increment
 * the reference count on the layer to prevent it being released before the
 * destructor for the old target occurs.
 *
 * @param layer  The layer in question
 * @param ti     The new target structure
 * @param dev    The new underlying device
 **/
static void convertToNewDevice(KernelLayer      *layer,
                               struct dm_target *ti,
                               struct dm_dev    *dev)
{
  if (layer->oldTI != NULL) {
    logDebug("Releasing ref by %p  to %p", layer->oldTI, layer->oldDev);
    dm_put_device(layer->oldTI, layer->oldDev);
  } else {
    kobject_get(&layer->kobj);
  }
  layer->oldTI  = layer->ti;
  layer->oldDev = layer->dev;
  layer->ti     = ti;
  layer->dev    = dev;
}

/**********************************************************************/
static int vdoCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  // Mild hack to avoid bumping instance number when we needn't.
  char *poolName;
  int result = getPoolNameFromArgv(argc, argv, &ti->error, &poolName);
  if (result != VDO_SUCCESS) {
    return -EINVAL;
  }

  RegisteredThread allocatingThread;
  registerAllocatingThread(&allocatingThread, NULL);

  KernelLayer *oldLayer = getLayerByName(poolName);
  unsigned int instance;
  if (oldLayer == NULL) {
    result = allocateKVDOInstance(&instance);
    if (result != VDO_SUCCESS) {
      unregisterAllocatingThread();
      return -ENOMEM;
    }
  } else {
    instance = oldLayer->instance;
  }

  RegisteredThread instanceThread;
  registerThreadDeviceID(&instanceThread, &instance);

  DeviceConfig *config = NULL;
  result = parseDeviceConfig(argc, argv, &ti->error, &config);
  if (result != VDO_SUCCESS) {
    unregisterThreadDeviceID();
    unregisterAllocatingThread();
    releaseKVDOInstance(instance);
    return -EINVAL;
  }

  // Is there already a device of this name?
  if (oldLayer != NULL) {
    if (getKernelLayerState(oldLayer) != LAYER_SUSPENDED) {
      logError("Can't modify already-existing VDO named %s that isn't"
               " suspended", poolName);
      freeDeviceConfig(&config);
      unregisterThreadDeviceID();
      unregisterAllocatingThread();
      releaseKVDOInstance(instance);
      return -EINVAL;
    }

    /*
     * Applying the new table here is technically incorrect. Most
     * devices don't apply the new table until they go through a
     * suspend/resume cycle, and if they fail to apply the new table
     * in their preresume step, they remain in a suspended state without a
     * valid table. We want to apply some modifications without a suspend
     * and resume cycle, and if the modifications are invalid we want to
     * remain active rather than suspended, so we apply the changes here
     * instead of in preresume.
     */
    logInfo("modifying device '%s'", config->poolName);
    ti->private = oldLayer;

    struct dm_dev *newDev;
    result = getUnderlyingDevice(ti, config->parentDeviceName, &newDev);
    if (result == 0) {
     struct request_queue *requestQueue = bdev_get_queue(newDev->bdev);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,7,0)
    bool flushSupported
      = ((requestQueue->queue_flags & (1ULL << QUEUE_FLAG_WC)) != 0);
#else
    bool flushSupported
      = ((requestQueue->flush_flags & REQ_FLUSH) == REQ_FLUSH);
#endif
     resolveConfigWithFlushSupport(config, flushSupported);

     result = modifyKernelLayer(oldLayer, ti, config, &ti->error);
      if (result != VDO_SUCCESS) {
        result = mapToSystemError(result);
        dm_put_device(ti, newDev);
        freeDeviceConfig(&config);
      } else {
        configureTargetCapabilities(ti, oldLayer);
        convertToNewDevice(oldLayer, ti, newDev);
        DeviceConfig *oldConfig = oldLayer->deviceConfig;
        oldLayer->deviceConfig = config;
        freeDeviceConfig(&oldConfig);
      }
    } else {
      freeDeviceConfig(&config);
      logError("Could not find underlying device");
    }
    unregisterThreadDeviceID();
    unregisterAllocatingThread();
    return result;
  }

  // Henceforth, the config will be freed within any failing function and it
  // is the kernel layer's responsibility to free when appropriate.
  result = vdoInitialize(ti, instance, config);
  if (result != VDO_SUCCESS) {
    // vdoInitialize calls into various VDO routines, so map error
    result = mapToSystemError(result);
  }

  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return result;
}

/**********************************************************************/
static void vdoDtr(struct dm_target *ti)
{
  KernelLayer *layer = ti->private;
  if (layer->ti != ti) {
    /*
     * This must be the destructor associated with a reload.
     *
     * We cannot access anything that may have been cleaned up by a previous
     * invocation of the destructor.  That is, there's no guarantee that this
     * code path is being executed before one that actually tore down the
     * internals of the layer.
     *
     * Only perform the put on the device and kobject if the dm_target is the
     * specific target tracked in the layer's oldTI field.  This allows
     * multiple construction/destruction associated with the same layer (e.g.,
     * as a result of multiple dmsetup reloads) without incorrectly destructing
     * the layer.
     */
    if (layer->oldTI == ti) {
      logDebug("Releasing reference by old ti %p to dev %p", layer->oldTI,
               layer->oldDev);
      dm_put_device(layer->oldTI, layer->oldDev);
      layer->oldTI  = NULL;
      layer->oldDev = NULL;
      kobject_put(&layer->kobj);
    }
    return;
  }

  struct dm_dev *dev      = layer->dev;
  unsigned int   instance = layer->instance;

  RegisteredThread allocatingThread, instanceThread;
  registerThreadDeviceID(&instanceThread, &instance);
  registerAllocatingThread(&allocatingThread, NULL);

  waitForNoRequestsActive(layer);
  logInfo("stopping device '%s'", layer->deviceConfig->poolName);

  if (layer->dumpOnShutdown) {
    vdoDumpAll(layer, "device shutdown");
  }

  // Copy the device name (for logging) out of the layer since it's about to be
  // freed.
  const char *poolName;
  char *poolNameToFree;
  int result = duplicateString(layer->deviceConfig->poolName, "pool name",
                               &poolNameToFree);
  poolName = (result == VDO_SUCCESS) ? poolNameToFree : "unknown";

  freeKernelLayer(layer);
  dm_put_device(ti, dev);

  logInfo("device '%s' stopped", poolName);
  poolName = NULL;
  FREE(poolNameToFree);
  poolNameToFree = NULL;

  unregisterThreadDeviceID();
  unregisterAllocatingThread();
}

/**********************************************************************/
static void vdoPostsuspend(struct dm_target *ti)
{
  KernelLayer *layer = ti->private;
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);
  const char *poolName = layer->deviceConfig->poolName;
  logInfo("suspending device '%s'", poolName);
  int result = suspendKernelLayer(layer);
  if (result == VDO_SUCCESS) {
    logInfo("device '%s' suspended", poolName);
  } else {
    logError("suspend of device '%s' failed with error: %d", poolName, result);
  }
  unregisterThreadDeviceID();
}

/**********************************************************************/
static int vdoPreresume(struct dm_target *ti)
{
  KernelLayer *layer = ti->private;
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);
  logInfo("resuming device '%s'", layer->deviceConfig->poolName);
  int result = resumeKernelLayer(layer);
  if (result != VDO_SUCCESS) {
    logError("resume of device '%s' failed with error: %d",
             layer->deviceConfig->poolName, result);
  }
  unregisterThreadDeviceID();
  return mapToSystemError(result);
}

/**********************************************************************/
static void vdoResume(struct dm_target *ti)
{
  KernelLayer *layer = ti->private;
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);
  logInfo("device '%s' resumed", layer->deviceConfig->poolName);
  unregisterThreadDeviceID();
}

static struct target_type vdoTargetBio = {
  .name            = "vdo",
  .version         = {
    VDO_VERSION_MAJOR, VDO_VERSION_MINOR, VDO_VERSION_MICRO,
  },
  .module          = THIS_MODULE,
  .ctr             = vdoCtr,
  .dtr             = vdoDtr,
  .io_hints        = vdoIoHints,
  .iterate_devices = vdoIterateDevices,
  .map             = vdoMapBio,
  .message         = vdoMessage,
  .status          = vdoStatus,
  .postsuspend     = vdoPostsuspend,
  .preresume       = vdoPreresume,
  .resume          = vdoResume,
};

static bool dmRegistered     = false;
static bool sysfsInitialized = false;

/**********************************************************************/
static void vdoDestroy(void)
{
  logDebug("in %s", __func__);

  kvdoDevice.status = SHUTTING_DOWN;

  if (sysfsInitialized) {
    vdoPutSysfs(&kvdoDevice.kobj);
  }
  vdoDestroyProcfs();

  kvdoDevice.status = UNINITIALIZED;

  if (dmRegistered) {
    dm_unregister_target(&vdoTargetBio);
  }

  cleanUpInstanceNumberTracking();

  logInfo("unloaded version %s", CURRENT_VERSION);
}

/**********************************************************************/
static int __init vdoInit(void)
{
  int result = 0;

  initializeThreadDeviceRegistry();
  initializeStandardErrorBlocks();
  initializeDeviceRegistryOnce();
  logInfo("loaded version %s", CURRENT_VERSION);

  result = dm_register_target(&vdoTargetBio);
  if (result < 0) {
    logError("dm_register_target failed %d", result);
    vdoDestroy();
    return result;
  }
  dmRegistered = true;

  kvdoDevice.status = UNINITIALIZED;

  vdoInitProcfs();
  /*
   * Set up global sysfs stuff
   */
  result = vdoInitSysfs(&kvdoDevice.kobj);
  if (result < 0) {
    logError("sysfs initialization failed %d", result);
    vdoDestroy();
    // vdoInitSysfs only returns system error codes
    return result;
  }
  sysfsInitialized = true;

  initWorkQueueOnce();
  initializeTraceLoggingOnce();
  initKernelVDOOnce();
  initializeInstanceNumberTracking();

  kvdoDevice.status = READY;
  return result;
}

/**********************************************************************/
static void __exit vdoExit(void)
{
  vdoDestroy();
}

module_init(vdoInit);
module_exit(vdoExit);

MODULE_DESCRIPTION(DM_NAME " target for transparent deduplication");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
