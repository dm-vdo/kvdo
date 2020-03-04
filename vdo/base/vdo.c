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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.c#38 $
 */

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdoInternal.h"

#include "buffer.h"
#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "extent.h"
#include "hashZone.h"
#include "header.h"
#include "logicalZone.h"
#include "numUtils.h"
#include "packer.h"
#include "physicalZone.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "statistics.h"
#include "statusCodes.h"
#include "threadConfig.h"
#include "vdoLayout.h"
#include "vioWrite.h"
#include "volumeGeometry.h"

/**
 * The master version of the on-disk format of a VDO. This should be
 * incremented any time the on-disk representation of any VDO structure
 * changes. Changes which require only online upgrade steps should increment
 * the minor version. Changes which require an offline upgrade or which can not
 * be upgraded to at all should increment the major version and set the minor
 * version to 0.
 **/
static const struct version_number VDO_MASTER_VERSION_67_0 = {
  .major_version = 67,
  .minor_version =  0,
};

/**
 * The current version for the data encoded in the super block. This must
 * be changed any time there is a change to encoding of the component data
 * of any VDO component.
 **/
static const struct version_number VDO_COMPONENT_DATA_41_0 = {
  .major_version = 41,
  .minor_version =  0,
};

/**
 * This is the structure that captures the vdo fields saved as a SuperBlock
 * component.
 **/
struct vdo_component_41_0 {
  VDOState  state;
  uint64_t  completeRecoveries;
  uint64_t  readOnlyRecoveries;
  VDOConfig config;
  Nonce     nonce;
} __attribute__((packed));

/**********************************************************************/
int allocateVDO(PhysicalLayer *layer, struct vdo **vdoPtr)
{
  int result = register_status_codes();
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct vdo *vdo;
  result = ALLOCATE(1, struct vdo, __func__, &vdo);
  if (result != UDS_SUCCESS) {
    return result;
  }

  vdo->layer = layer;
  if (layer->createEnqueueable != NULL) {
    result = initialize_admin_completion(vdo, &vdo->adminCompletion);
    if (result != VDO_SUCCESS) {
      freeVDO(&vdo);
      return result;
    }
  }

  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeVDO(PhysicalLayer *layer, struct vdo **vdoPtr)
{
  struct vdo *vdo;
  int result = allocateVDO(layer, &vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeZeroThreadConfig(&vdo->loadConfig.threadConfig);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
void destroyVDO(struct vdo *vdo)
{
  free_flusher(&vdo->flusher);
  free_packer(&vdo->packer);
  free_recovery_journal(&vdo->recoveryJournal);
  free_slab_depot(&vdo->depot);
  freeVDOLayout(&vdo->layout);
  free_super_block(&vdo->superBlock);
  freeBlockMap(&vdo->blockMap);

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (vdo->hashZones != NULL) {
    ZoneCount zone;
    for (zone = 0; zone < threadConfig->hashZoneCount; zone++) {
      free_hash_zone(&vdo->hashZones[zone]);
    }
  }
  FREE(vdo->hashZones);
  vdo->hashZones = NULL;

  free_logical_zones(&vdo->logicalZones);

  if (vdo->physicalZones != NULL) {
    ZoneCount zone;
    for (zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
      free_physical_zone(&vdo->physicalZones[zone]);
    }
  }
  FREE(vdo->physicalZones);
  vdo->physicalZones = NULL;

  uninitialize_admin_completion(&vdo->adminCompletion);
  free_read_only_notifier(&vdo->readOnlyNotifier);
  freeThreadConfig(&vdo->loadConfig.threadConfig);
}

/**********************************************************************/
void freeVDO(struct vdo **vdoPtr)
{
  if (*vdoPtr == NULL) {
    return;
  }

  destroyVDO(*vdoPtr);
  FREE(*vdoPtr);
  *vdoPtr = NULL;
}

/**********************************************************************/
VDOState getVDOState(const struct vdo *vdo)
{
  return atomicLoad32(&vdo->state);
}

/**********************************************************************/
void setVDOState(struct vdo *vdo, VDOState state)
{
  atomicStore32(&vdo->state, state);
}

/**********************************************************************/
size_t getComponentDataSize(struct vdo *vdo)
{
  return (sizeof(struct version_number)
          + sizeof(struct version_number)
          + sizeof(struct vdo_component_41_0)
          + getVDOLayoutEncodedSize(vdo->layout)
          + get_recovery_journal_encoded_size()
          + get_slab_depot_encoded_size()
          + getBlockMapEncodedSize());
}

/**
 * Encode the vdo master version.
 *
 * @param buffer  The buffer in which to encode the version
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeMasterVersion(Buffer *buffer)
{
  return encode_version_number(VDO_MASTER_VERSION_67_0, buffer);
}

/**
 * Encode a VDOConfig structure into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeVDOConfig(const VDOConfig *config, Buffer *buffer)
{
  int result = putUInt64LEIntoBuffer(buffer, config->logicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->physicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->slabSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, config->recoveryJournalSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return putUInt64LEIntoBuffer(buffer, config->slabJournalBlocks);
}

/**
 * Encode the component data for the vdo itself.
 *
 * @param vdo     The vdo to encode
 * @param buffer  The buffer in which to encode the vdo
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeVDOComponent(const struct vdo *vdo, Buffer *buffer)
{
  int result = encode_version_number(VDO_COMPONENT_DATA_41_0, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = putUInt32LEIntoBuffer(buffer, getVDOState(vdo));
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, vdo->completeRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, vdo->readOnlyRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeVDOConfig(&vdo->config, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = putUInt64LEIntoBuffer(buffer, vdo->nonce);
  if (result != VDO_SUCCESS) {
    return result;
  }

  size_t encodedSize = contentLength(buffer) - initialLength;
  return ASSERT(encodedSize == sizeof(struct vdo_component_41_0),
                "encoded VDO component size must match structure size");
}

/**********************************************************************/
static int encodeVDO(struct vdo *vdo)
{
  Buffer *buffer = get_component_buffer(vdo->superBlock);
  int result = resetBufferEnd(buffer, 0);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeMasterVersion(buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeVDOComponent(vdo, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeVDOLayout(vdo->layout, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encode_recovery_journal(vdo->recoveryJournal, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encode_slab_depot(vdo->depot, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeBlockMap(vdo->blockMap, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ASSERT_LOG_ONLY((contentLength(buffer) == getComponentDataSize(vdo)),
                  "All super block component data was encoded");
  return VDO_SUCCESS;
}

/**********************************************************************/
int saveVDOComponents(struct vdo *vdo)
{
  int result = encodeVDO(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return save_super_block(vdo->layer, vdo->superBlock,
                          getFirstBlockOffset(vdo));
}

/**********************************************************************/
void saveVDOComponentsAsync(struct vdo *vdo, struct vdo_completion *parent)
{
  int result = encodeVDO(vdo);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  save_super_block_async(vdo->superBlock, getFirstBlockOffset(vdo), parent);
}

/**********************************************************************/
int saveReconfiguredVDO(struct vdo *vdo)
{
  Buffer *buffer         = get_component_buffer(vdo->superBlock);
  size_t  componentsSize = contentLength(buffer);

  byte *components;
  int   result = copyBytes(buffer, componentsSize, &components);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = resetBufferEnd(buffer, 0);
  if (result != VDO_SUCCESS) {
    FREE(components);
    return result;
  }

  result = encodeMasterVersion(buffer);
  if (result != VDO_SUCCESS) {
    FREE(components);
    return result;
  }

  result = encodeVDOComponent(vdo, buffer);
  if (result != VDO_SUCCESS) {
    FREE(components);
    return result;
  }

  result = putBytes(buffer, componentsSize, components);
  FREE(components);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return save_super_block(vdo->layer, vdo->superBlock,
                          getFirstBlockOffset(vdo));
}

/**********************************************************************/
int decodeVDOVersion(struct vdo *vdo)
{
  return decode_version_number(get_component_buffer(vdo->superBlock),
                               &vdo->loadVersion);
}

/**********************************************************************/
int validateVDOVersion(struct vdo *vdo)
{
  int result = decodeVDOVersion(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ReleaseVersionNumber loadedReleaseVersion
    = get_loaded_release_version(vdo->superBlock);
  if (vdo->loadConfig.releaseVersion != loadedReleaseVersion) {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "Geometry release version %" PRIu32 " does "
                                   "not match super block release version %"
                                   PRIu32,
                                   vdo->loadConfig.releaseVersion,
                                   loadedReleaseVersion);
  }

  return validate_version(VDO_MASTER_VERSION_67_0, vdo->loadVersion, "master");
}

/**
 * Decode a VDOConfig structure from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static int decodeVDOConfig(Buffer *buffer, VDOConfig *config)
{
  BlockCount logicalBlocks;
  int result = getUInt64LEFromBuffer(buffer, &logicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount physicalBlocks;
  result = getUInt64LEFromBuffer(buffer, &physicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount slabSize;
  result = getUInt64LEFromBuffer(buffer, &slabSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount recoveryJournalSize;
  result = getUInt64LEFromBuffer(buffer, &recoveryJournalSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount slabJournalBlocks;
  result = getUInt64LEFromBuffer(buffer, &slabJournalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *config = (VDOConfig) {
    .logicalBlocks       = logicalBlocks,
    .physicalBlocks      = physicalBlocks,
    .slabSize            = slabSize,
    .recoveryJournalSize = recoveryJournalSize,
    .slabJournalBlocks   = slabJournalBlocks,
  };
  return VDO_SUCCESS;
}

/**
 * Decode the version 41.0 component state for the vdo itself from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int decodeVDOComponent_41_0(Buffer                    *buffer,
                                   struct vdo_component_41_0 *state)
{
  size_t initialLength = contentLength(buffer);

  VDOState vdoState;
  int result = getUInt32LEFromBuffer(buffer, &vdoState);
  if (result != VDO_SUCCESS) {
    return result;
  }

  uint64_t completeRecoveries;
  result = getUInt64LEFromBuffer(buffer, &completeRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  uint64_t readOnlyRecoveries;
  result = getUInt64LEFromBuffer(buffer, &readOnlyRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDOConfig config;
  result = decodeVDOConfig(buffer, &config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  Nonce nonce;
  result = getUInt64LEFromBuffer(buffer, &nonce);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *state = (struct vdo_component_41_0) {
    .state              = vdoState,
    .completeRecoveries = completeRecoveries,
    .readOnlyRecoveries = readOnlyRecoveries,
    .config             = config,
    .nonce              = nonce,
  };

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(decodedSize == sizeof(struct vdo_component_41_0),
                "decoded VDO component size must match structure size");
}

/**********************************************************************/
int decodeVDOComponent(struct vdo *vdo)
{
  Buffer *buffer = get_component_buffer(vdo->superBlock);

  struct version_number version;
  int result = decode_version_number(buffer, &version);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validate_version(version, VDO_COMPONENT_DATA_41_0,
                            "VDO component data");
  if (result != VDO_SUCCESS) {
    return result;
  }

  struct vdo_component_41_0 component;
  result = decodeVDOComponent_41_0(buffer, &component);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Copy the decoded component into the vdo structure.
  setVDOState(vdo, component.state);
  vdo->loadState          = component.state;
  vdo->completeRecoveries = component.completeRecoveries;
  vdo->readOnlyRecoveries = component.readOnlyRecoveries;
  vdo->config             = component.config;
  vdo->nonce              = component.nonce;
  return VDO_SUCCESS;
}

/**********************************************************************/
int validateVDOConfig(const VDOConfig *config,
                      BlockCount       blockCount,
                      bool             requireLogical)
{
  int result = ASSERT(config->slabSize > 0, "slab size unspecified");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(isPowerOfTwo(config->slabSize),
                  "slab size must be a power of two");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->slabSize <= (1 << MAX_SLAB_BITS),
                  "slab size must be less than or equal to 2^%d",
                  MAX_SLAB_BITS);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ASSERT(config->slabJournalBlocks >= MINIMUM_SLAB_JOURNAL_BLOCKS,
                  "slab journal size meets minimum size");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->slabJournalBlocks <= config->slabSize,
                  "slab journal size is within expected bound");
  if (result != UDS_SUCCESS) {
    return result;
  }

  SlabConfig slabConfig;
  result = configure_slab(config->slabSize, config->slabJournalBlocks,
                          &slabConfig);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ASSERT((slabConfig.dataBlocks >= 1),
                  "slab must be able to hold at least one block");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->physicalBlocks > 0, "physical blocks unspecified");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->physicalBlocks <= MAXIMUM_PHYSICAL_BLOCKS,
                  "physical block count %llu exceeds maximum %llu",
                  config->physicalBlocks, MAXIMUM_PHYSICAL_BLOCKS);
  if (result != UDS_SUCCESS) {
    return VDO_OUT_OF_RANGE;
  }

  // This can't check equality because FileLayer et al can only known about
  // the storage size, which may not match the super block size.
  if (blockCount < config->physicalBlocks) {
    logError("A physical size of %llu blocks was specified,"
             " but that is smaller than the %llu blocks"
             " configured in the vdo super block",
             blockCount, config->physicalBlocks);
    return VDO_PARAMETER_MISMATCH;
  }

  result = ASSERT(!requireLogical || (config->logicalBlocks > 0),
                  "logical blocks unspecified");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->logicalBlocks <= MAXIMUM_LOGICAL_BLOCKS,
                  "logical blocks too large");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(config->recoveryJournalSize > 0,
                  "recovery journal size unspecified");
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = ASSERT(isPowerOfTwo(config->recoveryJournalSize),
                  "recovery journal size must be a power of two");
  if (result != UDS_SUCCESS) {
    return result;
  }

  return result;
}

/**
 * Notify a vdo that it is going read-only. This will save the read-only state
 * to the super block.
 *
 * <p>Implements ReadOnlyNotification.
 *
 * @param listener  The vdo
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void notifyVDOOfReadOnlyMode(void                  *listener,
                                    struct vdo_completion *parent)
{
  struct vdo *vdo = listener;
  if (inReadOnlyMode(vdo)) {
    completeCompletion(parent);
  }

  setVDOState(vdo, VDO_READ_ONLY_MODE);
  saveVDOComponentsAsync(vdo, parent);
}

/**********************************************************************/
int enableReadOnlyEntry(struct vdo *vdo)
{
  return register_read_only_listener(vdo->readOnlyNotifier, vdo,
                                     notifyVDOOfReadOnlyMode,
                                     getAdminThread(getThreadConfig(vdo)));
}

/**********************************************************************/
bool inReadOnlyMode(const struct vdo *vdo)
{
  return (getVDOState(vdo) == VDO_READ_ONLY_MODE);
}

/**********************************************************************/
bool wasNew(const struct vdo *vdo)
{
  return (vdo->loadState == VDO_NEW);
}

/**********************************************************************/
bool requiresReadOnlyRebuild(const struct vdo *vdo)
{
  return ((vdo->loadState == VDO_FORCE_REBUILD)
          || (vdo->loadState == VDO_REBUILD_FOR_UPGRADE));
}

/**********************************************************************/
bool requiresRebuild(const struct vdo *vdo)
{
  switch (getVDOState(vdo)) {
  case VDO_DIRTY:
  case VDO_FORCE_REBUILD:
  case VDO_REPLAYING:
  case VDO_REBUILD_FOR_UPGRADE:
    return true;

  default:
    return false;
  }
}

/**********************************************************************/
bool requiresRecovery(const struct vdo *vdo)
{
  return ((vdo->loadState == VDO_DIRTY) || (vdo->loadState == VDO_REPLAYING)
          || (vdo->loadState == VDO_RECOVERING));
}

/**********************************************************************/
bool isReplaying(const struct vdo *vdo)
{
  return (getVDOState(vdo) == VDO_REPLAYING);
}

/**********************************************************************/
bool inRecoveryMode(const struct vdo *vdo)
{
  return (getVDOState(vdo) == VDO_RECOVERING);
}

/**********************************************************************/
void enterRecoveryMode(struct vdo *vdo)
{
  assertOnAdminThread(vdo, __func__);

  if (inReadOnlyMode(vdo)) {
    return;
  }

  logInfo("Entering recovery mode");
  setVDOState(vdo, VDO_RECOVERING);
}

/**********************************************************************/
void makeVDOReadOnly(struct vdo *vdo, int errorCode)
{
  enter_read_only_mode(vdo->readOnlyNotifier, errorCode);
}

/**********************************************************************/
bool setVDOCompressing(struct vdo *vdo, bool enableCompression)
{
  bool stateChanged = compareAndSwapBool(&vdo->compressing, !enableCompression,
                                         enableCompression);
  if (stateChanged && !enableCompression) {
    // Flushing the packer is asynchronous, but we don't care when it
    // finishes.
    flush_packer(vdo->packer);
  }

  logInfo("compression is %s", (enableCompression ? "enabled" : "disabled"));
  return (stateChanged ? !enableCompression : enableCompression);
}

/**********************************************************************/
bool getVDOCompressing(struct vdo *vdo)
{
  return atomicLoadBool(&vdo->compressing);
}

/**********************************************************************/
static size_t getBlockMapCacheSize(const struct vdo *vdo)
{
  return ((size_t) vdo->loadConfig.cacheSize) * VDO_BLOCK_SIZE;
}

/**
 * Tally the hash lock statistics from all the hash zones.
 *
 * @param vdo  The vdo to query
 *
 * @return The sum of the hash lock statistics from all hash zones
 **/
static HashLockStatistics getHashLockStatistics(const struct vdo *vdo)
{
  HashLockStatistics totals;
  memset(&totals, 0, sizeof(totals));

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  ZoneCount zone;
  for (zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    HashLockStatistics stats  = get_hash_zone_statistics(vdo->hashZones[zone]);
    totals.dedupeAdviceValid        += stats.dedupeAdviceValid;
    totals.dedupeAdviceStale        += stats.dedupeAdviceStale;
    totals.concurrentDataMatches    += stats.concurrentDataMatches;
    totals.concurrentHashCollisions += stats.concurrentHashCollisions;
  }

  return totals;
}

/**
 * Get the current error statistics from a vdo.
 *
 * @param vdo  The vdo to query
 *
 * @return a copy of the current vdo error counters
 **/
static ErrorStatistics getVDOErrorStatistics(const struct vdo *vdo)
{
  /*
   * The error counts can be incremented from arbitrary threads and so must be
   * incremented atomically, but they are just statistics with no semantics
   * that could rely on memory order, so unfenced reads are sufficient.
   */
  const struct atomic_error_statistics *atoms = &vdo->errorStats;
  return (ErrorStatistics) {
    .invalidAdvicePBNCount = relaxedLoad64(&atoms->invalidAdvicePBNCount),
    .noSpaceErrorCount     = relaxedLoad64(&atoms->noSpaceErrorCount),
    .readOnlyErrorCount    = relaxedLoad64(&atoms->readOnlyErrorCount),
  };
}

/**********************************************************************/
void getVDOStatistics(const struct vdo *vdo, VDOStatistics *stats)
{
  // These are immutable properties of the vdo object, so it is safe to
  // query them from any thread.
  struct recovery_journal *journal  = vdo->recoveryJournal;
  struct slab_depot       *depot    = vdo->depot;
  // XXX config.physicalBlocks is actually mutated during resize and is in a
  // packed structure, but resize runs on the admin thread so we're usually OK.
  stats->version                    = STATISTICS_VERSION;
  stats->releaseVersion             = CURRENT_RELEASE_VERSION_NUMBER;
  stats->logicalBlocks              = vdo->config.logicalBlocks;
  stats->physicalBlocks             = vdo->config.physicalBlocks;
  stats->blockSize                  = VDO_BLOCK_SIZE;
  stats->completeRecoveries         = vdo->completeRecoveries;
  stats->readOnlyRecoveries         = vdo->readOnlyRecoveries;
  stats->blockMapCacheSize          = getBlockMapCacheSize(vdo);
  snprintf(stats->writePolicy, sizeof(stats->writePolicy), "%s",
           ((getWritePolicy(vdo) == WRITE_POLICY_ASYNC) ? "async" : "sync"));

  // The callees are responsible for thread-safety.
  stats->dataBlocksUsed     = getPhysicalBlocksAllocated(vdo);
  stats->overheadBlocksUsed = getPhysicalBlocksOverhead(vdo);
  stats->logicalBlocksUsed  = get_journal_logical_blocks_used(journal);
  stats->allocator          = get_depot_block_allocator_statistics(depot);
  stats->journal            = get_recovery_journal_statistics(journal);
  stats->packer             = get_packer_statistics(vdo->packer);
  stats->slabJournal        = get_depot_slab_journal_statistics(depot);
  stats->slabSummary        = get_slab_summary_statistics(get_slab_summary(depot));
  stats->refCounts          = get_depot_ref_counts_statistics(depot);
  stats->blockMap           = getBlockMapStatistics(vdo->blockMap);
  stats->hashLock           = getHashLockStatistics(vdo);
  stats->errors             = getVDOErrorStatistics(vdo);
  SlabCount slabTotal       = get_depot_slab_count(depot);
  stats->recoveryPercentage
    = (slabTotal - get_depot_unrecovered_slab_count(depot)) * 100 / slabTotal;

  VDOState state        = getVDOState(vdo);
  stats->inRecoveryMode = (state == VDO_RECOVERING);
  snprintf(stats->mode, sizeof(stats->mode), "%s", describeVDOState(state));
}

/**********************************************************************/
BlockCount getPhysicalBlocksAllocated(const struct vdo *vdo)
{
  return (get_depot_allocated_blocks(vdo->depot)
          - get_journal_block_map_data_blocks_used(vdo->recoveryJournal));
}

/**********************************************************************/
BlockCount getPhysicalBlocksFree(const struct vdo *vdo)
{
  return get_depot_free_blocks(vdo->depot);
}

/**********************************************************************/
BlockCount getPhysicalBlocksOverhead(const struct vdo *vdo)
{
  // XXX config.physicalBlocks is actually mutated during resize and is in a
  // packed structure, but resize runs on admin thread so we're usually OK.
  return (vdo->config.physicalBlocks
          - get_depot_data_blocks(vdo->depot)
          + get_journal_block_map_data_blocks_used(vdo->recoveryJournal));
}

/**********************************************************************/
BlockCount getTotalBlockMapBlocks(const struct vdo *vdo)
{
  return (getNumberOfFixedBlockMapPages(vdo->blockMap)
          + get_journal_block_map_data_blocks_used(vdo->recoveryJournal));
}

/**********************************************************************/
WritePolicy getWritePolicy(const struct vdo *vdo)
{
  return vdo->loadConfig.writePolicy;
}

/**********************************************************************/
void setWritePolicy(struct vdo *vdo, WritePolicy new)
{
  vdo->loadConfig.writePolicy = new;
}

/**********************************************************************/
const VDOLoadConfig *getVDOLoadConfig(const struct vdo *vdo)
{
  return &vdo->loadConfig;
}

/**********************************************************************/
const ThreadConfig *getThreadConfig(const struct vdo *vdo)
{
  return vdo->loadConfig.threadConfig;
}

/**********************************************************************/
BlockCount getConfiguredBlockMapMaximumAge(const struct vdo *vdo)
{
  return vdo->loadConfig.maximumAge;
}

/**********************************************************************/
PageCount getConfiguredCacheSize(const struct vdo *vdo)
{
  return vdo->loadConfig.cacheSize;
}

/**********************************************************************/
PhysicalBlockNumber getFirstBlockOffset(const struct vdo *vdo)
{
  return vdo->loadConfig.firstBlockOffset;
}

/**********************************************************************/
struct block_map *getBlockMap(const struct vdo *vdo)
{
  return vdo->blockMap;
}

/**********************************************************************/
struct slab_depot *getSlabDepot(struct vdo *vdo)
{
  return vdo->depot;
}

/**********************************************************************/
struct recovery_journal *getRecoveryJournal(struct vdo *vdo)
{
  return vdo->recoveryJournal;
}

/**********************************************************************/
void dumpVDOStatus(const struct vdo *vdo)
{
  dump_flusher(vdo->flusher);
  dump_recovery_journal_statistics(vdo->recoveryJournal);
  dump_packer(vdo->packer);
  dump_slab_depot(vdo->depot);

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  ZoneCount zone;
  for (zone = 0; zone < threadConfig->logicalZoneCount; zone++) {
    dump_logical_zone(get_logical_zone(vdo->logicalZones, zone));
  }

  for (zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
    dump_physical_zone(vdo->physicalZones[zone]);
  }

  for (zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    dump_hash_zone(vdo->hashZones[zone]);
  }
}

/**********************************************************************/
void setVDOTracingFlags(struct vdo *vdo, bool vioTracing)
{
  vdo->vioTraceRecording = vioTracing;
}

/**********************************************************************/
bool vdoVIOTracingEnabled(const struct vdo *vdo)
{
  return ((vdo != NULL) && vdo->vioTraceRecording);
}

/**********************************************************************/
void assertOnAdminThread(struct vdo *vdo, const char *name)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == getAdminThread(getThreadConfig(vdo))),
                  "%s called on admin thread", name);
}

/**********************************************************************/
void assertOnLogicalZoneThread(const struct vdo *vdo,
                               ZoneCount         logicalZone,
                               const char       *name)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == getLogicalZoneThread(getThreadConfig(vdo), logicalZone)),
                  "%s called on logical thread", name);
}

/**********************************************************************/
void assertOnPhysicalZoneThread(const struct vdo *vdo,
                                ZoneCount         physicalZone,
                                const char       *name)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == getPhysicalZoneThread(getThreadConfig(vdo),
                                            physicalZone)),
                  "%s called on physical thread", name);
}

/**********************************************************************/
struct hash_zone *selectHashZone(const struct vdo   *vdo,
                                 const UdsChunkName *name)
{
  /*
   * Use a fragment of the chunk name as a hash code. To ensure uniform
   * distributions, it must not overlap with fragments used elsewhere. Eight
   * bits of hash should suffice since the number of hash zones is small.
   */
  // XXX Make a central repository for these offsets ala hashUtils.
  // XXX Verify that the first byte is independent enough.
  uint32_t hash = name->name[0];

  /*
   * Scale the 8-bit hash fragment to a zone index by treating it as a binary
   * fraction and multiplying that by the zone count. If the hash is uniformly
   * distributed over [0 .. 2^8-1], then (hash * count / 2^8) should be
   * uniformly distributed over [0 .. count-1]. The multiply and shift is much
   * faster than a divide (modulus) on X86 CPUs.
   */
  return vdo->hashZones[(hash * getThreadConfig(vdo)->hashZoneCount) >> 8];
}

/**********************************************************************/
int getPhysicalZone(const struct vdo      *vdo,
                    PhysicalBlockNumber    pbn,
                    struct physical_zone **zonePtr)
{
  if (pbn == ZERO_BLOCK) {
    *zonePtr = NULL;
    return VDO_SUCCESS;
  }

  // Used because it does a more restrictive bounds check than get_slab(), and
  // done first because it won't trigger read-only mode on an invalid PBN.
  if (!is_physical_data_block(vdo->depot, pbn)) {
    return VDO_OUT_OF_RANGE;
  }

  // With the PBN already checked, we should always succeed in finding a slab.
  struct vdo_slab *slab = get_slab(vdo->depot, pbn);
  int result = ASSERT(slab != NULL, "get_slab must succeed on all valid PBNs");
  if (result != VDO_SUCCESS) {
    return result;
  }

  *zonePtr = vdo->physicalZones[get_slab_zone_number(slab)];
  return VDO_SUCCESS;
}

/**********************************************************************/
struct zoned_pbn validateDedupeAdvice(struct vdo                 *vdo,
                                      const struct data_location *advice,
                                      LogicalBlockNumber          lbn)
{
  struct zoned_pbn noAdvice = { .pbn = ZERO_BLOCK };
  if (advice == NULL) {
    return noAdvice;
  }

  // Don't use advice that's clearly meaningless.
  if ((advice->state == MAPPING_STATE_UNMAPPED)
      || (advice->pbn == ZERO_BLOCK)) {
    logDebug("Invalid advice from deduplication server: pbn %llu, "
             "state %u. Giving up on deduplication of logical block %llu",
             advice->pbn, advice->state, lbn);
    atomicAdd64(&vdo->errorStats.invalidAdvicePBNCount, 1);
    return noAdvice;
  }

  struct physical_zone  *zone;
  int result = getPhysicalZone(vdo, advice->pbn, &zone);
  if ((result != VDO_SUCCESS) || (zone == NULL)) {
    logDebug("Invalid physical block number from deduplication server: %"
             PRIu64 ", giving up on deduplication of logical block %llu",
             advice->pbn, lbn);
    atomicAdd64(&vdo->errorStats.invalidAdvicePBNCount, 1);
    return noAdvice;
  }

  return (struct zoned_pbn) {
    .pbn   = advice->pbn,
    .state = advice->state,
    .zone  = zone,
  };
}
