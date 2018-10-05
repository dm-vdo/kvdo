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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdo.c#10 $
 */

/*
 * This file contains the main entry points for normal operations on a VDO as
 * well as functions for constructing and destroying VDO instances (in memory).
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
static const VersionNumber VDO_MASTER_VERSION_67_0 = {
  .majorVersion = 67,
  .minorVersion =  0,
};

/**
 * The current version for the data encoded in the super block. This must
 * be changed any time there is a change to encoding of the component data
 * of any VDO component.
 **/
static const VersionNumber VDO_COMPONENT_DATA_41_0 = {
  .majorVersion = 41,
  .minorVersion =  0,
};

/**
 * This is the structure that captures the VDO fields saved as a SuperBlock
 * component.
 **/
typedef struct {
  VDOState  state;
  uint64_t  completeRecoveries;
  uint64_t  readOnlyRecoveries;
  VDOConfig config;
  Nonce     nonce;
} __attribute__((packed)) VDOComponent41_0;

/**
 * Implements ReadOnlyModeQuery.
 **/
static bool isReadOnlyVDO(void *context)
{
  return getThreadData((VDO *) context)->isReadOnly;
}

/**
 * Implements ReadOnlyModeEnterer.
 **/
static void enterReadOnlyModeFromContext(void *context, int errorCode)
{
  makeVDOReadOnly((VDO *) context, errorCode, true);
}

/**********************************************************************/
int allocateVDO(PhysicalLayer *layer, VDO **vdoPtr)
{
  int result = registerStatusCodes();
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDO *vdo;
  result = ALLOCATE(1, VDO, __func__, &vdo);
  if (result != UDS_SUCCESS) {
    return result;
  }

  vdo->layer           = layer;
  vdo->readOnlyContext = (ReadOnlyModeContext) {
    .context           = vdo,
    .isReadOnly        = isReadOnlyVDO,
    .enterReadOnlyMode = enterReadOnlyModeFromContext,
  };

  if (layer->createEnqueueable != NULL) {
    result = makeAdminCompletion(layer, &vdo->adminCompletion);
    if (result != VDO_SUCCESS) {
      freeVDO(&vdo);
      return result;
    }
  }

  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeVDO(PhysicalLayer *layer, VDO **vdoPtr)
{
  VDO *vdo;
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
void destroyVDO(VDO *vdo)
{
  freeFlusher(&vdo->flusher);
  freePacker(&vdo->packer);
  freeRecoveryJournal(&vdo->recoveryJournal);
  freeSlabDepot(&vdo->depot);
  freeVDOLayout(&vdo->layout);
  freeSuperBlock(&vdo->superBlock);
  freeBlockMap(&vdo->blockMap);

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  if (vdo->hashZones != NULL) {
    for (ZoneCount zone = 0; zone < threadConfig->hashZoneCount; zone++) {
      freeHashZone(&vdo->hashZones[zone]);
    }
  }
  FREE(vdo->hashZones);
  vdo->hashZones = NULL;

  if (vdo->logicalZones != NULL) {
    for (ZoneCount zone = 0; zone < threadConfig->logicalZoneCount; zone++) {
      freeLogicalZone(&vdo->logicalZones[zone]);
    }
  }
  FREE(vdo->logicalZones);
  vdo->logicalZones = NULL;

  if (vdo->physicalZones != NULL) {
    for (ZoneCount zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
      freePhysicalZone(&vdo->physicalZones[zone]);
    }
  }
  FREE(vdo->physicalZones);
  vdo->physicalZones = NULL;

  if (threadConfig != NULL) {
    freeThreadDataArray(&vdo->threadData, threadConfig->baseThreadCount);
  }

  freeAdminCompletion(&vdo->adminCompletion);
  freeThreadConfig(&vdo->loadConfig.threadConfig);
}

/**********************************************************************/
void freeVDO(VDO **vdoPtr)
{
  if (*vdoPtr == NULL) {
    return;
  }

  destroyVDO(*vdoPtr);
  FREE(*vdoPtr);
  *vdoPtr = NULL;
}

/**********************************************************************/
size_t getComponentDataSize(VDO *vdo)
{
  return (sizeof(VersionNumber)
          + sizeof(VersionNumber)
          + sizeof(VDOComponent41_0)
          + getVDOLayoutEncodedSize(vdo->layout)
          + getRecoveryJournalEncodedSize()
          + getSlabDepotEncodedSize()
          + getBlockMapEncodedSize());
}

/**
 * Encode the VDO master version.
 *
 * @param buffer  The buffer in which to encode the version
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeMasterVersion(Buffer *buffer)
{
  return encodeVersionNumber(VDO_MASTER_VERSION_67_0, buffer);
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
 * Encode the component data for the VDO itself.
 *
 * @param vdo     The vdo to encode
 * @param buffer  The buffer in which to encode the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int encodeVDOComponent(const VDO *vdo, Buffer *buffer)
{
  int result = encodeVersionNumber(VDO_COMPONENT_DATA_41_0, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  size_t initialLength = contentLength(buffer);

  result = putUInt32LEIntoBuffer(buffer, vdo->state);
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
  return ASSERT(encodedSize == sizeof(VDOComponent41_0),
                "encoded VDO component size must match structure size");
}

/**********************************************************************/
static int encodeVDO(VDO *vdo)
{
  Buffer *buffer = getComponentBuffer(vdo->superBlock);
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

  result = encodeRecoveryJournal(vdo->recoveryJournal, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = encodeSlabDepot(vdo->depot, buffer);
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
int saveVDOComponents(VDO *vdo)
{
  int result = encodeVDO(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return saveSuperBlock(vdo->layer, vdo->superBlock, getFirstBlockOffset(vdo));
}

/**********************************************************************/
void saveVDOComponentsAsync(VDO *vdo, VDOCompletion *parent)
{
  int result = encodeVDO(vdo);
  if (result != VDO_SUCCESS) {
    finishCompletion(parent, result);
    return;
  }

  saveSuperBlockAsync(vdo->superBlock, getFirstBlockOffset(vdo), parent);
}

/**********************************************************************/
int saveReconfiguredVDO(VDO *vdo)
{
  Buffer *buffer         = getComponentBuffer(vdo->superBlock);
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

  return saveSuperBlock(vdo->layer, vdo->superBlock, getFirstBlockOffset(vdo));
}

/**********************************************************************/
int decodeVDOVersion(VDO *vdo)
{
  return decodeVersionNumber(getComponentBuffer(vdo->superBlock),
                             &vdo->loadVersion);
}

/**********************************************************************/
int validateVDOVersion(VDO *vdo)
{
  int result = decodeVDOVersion(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ReleaseVersionNumber loadedReleaseVersion
    = getLoadedReleaseVersion(vdo->superBlock);
  if (vdo->loadConfig.releaseVersion != loadedReleaseVersion) {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "Geometry release version %" PRIu32 " does "
                                   "not match super block release version %"
                                   PRIu32,
                                   vdo->loadConfig.releaseVersion,
                                   loadedReleaseVersion);
  }

  return validateVersion(VDO_MASTER_VERSION_67_0, vdo->loadVersion, "master");
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
  int result = getUInt64LEFromBuffer(buffer, &config->logicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->physicalBlocks);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->slabSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &config->recoveryJournalSize);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return getUInt64LEFromBuffer(buffer, &config->slabJournalBlocks);
}

/**
 * Decode the version 41.0 component state for the VDO itself from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param state   The state structure to receive the decoded values
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
  static int decodeVDOComponent_41_0(Buffer *buffer, VDOComponent41_0 *state)
{
  size_t initialLength = contentLength(buffer);

  int result = getUInt32LEFromBuffer(buffer, &state->state);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->completeRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->readOnlyRecoveries);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVDOConfig(buffer, &state->config);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = getUInt64LEFromBuffer(buffer, &state->nonce);
  if (result != VDO_SUCCESS) {
    return result;
  }

  size_t decodedSize = initialLength - contentLength(buffer);
  return ASSERT(decodedSize == sizeof(VDOComponent41_0),
                "decoded VDO component size must match structure size");
}

/**********************************************************************/
int decodeVDOComponent(VDO *vdo)
{
  Buffer *buffer = getComponentBuffer(vdo->superBlock);

  VersionNumber version;
  int result = decodeVersionNumber(buffer, &version);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = validateVersion(version, VDO_COMPONENT_DATA_41_0,
                           "VDO component data");
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDOComponent41_0 component;
  result = decodeVDOComponent_41_0(buffer, &component);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Copy the decoded component into the VDO structure.
  vdo->state              = component.state;
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
  result = configureSlab(config->slabSize, config->slabJournalBlocks,
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
                  "physical block count %" PRIu64 " exceeds maximum %" PRIu64,
                  config->physicalBlocks, MAXIMUM_PHYSICAL_BLOCKS);
  if (result != UDS_SUCCESS) {
    return VDO_OUT_OF_RANGE;
  }

  // This can't check equality because FileLayer et al can only known about
  // the storage size, which may not match the super block size.
  result = ASSERT(config->physicalBlocks <= blockCount,
                  "Physical size %" PRIu64 " in super block smaller than"
                  " expected size %" PRIu64, config->physicalBlocks,
                  blockCount);
  if (result != UDS_SUCCESS) {
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

/**********************************************************************/
ThreadData *getThreadData(const VDO *vdo)
{
  return &vdo->threadData[getCallbackThreadID()];
}

/**********************************************************************/
bool inReadOnlyMode(const VDO *vdo)
{
  return (vdo->state == VDO_READ_ONLY_MODE);
}

/**********************************************************************/
bool isClean(const VDO *vdo)
{
  return ((vdo->state == VDO_CLEAN) || (vdo->state == VDO_NEW));
}

/**********************************************************************/
bool wasClean(const VDO *vdo)
{
  return ((vdo->loadState == VDO_CLEAN) || (vdo->loadState == VDO_NEW));
}

/**********************************************************************/
bool wasNew(const VDO *vdo)
{
  return (vdo->loadState == VDO_NEW);
}

/**********************************************************************/
bool requiresReadOnlyRebuild(const VDO *vdo)
{
  return ((vdo->loadState == VDO_FORCE_REBUILD)
          || (vdo->loadState == VDO_REBUILD_FOR_UPGRADE));
}

/**********************************************************************/
bool requiresRebuild(const VDO *vdo)
{
  return ((vdo->state == VDO_DIRTY)
          || (vdo->state == VDO_FORCE_REBUILD)
          || (vdo->state == VDO_REPLAYING)
          || (vdo->state == VDO_REBUILD_FOR_UPGRADE));
}

/**********************************************************************/
bool requiresRecovery(const VDO *vdo)
{
  return ((vdo->loadState == VDO_DIRTY) || (vdo->loadState == VDO_REPLAYING)
          || (vdo->loadState == VDO_RECOVERING));
}

/**********************************************************************/
bool isReplaying(const VDO *vdo)
{
  return (vdo->state == VDO_REPLAYING);
}

/**********************************************************************/
bool inRecoveryMode(const VDO *vdo)
{
  return (vdo->state == VDO_RECOVERING);
}

/**********************************************************************/
void enterRecoveryMode(VDO *vdo)
{
  assertOnAdminThread(vdo, __func__);

  if (isReadOnlyVDO(vdo)) {
    return;
  }

  logInfo("Entering recovery mode");
  vdo->state = VDO_RECOVERING;
}

/**********************************************************************/
void leaveRecoveryMode(VDO *vdo)
{
  assertOnAdminThread(vdo, __func__);

  /*
   * Since scrubbing can be stopped by vdoClose during recovery mode,
   * do not change the VDO state if there are outstanding unrecovered slabs.
   */
  if (vdo->closeRequested || isReadOnlyVDO(vdo)) {
    return;
  }

  ASSERT_LOG_ONLY(inRecoveryMode(vdo), "VDO is in recovery mode");
  logInfo("Exiting recovery mode");
  vdo->state = VDO_DIRTY;
}

/**********************************************************************/
bool setVDOCompressing(VDO *vdo, bool enableCompression)
{
  bool stateChanged = compareAndSwapBool(&vdo->compressing, !enableCompression,
                                         enableCompression);
  if (stateChanged && !enableCompression) {
    // Flushing the packer is asynchronous, but we don't care when it
    // finishes.
    flushPacker(vdo->packer);
  }

  logInfo("compression is %s", (enableCompression ? "enabled" : "disabled"));
  return (stateChanged ? !enableCompression : enableCompression);
}

/**********************************************************************/
bool getVDOCompressing(VDO *vdo)
{
  return atomicLoadBool(&vdo->compressing);
}

/**********************************************************************/
static size_t getBlockMapCacheSize(const VDO *vdo)
{
  return ((size_t) vdo->loadConfig.cacheSize) * VDO_BLOCK_SIZE;
}

/**
 * Return a user-visible string describing the current VDO state.
 *
 * @param state  The VDO state to describe
 *
 * @return A string constant describing the state
 **/
static const char *describeVDOState(VDOState state)
{
  // These strings should all fit in the 15 chars of VDOStatistics.mode.
  switch (state) {
  case VDO_RECOVERING:
    return "recovering";

  case VDO_READ_ONLY_MODE:
    return "read-only";

  default:
    return "normal";
  }
}

/**
 * Tally the hash lock statistics from all the hash zones.
 *
 * @param vdo  The vdo to query
 *
 * @return The sum of the hash lock statistics from all hash zones
 **/
static HashLockStatistics getHashLockStatistics(const VDO *vdo)
{
  HashLockStatistics totals;
  memset(&totals, 0, sizeof(totals));

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  for (ZoneCount zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    HashLockStatistics stats  = getHashZoneStatistics(vdo->hashZones[zone]);
    totals.dedupeAdviceValid        += stats.dedupeAdviceValid;
    totals.dedupeAdviceStale        += stats.dedupeAdviceStale;
    totals.concurrentDataMatches    += stats.concurrentDataMatches;
    totals.concurrentHashCollisions += stats.concurrentHashCollisions;
  }

  return totals;
}

/**
 * Get the current error statistics from VDO.
 *
 * @param vdo  The vdo to query
 *
 * @return a copy of the current VDO error counters
 **/
static ErrorStatistics getVDOErrorStatistics(const VDO *vdo)
{
  /*
   * The error counts can be incremented from arbitrary threads and so must be
   * incremented atomically, but they are just statistics with no semantics
   * that could rely on memory order, so unfenced reads are sufficient.
   */
  const AtomicErrorStatistics *atoms = &vdo->errorStats;
  return (ErrorStatistics) {
    .invalidAdvicePBNCount = relaxedLoad64(&atoms->invalidAdvicePBNCount),
    .noSpaceErrorCount     = relaxedLoad64(&atoms->noSpaceErrorCount),
    .readOnlyErrorCount    = relaxedLoad64(&atoms->readOnlyErrorCount),
  };
}

/**********************************************************************/
void getVDOStatistics(const VDO *vdo, VDOStatistics *stats)
{
  // These are immutable properties of the VDO object, so it is safe to
  // query them from any thread.
  RecoveryJournal *journal  = vdo->recoveryJournal;
  SlabDepot       *depot    = vdo->depot;
  // XXX config.physicalBlocks is actually mutated during resize and is in a
  // packed structure, but resize runs on the admin thread so we're usually OK.
  stats->version            = STATISTICS_VERSION;
  stats->releaseVersion     = CURRENT_RELEASE_VERSION_NUMBER;
  stats->logicalBlocks      = vdo->config.logicalBlocks;
  stats->physicalBlocks     = vdo->config.physicalBlocks;
  stats->blockSize          = VDO_BLOCK_SIZE;
  stats->completeRecoveries = vdo->completeRecoveries;
  stats->readOnlyRecoveries = vdo->readOnlyRecoveries;
  stats->blockMapCacheSize  = getBlockMapCacheSize(vdo);
  snprintf(stats->writePolicy, sizeof(stats->writePolicy), "%s",
           ((getWritePolicy(vdo) == WRITE_POLICY_ASYNC) ? "async" : "sync"));

  // The callees are responsible for thread-safety.
  stats->dataBlocksUsed     = getPhysicalBlocksAllocated(vdo);
  stats->overheadBlocksUsed = getPhysicalBlocksOverhead(vdo);
  stats->logicalBlocksUsed  = getJournalLogicalBlocksUsed(journal);
  stats->allocator          = getDepotBlockAllocatorStatistics(depot);
  stats->journal            = getRecoveryJournalStatistics(journal);
  stats->packer             = getPackerStatistics(vdo->packer);
  stats->slabJournal        = getDepotSlabJournalStatistics(depot);
  stats->slabSummary        = getSlabSummaryStatistics(getSlabSummary(depot));
  stats->refCounts          = getDepotRefCountsStatistics(depot);
  stats->blockMap           = getBlockMapStatistics(vdo->blockMap);
  stats->hashLock           = getHashLockStatistics(vdo);
  stats->errors             = getVDOErrorStatistics(vdo);
  SlabCount slabTotal       = getDepotSlabCount(depot);
  stats->recoveryPercentage
    = (slabTotal - getDepotUnrecoveredSlabCount(depot)) * 100 / slabTotal;

  // The "state" field is mutable, but we just need a unfenced atomic read.
  VDOState state        = *((const volatile VDOState *) &vdo->state);
  stats->inRecoveryMode = (state == VDO_RECOVERING);
  snprintf(stats->mode, sizeof(stats->mode), "%s", describeVDOState(state));
}

/**********************************************************************/
BlockCount getPhysicalBlocksAllocated(const VDO *vdo)
{
  return (getDepotAllocatedBlocks(vdo->depot)
          - getJournalBlockMapDataBlocksUsed(vdo->recoveryJournal));
}

/**********************************************************************/
BlockCount getPhysicalBlocksFree(const VDO *vdo)
{
  return getDepotFreeBlocks(vdo->depot);
}

/**********************************************************************/
BlockCount getPhysicalBlocksOverhead(const VDO *vdo)
{
  // XXX config.physicalBlocks is actually mutated during resize and is in a
  // packed structure, but resize runs on admin thread so we're usually OK.
  return (vdo->config.physicalBlocks
          - getDepotDataBlocks(vdo->depot)
          + getJournalBlockMapDataBlocksUsed(vdo->recoveryJournal));
}

/**********************************************************************/
BlockCount getTotalBlockMapBlocks(const VDO *vdo)
{
  return (getNumberOfFixedBlockMapPages(vdo->blockMap)
          + getJournalBlockMapDataBlocksUsed(vdo->recoveryJournal));
}

/**********************************************************************/
WritePolicy getWritePolicy(const VDO *vdo)
{
  return vdo->loadConfig.writePolicy;
}

/**********************************************************************/
void setWritePolicy(VDO *vdo, WritePolicy new)
{
  vdo->loadConfig.writePolicy = new;
}

/**********************************************************************/
const VDOLoadConfig *getVDOLoadConfig(const VDO *vdo)
{
  return &vdo->loadConfig;
}

/**********************************************************************/
const ThreadConfig *getThreadConfig(const VDO *vdo)
{
  return vdo->loadConfig.threadConfig;
}

/**********************************************************************/
BlockCount getConfiguredBlockMapMaximumAge(const VDO *vdo)
{
  return vdo->loadConfig.maximumAge;
}

/**********************************************************************/
PageCount getConfiguredCacheSize(const VDO *vdo)
{
  return vdo->loadConfig.cacheSize;
}

/**********************************************************************/
PhysicalBlockNumber getFirstBlockOffset(const VDO *vdo)
{
  return vdo->loadConfig.firstBlockOffset;
}

/**********************************************************************/
BlockMap *getBlockMap(const VDO *vdo)
{
  return vdo->blockMap;
}

/**********************************************************************/
SlabDepot *getSlabDepot(VDO *vdo)
{
  return vdo->depot;
}

/**********************************************************************/
RecoveryJournal *getRecoveryJournal(VDO *vdo)
{
  return vdo->recoveryJournal;
}

/**********************************************************************/
void dumpVDOStatus(const VDO *vdo)
{
  dumpFlusher(vdo->flusher);
  dumpRecoveryJournalStatistics(vdo->recoveryJournal);
  dumpPacker(vdo->packer);
  dumpSlabDepot(vdo->depot);

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  for (ZoneCount zone = 0; zone < threadConfig->logicalZoneCount; zone++) {
    dumpLogicalZone(vdo->logicalZones[zone]);
  }

  for (ZoneCount zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
    dumpPhysicalZone(vdo->physicalZones[zone]);
  }

  for (ZoneCount zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    dumpHashZone(vdo->hashZones[zone]);
  }
}

/**********************************************************************/
void setVDOTracingFlags(VDO *vdo, bool vioTracing)
{
  vdo->vioTraceRecording = vioTracing;
}

/**********************************************************************/
bool vdoVIOTracingEnabled(const VDO *vdo)
{
  return ((vdo != NULL) && vdo->vioTraceRecording);
}

/**********************************************************************/
void assertOnAdminThread(VDO *vdo, const char *name)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == getAdminThread(getThreadConfig(vdo))),
                  "%s called on admin thread", name);
}

/**********************************************************************/
void assertOnLogicalZoneThread(const VDO  *vdo,
                               ZoneCount   logicalZone,
                               const char *name)
{
  ASSERT_LOG_ONLY((getCallbackThreadID()
                   == getLogicalZoneThread(getThreadConfig(vdo), logicalZone)),
                  "%s called on logical thread", name);
}

/**********************************************************************/
HashZone *selectHashZone(const VDO *vdo, const UdsChunkName *name)
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
int getPhysicalZone(const VDO            *vdo,
                    PhysicalBlockNumber   pbn,
                    PhysicalZone        **zonePtr)
{
  if (pbn == ZERO_BLOCK) {
    *zonePtr = NULL;
    return VDO_SUCCESS;
  }

  // Used because it does a more restrictive bounds check than getSlab(), and
  // done first because it won't trigger read-only mode on an invalid PBN.
  if (!isPhysicalDataBlock(vdo->depot, pbn)) {
    return VDO_OUT_OF_RANGE;
  }

  // With the PBN already checked, we should always succeed in finding a slab.
  Slab *slab = getSlab(vdo->depot, pbn);
  int result = ASSERT(slab != NULL, "getSlab must succeed on all valid PBNs");
  if (result != VDO_SUCCESS) {
    return result;
  }

  *zonePtr = vdo->physicalZones[getSlabZoneNumber(slab)];
  return VDO_SUCCESS;
}

/**********************************************************************/
ZonedPBN validateDedupeAdvice(VDO                *vdo,
                              const DataLocation *advice,
                              LogicalBlockNumber  lbn)
{
  ZonedPBN noAdvice = { .pbn = ZERO_BLOCK };
  if (advice == NULL) {
    return noAdvice;
  }

  // Don't use advice that's clearly meaningless.
  if ((advice->state == MAPPING_STATE_UNMAPPED)
      || (advice->pbn == ZERO_BLOCK)) {
    logDebug("Invalid advice from deduplication server: pbn %" PRIu64 ", "
             "state %u. Giving up on deduplication of logical block %" PRIu64,
             advice->pbn, advice->state, lbn);
    atomicAdd64(&vdo->errorStats.invalidAdvicePBNCount, 1);
    return noAdvice;
  }

  PhysicalZone  *zone;
  int result = getPhysicalZone(vdo, advice->pbn, &zone);
  if ((result != VDO_SUCCESS) || (zone == NULL)) {
    logDebug("Invalid physical block number from deduplication server: %"
             PRIu64 ", giving up on deduplication of logical block %" PRIu64,
             advice->pbn, lbn);
    atomicAdd64(&vdo->errorStats.invalidAdvicePBNCount, 1);
    return noAdvice;
  }

  return (ZonedPBN) {
    .pbn   = advice->pbn,
    .state = advice->state,
    .zone  = zone,
  };
}
