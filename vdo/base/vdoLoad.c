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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoLoad.c#6 $
 */

#include "vdoLoad.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "hashZone.h"
#include "header.h"
#include "logicalZone.h"
#include "physicalZone.h"
#include "readOnlyRebuild.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "types.h"
#include "vdoInternal.h"
#include "vdoRecovery.h"
#include "volumeGeometry.h"

/**
 * Extract the VDO from an AdminCompletion, checking that the current operation
 * is a load.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The VDO
 **/
static inline VDO *vdoFromLoadSubTask(VDOCompletion *completion)
{
  return vdoFromAdminSubTask(completion, ADMIN_OPERATION_LOAD);
}

/**
 * Finish aborting a load now that any entry to read-only mode is complete.
 * This callback is registered in abortLoad().
 *
 * @param completion  The sub-task completion
 **/
static void finishAborting(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  vdo->closeRequired = false;
  finishParentCallback(completion);
}

/**
 * Make sure the recovery journal is closed when aborting a load.
 *
 * @param completion  The sub-task completion
 **/
static void closeRecoveryJournalForAbort(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  prepareAdminSubTask(vdo, finishAborting, finishAborting);
  closeRecoveryJournal(vdo->recoveryJournal, completion);
}

/**
 * Clean up after an error loading a VDO. This error handler is set in
 * loadCallback() and loadVDOComponents().
 *
 * @param completion  The sub-task completion
 **/
static void abortLoad(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  logErrorWithStringError(completion->result, "aborting load");
  if (vdo->threadData == NULL) {
    // There are no threads, so we're done
    finishParentCallback(completion);
    return;
  }

  // Preserve the error.
  setCompletionResult(completion->parent, completion->result);
  if (vdo->recoveryJournal == NULL) {
    prepareAdminSubTask(vdo, finishAborting, finishAborting);
  } else {
    prepareAdminSubTaskOnThread(vdo, closeRecoveryJournalForAbort,
                                closeRecoveryJournalForAbort,
                                getJournalZoneThread(getThreadConfig(vdo)));
  }

  waitUntilNotEnteringReadOnlyMode(vdo, completion);
}

/**
 * Wait for the VDO to be in read-only mode.
 *
 * @param completion  The sub-task completion
 **/
static void waitForReadOnlyMode(VDOCompletion *completion)
{
  prepareToFinishParent(completion, completion->parent);
  setCompletionResult(completion, VDO_READ_ONLY);
  waitUntilNotEnteringReadOnlyMode(vdoFromLoadSubTask(completion), completion);
}

/**
 * Finish loading the VDO after an error, but leave it in read-only
 * mode.  This error handler is set in makeDirty(), scrubSlabs(), and
 * loadVDOComponents().
 *
 * @param completion  The sub-task completion
 **/
static void continueLoadReadOnly(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  logErrorWithStringError(completion->result,
                          "Entering read-only mode due to load error");
  makeVDOReadOnly(vdo, completion->result, true);
  waitForReadOnlyMode(completion);
}

/**
 * Exit recovery mode if necessary now that online slab scrubbing or loading
 * is complete. This callback is registrered in scrubSlabs().
 *
 * @param completion  The slab scrubber completion
 **/
static void finishScrubbingSlabs(VDOCompletion *completion)
{
  VDO *vdo = completion->parent;
  assertOnAdminThread(vdo, __func__);
  if (inRecoveryMode(vdo)) {
    leaveRecoveryMode(vdo);
  } else {
    logInfo("VDO commencing normal operation");
  }
}

/**
 * Handle an error scrubbing or loading all slabs after the VDO has come
 * online. This error handler is registered in scrubSlabs().
 *
 * @param completion  The slab scrubber completion
 **/
static void handleScrubAllError(VDOCompletion *completion)
{
  VDO *vdo = completion->parent;
  enterReadOnlyMode(&vdo->readOnlyContext, completion->result);
}

/**
 * Initiate slab scrubbing if necessary. This callback is registered in
 * prepareToComeOnline().
 *
 * @param completion   The sub-task completion
 **/
static void scrubSlabs(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  if (!hasUnrecoveredSlabs(vdo->depot)) {
    finishParentCallback(completion);
    return;
  }

  if (requiresRecovery(vdo)) {
    enterRecoveryMode(vdo);
  }

  prepareAdminSubTask(vdo, finishParentCallback, continueLoadReadOnly);
  scrubAllUnrecoveredSlabs(vdo->depot, vdo, finishScrubbingSlabs,
                           handleScrubAllError, 0, completion);
}

/**
 * This is the error handler for slab scrubbing. It is registered in
 * prepareToComeOnline().
 *
 * @param completion  The sub-task completion
 **/
static void handleScrubbingError(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  enterReadOnlyMode(&vdo->readOnlyContext, completion->result);
  waitForReadOnlyMode(completion);
}

/**
 * This is the callback after the super block is written. It prepares the block
 * allocator to come online and start allocating. It is registered in
 * makeDirty().
 *
 * @param completion  The sub-task completion
 **/
static void prepareToComeOnline(VDOCompletion *completion)
{
  VDO               *vdo      = vdoFromLoadSubTask(completion);
  SlabDepotLoadType  loadType = NORMAL_LOAD;
  if (requiresReadOnlyRebuild(vdo)) {
    loadType = NO_LOAD;
  } else if (requiresRecovery(vdo)) {
    loadType = DEFER_LOAD;
  }

  initializeBlockMapFromJournal(vdo->blockMap, vdo->recoveryJournal);

  prepareAdminSubTask(vdo, scrubSlabs, handleScrubbingError);
  prepareToAllocate(vdo->depot, loadType, completion);
}

/**
 * Mark the super block as dirty now that everything has been loaded or
 * rebuilt.
 *
 * @param completion  The sub-task completion
 **/
static void makeDirty(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  if (isReadOnly(&vdo->readOnlyContext)) {
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  vdo->state = VDO_DIRTY;
  prepareAdminSubTask(vdo, prepareToComeOnline, continueLoadReadOnly);
  saveVDOComponentsAsync(vdo, completion);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int startVDODecode(VDO *vdo, bool validateConfig)
{
  int result = validateVDOVersion(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVDOComponent(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (!validateConfig) {
    return VDO_SUCCESS;
  }

  if (vdo->loadConfig.nonce != vdo->nonce) {
    return logErrorWithStringError(VDO_BAD_NONCE, "Geometry nonce %" PRIu64
                                   " does not match superblock nonce %" PRIu64,
                                   vdo->loadConfig.nonce, vdo->nonce);
  }

  BlockCount blockCount = vdo->layer->getBlockCount(vdo->layer);
  return validateVDOConfig(&vdo->config, blockCount, true);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int finishVDODecode(VDO *vdo)
{
  Buffer             *buffer       = getComponentBuffer(vdo->superBlock);
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  int result = makeRecoveryJournal(vdo->nonce, vdo->layer,
                                   getVDOPartition(vdo->layout,
                                                   RECOVERY_JOURNAL_PARTITION),
                                   vdo->completeRecoveries,
                                   vdo->config.recoveryJournalSize,
                                   RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
                                   &vdo->readOnlyContext, threadConfig,
                                   &vdo->recoveryJournal);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeRecoveryJournal(vdo->recoveryJournal, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeSlabDepot(buffer, threadConfig, vdo->nonce, vdo->layer,
                           getVDOPartition(vdo->layout,
                                           SLAB_SUMMARY_PARTITION),
                           &vdo->readOnlyContext, vdo->recoveryJournal,
                           &vdo->depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeBlockMap(buffer, vdo->config.logicalBlocks, threadConfig,
                          &vdo->blockMap);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ASSERT_LOG_ONLY((contentLength(buffer) == 0),
                  "All decoded component data was used");
  return VDO_SUCCESS;
}

/**
 * Decode the component data portion of a super block and fill in the
 * corresponding portions of the VDO being loaded. This will also allocate the
 * recovery journal and slab depot. If this method is called with an
 * asynchronous layer (i.e. a thread config which specifies at least one base
 * thread), the block map and packer will be constructed as well.
 *
 * @param vdo             The VDO being loaded
 * @param validateConfig  Whether to validate the config
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int decodeVDO(VDO *vdo, bool validateConfig)
{
  int result = startVDODecode(vdo, validateConfig);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVDOLayout(getComponentBuffer(vdo->superBlock), &vdo->layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  result = makeThreadDataArray((vdo->state == VDO_READ_ONLY_MODE),
                               threadConfig, vdo->layer, &vdo->threadData);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = finishVDODecode(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = makeFlusher(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount maximumAge = getConfiguredBlockMapMaximumAge(vdo);
  BlockCount journalLength
    = getRecoveryJournalLength(vdo->config.recoveryJournalSize);
  if ((maximumAge > (journalLength / 2)) || (maximumAge < 1)) {
    return VDO_BAD_CONFIGURATION;
  }
  result = makeBlockMapCaches(vdo->blockMap, vdo->layer,
                              &vdo->readOnlyContext, vdo->recoveryJournal,
                              vdo->nonce, getConfiguredCacheSize(vdo),
                              maximumAge);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Prepare the recovery journal for new entries.
  openRecoveryJournal(vdo->recoveryJournal, vdo->depot, vdo->blockMap);

  result = ALLOCATE(threadConfig->hashZoneCount, HashZone *, __func__,
                    &vdo->hashZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (ZoneCount zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    result = makeHashZone(vdo, zone, &vdo->hashZones[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  result = ALLOCATE(threadConfig->logicalZoneCount, LogicalZone *, __func__,
                    &vdo->logicalZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  // Allocate in reverse zone number order so we can pass each logical zone's
  // successor to the zone's constructor.
  LogicalZone *logicalZone = NULL;
  for (int index = threadConfig->logicalZoneCount - 1; index >= 0; index--) {
    int result = makeLogicalZone(vdo, index, logicalZone, &logicalZone);
    if (result != VDO_SUCCESS) {
      return result;
    }
    vdo->logicalZones[index] = logicalZone;
  }

  result = ALLOCATE(threadConfig->physicalZoneCount, PhysicalZone *, __func__,
                    &vdo->physicalZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (ZoneCount zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
    result = makePhysicalZone(vdo, zone, &vdo->physicalZones[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return makePacker(vdo->layer, DEFAULT_PACKER_INPUT_BINS,
                    DEFAULT_PACKER_OUTPUT_BINS, threadConfig, &vdo->packer);
}

/**
 * Load the components of a VDO. This is the super block load callback
 * set by loadCallback().
 *
 * @param completion The sub-task completion
 **/
static void loadVDOComponents(VDOCompletion *completion)
{
  VDO *vdo    = vdoFromLoadSubTask(completion);
  int  result = decodeVDO(vdo, true);
  if (result != VDO_SUCCESS) {
    resetCompletion(completion);
    finishCompletion(completion, result);
    return;
  }

  vdo->closeRequired = true;
  if (isReadOnly(&vdo->readOnlyContext)) {
    // In read-only mode we don't use the allocator and it may not
    // even be readable, so use the default structure.
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  if (requiresRebuild(vdo)) {
    if (requiresReadOnlyRebuild(vdo)) {
      prepareAdminSubTask(vdo, makeDirty, abortLoad);
      launchRebuild(vdo, completion);
    } else {
      prepareAdminSubTask(vdo, makeDirty, continueLoadReadOnly);
      launchRecovery(vdo, completion);
    }
    return;
  }

  prepareAdminSubTask(vdo, makeDirty, continueLoadReadOnly);
  loadSlabDepot(vdo->depot, wasNew(vdo), completion);
}

/**
 * Callback to initiate a load, registered in performVDOLoad().
 *
 * @param completion  The sub-task completion
 **/
static void loadCallback(VDOCompletion *completion)
{
  VDO *vdo = vdoFromLoadSubTask(completion);
  assertOnAdminThread(vdo, __func__);
  prepareAdminSubTask(vdo, loadVDOComponents, abortLoad);
  loadSuperBlockAsync(completion, getFirstBlockOffset(vdo), &vdo->superBlock);
}

/**********************************************************************/
int performVDOLoad(VDO *vdo, const VDOLoadConfig *loadConfig)
{
  vdo->loadConfig = *loadConfig;
  return performAdminOperation(vdo, ADMIN_OPERATION_LOAD, loadCallback);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeSynchronousVDO(VDO *vdo, bool validateConfig)
{
  int result = startVDODecode(vdo, validateConfig);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVDOLayout(getComponentBuffer(vdo->superBlock), &vdo->layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return finishVDODecode(vdo);
}

/**********************************************************************/
int loadVDOSuperblock(PhysicalLayer   *layer,
                      VolumeGeometry  *geometry,
                      bool             validateConfig,
                      VDODecoder      *decoder,
                      VDO            **vdoPtr)
{
  VDO *vdo;
  int result = makeVDO(layer, &vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  setLoadConfigFromGeometry(geometry, &vdo->loadConfig);
  result = loadSuperBlock(layer, getFirstBlockOffset(vdo), &vdo->superBlock);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  result = ((decoder == NULL)
            ? decodeSynchronousVDO(vdo, validateConfig)
            : decoder(vdo, validateConfig));
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  *vdoPtr = vdo;
  return VDO_SUCCESS;

}
/**********************************************************************/
int loadVDO(PhysicalLayer  *layer,
            bool            validateConfig,
            VDODecoder     *decoder,
            VDO           **vdoPtr)
{
  VolumeGeometry geometry;
  int result = loadVolumeGeometry(layer, &geometry);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return loadVDOSuperblock(layer, &geometry, validateConfig, decoder, vdoPtr);
}
