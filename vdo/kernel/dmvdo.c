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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dmvdo.c#10 $
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
#include "ioSubmitter.h"
#include "kernelLayer.h"
#include "kvdoFlush.h"
#include "memoryUsage.h"
#include "statusProcfs.h"
#include "stringUtils.h"
#include "sysfs.h"
#include "threadDevice.h"
#include "threadRegistry.h"

struct kvdo_module_globals kvdoGlobals;

/*
 * Pre kernel version 4.3, we use the functionality in blkdev_issue_discard
 * and the value in max_discard_sectors to split large discards into smaller
 * ones. 4.3 to 4.18 kernels have removed the code in blkdev_issue_discard
 * and so in place of that, we use the code in device mapper itself to
 * split the discards. Unfortunately, it uses the same value to split large
 * discards as it does to split large data bios. 
 *
 * In kernel version 4.18, support for splitting discards was added
 * back into blkdev_issue_discard. Since this mode of splitting 
 * (based on max_discard_sectors) is preferable to splitting always
 * on 4k, we are turning off the device mapper splitting from 4.18
 * on.
 */
#define HAS_NO_BLKDEV_SPLIT LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0) \
                            && LINUX_VERSION_CODE < KERNEL_VERSION(4,18,0)

/**********************************************************************/

/**
 * Get the kernel layer associated with a dm target structure.
 *
 * @param ti  The dm target structure
 *
 * @return The kernel layer, or NULL.
 **/
static KernelLayer *getKernelLayerForTarget(struct dm_target *ti)
{
  return ((struct device_config *) ti->private)->layer;
}

/**
 * Begin VDO processing of a bio.  This is called by the device mapper
 * through the "map" function, and has resulted from a call to either
 * submit_bio or generic_make_request.
 *
 * @param ti      The dm_target.  We only need the "private" member to give
 *                us the KernelLayer.
 * @param bio     The bio.
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
static int vdoMapBio(struct dm_target *ti, struct bio *bio)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
  return kvdoMapBio(layer, bio);
}

/**********************************************************************/
static void vdoIoHints(struct dm_target *ti, struct queue_limits *limits)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);

  limits->logical_block_size  = layer->deviceConfig->logicalBlockSize;
  limits->physical_block_size = VDO_BLOCK_SIZE;

  // The minimum io size for random io
  blk_limits_io_min(limits, VDO_BLOCK_SIZE);
  // The optimal io size for streamed/sequential io
  blk_limits_io_opt(limits, VDO_BLOCK_SIZE);

  /*
   * Sets the maximum discard size that will be passed into VDO. This value
   * comes from a table line value passed in during dmsetup create.
   *
   * The value 1024 is the largest usable value on HD systems.  A 2048 sector
   * discard on a busy HD system takes 31 seconds.  We should use a value no
   * higher than 1024, which takes 15 to 16 seconds on a busy HD system.
   *
   * But using large values results in 120 second blocked task warnings in
   * /var/log/kern.log.  In order to avoid these warnings, we choose to use the
   * smallest reasonable value.  See VDO-3062 and VDO-3087.
   *
   * We allow setting of the value for max_discard_sectors even in situations 
   * where we only split on 4k (see comments for HAS_NO_BLKDEV_SPLIT) as the
   * value is still used in other code, like sysfs display of queue limits and 
   * most especially in dm-thin to determine whether to pass down discards.
   */
  limits->max_discard_sectors 
    = layer->deviceConfig->maxDiscardBlocks * VDO_SECTORS_PER_BLOCK;

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
  KernelLayer *layer = getKernelLayerForTarget(ti);
  sector_t len = blockToSector(layer, layer->deviceConfig->physicalBlocks);

  return fn(ti, layer->deviceConfig->ownedDevice, 0, len, data);
}

/*
 * Status line is:
 *    <device> <operating mode> <in recovery> <index state>
 *    <compression state> <used physical blocks> <total physical blocks>
 */

/**********************************************************************/
static void vdoStatus(struct dm_target *ti,
                      status_type_t     status_type,
                      unsigned int      status_flags,
                      char             *result,
                      unsigned int      maxlen)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
  char nameBuffer[BDEVNAME_SIZE];
  // N.B.: The DMEMIT macro uses the variables named "sz", "result", "maxlen".
  int sz = 0;

  switch (status_type) {
  case STATUSTYPE_INFO:
    // Report info for dmsetup status
    mutex_lock(&layer->statsMutex);
    getKVDOStatistics(&layer->kvdo, &layer->vdoStatsStorage);
    VDOStatistics *stats = &layer->vdoStatsStorage;
    DMEMIT("/dev/%s %s %s %s %s %llu %llu",
           bdevname(getKernelLayerBdev(layer), nameBuffer),
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
    DMEMIT("%s", ((struct device_config *) ti->private)->originalString);
    break;
  }

//  spin_unlock_irqrestore(&layer->lock, flags);
}


/**
 * Get the size of the underlying device, in blocks.
 *
 * @param [in] layer  The layer
 *
 * @return The size in blocks
 **/
static BlockCount getUnderlyingDeviceBlockCount(KernelLayer *layer)
{
  uint64_t physicalSize = i_size_read(getKernelLayerBdev(layer)->bd_inode);
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
    logWarning("Logical block count \"%llu\" exceeds the maximum (%"
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
      return prepareToResizePhysical(layer,
                                     getUnderlyingDeviceBlockCount(layer));
    }

    if (strcasecmp(argv[0], "growPhysical") == 0) {
      // The actual growPhysical will happen when the device is resumed.

      if (layer->deviceConfig->version != 0) {
        // XXX Uncomment this branch when new VDO manager is updated to not
        // send this message.

        // Old style message on new style table is unexpected; it means the
        // user started the VDO with new manager and is growing with old.
        // logInfo("Mismatch between growPhysical method and table version.");
        // return -EINVAL;
      } else {
        layer->deviceConfig->physicalBlocks
          = getUnderlyingDeviceBlockCount(layer);
      }
      return 0;
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
static int vdoMessage(struct dm_target  *ti,
                      unsigned int       argc,
                      char             **argv,
                      char              *resultBuffer,
                      unsigned int       maxlen)
#else
static int vdoMessage(struct dm_target *ti, unsigned int argc, char **argv)
#endif
{
  if (argc == 0) {
    logWarning("unspecified dmsetup message");
    return -EINVAL;
  }

  KernelLayer *layer = getKernelLayerForTarget(ti);
  RegisteredThread allocatingThread, instanceThread;
  registerAllocatingThread(&allocatingThread, NULL);
  registerThreadDevice(&instanceThread, layer);
  int result = processVDOMessage(layer, argc, argv);
  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return mapToSystemError(result);
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
  ti->discards_supported = 1;

  /**
   * This may appear to indicate we don't support flushes in sync mode.
   * However, dm will set up the request queue to accept flushes if any
   * device in the stack accepts flushes. Hence if the device under VDO
   * accepts flushes, we will receive flushes.
   **/
  ti->flush_supported = shouldProcessFlush(layer);
  ti->num_discard_bios = 1;
  ti->num_flush_bios = 1;

  // If this value changes, please make sure to update the
  // value for maxDiscardSectors accordingly.
  BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);

/*
 * Please see comments above where the macro is defined.
 */
#if HAS_NO_BLKDEV_SPLIT
  ti->split_discard_bios = 1;
#endif
  dm_table_add_target_callbacks(ti->table, &layer->callbacks);
}

/**
 * Handle a vdoInitialize failure, freeing all appropriate structures.
 *
 * @param ti            The device mapper target representing our device
 * @param threadConfig  The thread config (possibly NULL)
 * @param layer         The kernel layer (possibly NULL)
 * @param instance      The instance number to be released
 * @param why           The reason for failure
 **/
static void cleanupInitialize(struct dm_target *ti,
                              ThreadConfig     *threadConfig,
                              KernelLayer      *layer,
                              unsigned int      instance,
                              char             *why)
{
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
static int vdoInitialize(struct dm_target     *ti,
                         unsigned int          instance,
                         struct device_config *config)
{
  logInfo("starting device '%s'", config->poolName);

  uint64_t   blockSize      = VDO_BLOCK_SIZE;
  uint64_t   logicalSize    = to_bytes(ti->len);
  BlockCount logicalBlocks  = logicalSize / blockSize;

  logDebug("Logical block size     = %llu",
           (uint64_t) config->logicalBlockSize);
  logDebug("Logical blocks         = %llu", logicalBlocks);
  logDebug("Physical block size    = %llu", (uint64_t) blockSize);
  logDebug("Physical blocks        = %llu", config->physicalBlocks);
  logDebug("Block map cache blocks = %u", config->cacheSize);
  logDebug("Block map maximum age  = %u", config->blockMapMaximumAge);
  logDebug("MD RAID5 mode          = %s", (config->mdRaid5ModeEnabled
                                           ? "on" : "off"));
  logDebug("Write policy           = %s",
           get_config_write_policy_string(config));

  // The threadConfig will be copied by the VDO if it's successfully
  // created.
  VDOLoadConfig loadConfig = {
    .cacheSize    = config->cacheSize,
    .threadConfig = NULL,
    .writePolicy  = config->writePolicy,
    .maximumAge   = config->blockMapMaximumAge,
  };

  char        *failureReason;
  KernelLayer *layer;
  int result = makeKernelLayer(ti->begin, instance, config,
                               &kvdoGlobals.kobj, &loadConfig.threadConfig,
                               &failureReason, &layer);
  if (result != VDO_SUCCESS) {
    logError("Could not create kernel physical layer. (VDO error %d,"
             " message %s)", result, failureReason);
    cleanupInitialize(ti, loadConfig.threadConfig, NULL, instance,
                      failureReason);
    return result;
  }

  // Now that we have read the geometry, we can finish setting up the
  // VDOLoadConfig.
  setLoadConfigFromGeometry(&layer->geometry, &loadConfig);

  if (config->cacheSize < (2 * MAXIMUM_USER_VIOS
                   * loadConfig.threadConfig->logicalZoneCount)) {
    logWarning("Insufficient block map cache for logical zones");
    cleanupInitialize(ti, loadConfig.threadConfig, layer, instance,
                      "Insufficient block map cache for logical zones");
    return VDO_BAD_CONFIGURATION;
  }

  // Henceforth it is the kernel layer's responsibility to clean up the
  // ThreadConfig.
  result = startKernelLayer(layer, &loadConfig, &failureReason);
  if (result != VDO_SUCCESS) {
    logError("Could not start kernel physical layer. (VDO error %d,"
             " message %s)", result, failureReason);
    cleanupInitialize(ti, NULL, layer, instance, failureReason);
    return result;
  }

  acquireKernelLayerReference(layer, config);
  setKernelLayerActiveConfig(layer, config);
  ti->private = config;
  configureTargetCapabilities(ti, layer);

  logInfo("device '%s' started", config->poolName);
  return VDO_SUCCESS;
}

/**********************************************************************/
static int vdoCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  // Mild hack to avoid bumping instance number when we needn't.
  char *poolName;
  int result = get_pool_name_from_argv(argc, argv, &ti->error, &poolName);
  if (result != VDO_SUCCESS) {
    return -EINVAL;
  }

  RegisteredThread allocatingThread;
  registerAllocatingThread(&allocatingThread, NULL);

  KernelLayer *oldLayer = get_layer_by_name(poolName);
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

  bool verbose = (oldLayer == NULL);
  struct device_config *config = NULL;
  result = parse_device_config(argc, argv, ti, verbose, &config);
  if (result != VDO_SUCCESS) {
    unregisterThreadDeviceID();
    unregisterAllocatingThread();
    if (oldLayer == NULL) {
      releaseKVDOInstance(instance);
    }
    return -EINVAL;
  }

  // Is there already a device of this name?
  if (oldLayer != NULL) {
    /*
     * To preserve backward compatibility with old VDO Managers, we need to
     * allow this to happen when either suspended or not. We could assert
     * that if the config is version 0, we are suspended, and if not, we
     * are not, but we can't do that till new VDO Manager does the right
     * order.
     */
    logInfo("preparing to modify device '%s'", config->poolName);
    result = prepareToModifyKernelLayer(oldLayer, config, &ti->error);
    if (result != VDO_SUCCESS) {
      result = mapToSystemError(result);
      free_device_config(&config);
    } else {
      acquireKernelLayerReference(oldLayer, config);
      ti->private = config;
      configureTargetCapabilities(ti, oldLayer);
    }
    unregisterThreadDeviceID();
    unregisterAllocatingThread();
    return result;
  }

  result = vdoInitialize(ti, instance, config);
  if (result != VDO_SUCCESS) {
    // vdoInitialize calls into various VDO routines, so map error
    result = mapToSystemError(result);
    free_device_config(&config);
  }

  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return result;
}

/**********************************************************************/
static void vdoDtr(struct dm_target *ti)
{
  struct device_config *config = ti->private;
  KernelLayer  *layer  = getKernelLayerForTarget(ti);

  releaseKernelLayerReference(layer, config);

  if (layer->configReferences == 0) {
    // This was the last config referencing the layer. Free it.
    unsigned int instance = layer->instance;
    RegisteredThread allocatingThread, instanceThread;
    registerThreadDeviceID(&instanceThread, &instance);
    registerAllocatingThread(&allocatingThread, NULL);

    waitForNoRequestsActive(layer);
    logInfo("stopping device '%s'", config->poolName);

    if (layer->dumpOnShutdown) {
      vdoDumpAll(layer, "device shutdown");
    }

    freeKernelLayer(layer);
    logInfo("device '%s' stopped", config->poolName);
    unregisterThreadDeviceID();
    unregisterAllocatingThread();
  }

  free_device_config(&config);
  ti->private = NULL;
}

/**********************************************************************/
static void vdoPresuspend(struct dm_target *ti)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);  
  if (dm_noflush_suspending(ti)) {
    layer->noFlushSuspend = true;
  }
  unregisterThreadDeviceID();
}

/**********************************************************************/
static void vdoPostsuspend(struct dm_target *ti)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
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
  layer->noFlushSuspend = false;
  unregisterThreadDeviceID();
}

/**********************************************************************/
static int vdoPreresume(struct dm_target *ti)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
  struct device_config *config = ti->private;
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);
  logInfo("resuming device '%s'", config->poolName);

  // This is a noop if nothing has changed, and by calling it every time
  // we capture old-style growPhysicals, which change the config in place.
  int result = modifyKernelLayer(layer, config);
  if (result != VDO_SUCCESS) {
    logErrorWithStringError(result, "Commit of modifications to device '%s'"
                            " failed", config->poolName);
    setKernelLayerActiveConfig(layer, config);
    setKVDOReadOnly(&layer->kvdo, result);
  } else {
    setKernelLayerActiveConfig(layer, config);
    result = resumeKernelLayer(layer);
    if (result != VDO_SUCCESS) {
      logError("resume of device '%s' failed with error: %d",
	       layer->deviceConfig->poolName, result);
    }
  }
  unregisterThreadDeviceID();
  return mapToSystemError(result);
}

/**********************************************************************/
static void vdoResume(struct dm_target *ti)
{
  KernelLayer *layer = getKernelLayerForTarget(ti);
  RegisteredThread instanceThread;
  registerThreadDevice(&instanceThread, layer);
  logInfo("device '%s' resumed", layer->deviceConfig->poolName);
  unregisterThreadDeviceID();
}

/* 
 * If anything changes that affects how user tools will interact
 * with vdo, update the version number and make sure 
 * documentation about the change is complete so tools can
 * properly update their management code.
 */
static struct target_type vdoTargetBio = {
  .name            = "vdo",
  .version         = {6, 2, 0},
  .module          = THIS_MODULE,
  .ctr             = vdoCtr,
  .dtr             = vdoDtr,
  .io_hints        = vdoIoHints,
  .iterate_devices = vdoIterateDevices,
  .map             = vdoMapBio,
  .message         = vdoMessage,
  .status          = vdoStatus,
  .presuspend      = vdoPresuspend,
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

  kvdoGlobals.status = SHUTTING_DOWN;

  if (sysfsInitialized) {
    vdoPutSysfs(&kvdoGlobals.kobj);
  }
  vdoDestroyProcfs();

  kvdoGlobals.status = UNINITIALIZED;

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
  initialize_device_registry_once();
  logInfo("loaded version %s", CURRENT_VERSION);

  result = dm_register_target(&vdoTargetBio);
  if (result < 0) {
    logError("dm_register_target failed %d", result);
    vdoDestroy();
    return result;
  }
  dmRegistered = true;

  kvdoGlobals.status = UNINITIALIZED;

  vdoInitProcfs();

  result = vdoInitSysfs(&kvdoGlobals.kobj);
  if (result < 0) {
    logError("sysfs initialization failed %d", result);
    vdoDestroy();
    // vdoInitSysfs only returns system error codes
    return result;
  }
  sysfsInitialized = true;

  initWorkQueueOnce();
  initializeTraceLoggingOnce();
  initializeInstanceNumberTracking();

  kvdoGlobals.status = READY;
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
