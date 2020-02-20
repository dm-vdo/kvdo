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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoLoad.c#28 $
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
 * Extract the vdo from an AdminCompletion, checking that the current operation
 * is a load.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The vdo
 **/
static inline struct vdo *vdoFromLoadSubTask(struct vdo_completion *completion)
{
  return vdo_from_admin_sub_task(completion, ADMIN_OPERATION_LOAD);
}

/**
 * Finish aborting a load now that any entry to read-only mode is complete.
 * This callback is registered in abortLoad().
 *
 * @param completion  The sub-task completion
 **/
static void finishAborting(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  vdo->closeRequired = false;
  finishParentCallback(completion);
}

/**
 * Make sure the recovery journal is closed when aborting a load.
 *
 * @param completion  The sub-task completion
 **/
static void closeRecoveryJournalForAbort(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  prepare_admin_sub_task(vdo, finishAborting, finishAborting);
  drain_recovery_journal(vdo->recoveryJournal, ADMIN_STATE_SAVING, completion);
}

/**
 * Clean up after an error loading a VDO. This error handler is set in
 * loadCallback() and loadVDOComponents().
 *
 * @param completion  The sub-task completion
 **/
static void abortLoad(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  logErrorWithStringError(completion->result, "aborting load");
  if (vdo->readOnlyNotifier == NULL) {
    // There are no threads, so we're done
    finishParentCallback(completion);
    return;
  }

  // Preserve the error.
  setCompletionResult(completion->parent, completion->result);
  if (vdo->recoveryJournal == NULL) {
    prepare_admin_sub_task(vdo, finishAborting, finishAborting);
  } else {
    prepare_admin_sub_task_on_thread(vdo, closeRecoveryJournalForAbort,
                                     closeRecoveryJournalForAbort,
                                     getJournalZoneThread(getThreadConfig(vdo)));
  }

  wait_until_not_entering_read_only_mode(vdo->readOnlyNotifier, completion);
}

/**
 * Wait for the VDO to be in read-only mode.
 *
 * @param completion  The sub-task completion
 **/
static void waitForReadOnlyMode(struct vdo_completion *completion)
{
  prepareToFinishParent(completion, completion->parent);
  setCompletionResult(completion, VDO_READ_ONLY);
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  wait_until_not_entering_read_only_mode(vdo->readOnlyNotifier, completion);
}

/**
 * Finish loading the VDO after an error, but leave it in read-only
 * mode.  This error handler is set in makeDirty(), scrubSlabs(), and
 * loadVDOComponents().
 *
 * @param completion  The sub-task completion
 **/
static void continueLoadReadOnly(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  logErrorWithStringError(completion->result,
                          "Entering read-only mode due to load error");
  enter_read_only_mode(vdo->readOnlyNotifier, completion->result);
  waitForReadOnlyMode(completion);
}

/**
 * Initiate slab scrubbing if necessary. This callback is registered in
 * prepareToComeOnline().
 *
 * @param completion   The sub-task completion
 **/
static void scrubSlabs(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  if (!has_unrecovered_slabs(vdo->depot)) {
    finishParentCallback(completion);
    return;
  }

  if (requiresRecovery(vdo)) {
    enterRecoveryMode(vdo);
  }

  prepare_admin_sub_task(vdo, finishParentCallback, continueLoadReadOnly);
  scrub_all_unrecovered_slabs(vdo->depot, completion);
}

/**
 * This is the error handler for slab scrubbing. It is registered in
 * prepareToComeOnline().
 *
 * @param completion  The sub-task completion
 **/
static void handleScrubbingError(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  enter_read_only_mode(vdo->readOnlyNotifier, completion->result);
  waitForReadOnlyMode(completion);
}

/**
 * This is the callback after the super block is written. It prepares the block
 * allocator to come online and start allocating. It is registered in
 * makeDirty().
 *
 * @param completion  The sub-task completion
 **/
static void prepareToComeOnline(struct vdo_completion *completion)
{
  struct vdo           *vdo      = vdoFromLoadSubTask(completion);
  slab_depot_load_type  loadType = NORMAL_LOAD;
  if (requiresReadOnlyRebuild(vdo)) {
    loadType = REBUILD_LOAD;
  } else if (requiresRecovery(vdo)) {
    loadType = RECOVERY_LOAD;
  }

  initializeBlockMapFromJournal(vdo->blockMap, vdo->recoveryJournal);

  prepare_admin_sub_task(vdo, scrubSlabs, handleScrubbingError);
  prepare_to_allocate(vdo->depot, loadType, completion);
}

/**
 * Mark the super block as dirty now that everything has been loaded or
 * rebuilt.
 *
 * @param completion  The sub-task completion
 **/
static void makeDirty(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  if (is_read_only(vdo->readOnlyNotifier)) {
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  setVDOState(vdo, VDO_DIRTY);
  prepare_admin_sub_task(vdo, prepareToComeOnline, continueLoadReadOnly);
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Callback to do the destructive parts of a load now that the new VDO device
 * is being resumed.
 *
 * @param completion  The sub-task completion
 **/
static void loadCallback(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  assertOnAdminThread(vdo, __func__);

  // Prepare the recovery journal for new entries.
  open_recovery_journal(vdo->recoveryJournal, vdo->depot, vdo->blockMap);
  vdo->closeRequired = true;
  if (is_read_only(vdo->readOnlyNotifier)) {
    // In read-only mode we don't use the allocator and it may not
    // even be readable, so use the default structure.
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  if (requiresReadOnlyRebuild(vdo)) {
    prepare_admin_sub_task(vdo, makeDirty, abortLoad);
    launch_rebuild(vdo, completion);
    return;
  }

  if (requiresRebuild(vdo)) {
    prepare_admin_sub_task(vdo, makeDirty, continueLoadReadOnly);
    launchRecovery(vdo, completion);
    return;
  }

  prepare_admin_sub_task(vdo, makeDirty, continueLoadReadOnly);
  load_slab_depot(vdo->depot,
                  (wasNew(vdo) ? ADMIN_STATE_FORMATTING : ADMIN_STATE_LOADING),
                  completion, NULL);
}

/**********************************************************************/
int performVDOLoad(struct vdo *vdo)
{
  return perform_admin_operation(vdo, ADMIN_OPERATION_LOAD, NULL, loadCallback,
                                 loadCallback);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int startVDODecode(struct vdo *vdo, bool validateConfig)
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
                                   " does not match superblock nonce %llu",
                                   vdo->loadConfig.nonce, vdo->nonce);
  }

  BlockCount blockCount = vdo->layer->getBlockCount(vdo->layer);
  return validateVDOConfig(&vdo->config, blockCount, true);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int finishVDODecode(struct vdo *vdo)
{
  Buffer             *buffer       = getComponentBuffer(vdo->superBlock);
  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  int result = make_recovery_journal(vdo->nonce, vdo->layer,
                                     getVDOPartition(vdo->layout,
                                                     RECOVERY_JOURNAL_PARTITION),
                                     vdo->completeRecoveries,
                                     vdo->config.recoveryJournalSize,
                                     RECOVERY_JOURNAL_TAIL_BUFFER_SIZE,
                                     vdo->readOnlyNotifier, threadConfig,
                                     &vdo->recoveryJournal);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decode_recovery_journal(vdo->recoveryJournal, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decode_slab_depot(buffer, threadConfig, vdo->nonce, vdo->layer,
                             getVDOPartition(vdo->layout,
                                             SLAB_SUMMARY_PARTITION),
                             vdo->readOnlyNotifier, vdo->recoveryJournal,
                             &vdo->state, &vdo->depot);
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
 * corresponding portions of the vdo being loaded. This will also allocate the
 * recovery journal and slab depot. If this method is called with an
 * asynchronous layer (i.e. a thread config which specifies at least one base
 * thread), the block map and packer will be constructed as well.
 *
 * @param vdo             The vdo being loaded
 * @param validateConfig  Whether to validate the config
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int decodeVDO(struct vdo *vdo, bool validateConfig)
{
  int result = startVDODecode(vdo, validateConfig);
  if (result != VDO_SUCCESS) {
    return result;
  }

  const ThreadConfig *threadConfig = getThreadConfig(vdo);
  result = make_read_only_notifier(inReadOnlyMode(vdo), threadConfig,
                                   vdo->layer, &vdo->readOnlyNotifier);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = enableReadOnlyEntry(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeVDOLayout(getComponentBuffer(vdo->superBlock), &vdo->layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = finishVDODecode(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = make_flusher(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount maximumAge = getConfiguredBlockMapMaximumAge(vdo);
  BlockCount journalLength
    = get_recovery_journal_length(vdo->config.recoveryJournalSize);
  if ((maximumAge > (journalLength / 2)) || (maximumAge < 1)) {
    return VDO_BAD_CONFIGURATION;
  }
  result = makeBlockMapCaches(vdo->blockMap, vdo->layer,
                              vdo->readOnlyNotifier, vdo->recoveryJournal,
                              vdo->nonce, getConfiguredCacheSize(vdo),
                              maximumAge);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(threadConfig->hashZoneCount, struct hash_zone *, __func__,
                    &vdo->hashZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ZoneCount zone;
  for (zone = 0; zone < threadConfig->hashZoneCount; zone++) {
    result = make_hash_zone(vdo, zone, &vdo->hashZones[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  result = make_logical_zones(vdo, &vdo->logicalZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = ALLOCATE(threadConfig->physicalZoneCount, struct physical_zone *,
                    __func__, &vdo->physicalZones);
  if (result != VDO_SUCCESS) {
    return result;
  }

  for (zone = 0; zone < threadConfig->physicalZoneCount; zone++) {
    result = make_physical_zone(vdo, zone, &vdo->physicalZones[zone]);
    if (result != VDO_SUCCESS) {
      return result;
    }
  }

  return make_packer(vdo->layer, DEFAULT_PACKER_INPUT_BINS,
                     DEFAULT_PACKER_OUTPUT_BINS, threadConfig, &vdo->packer);
}

/**
 * Load the components of a VDO. This is the super block load callback
 * set by loadCallback().
 *
 * @param completion The sub-task completion
 **/
static void loadVDOComponents(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);

  prepareCompletion(completion, finishParentCallback, abortLoad,
                    completion->callbackThreadID, completion->parent);
  finishCompletion(completion, decodeVDO(vdo, true));
}

/**
 * Callback to initiate a pre-load, registered in prepareToLoadVDO().
 *
 * @param completion  The sub-task completion
 **/
static void preLoadCallback(struct vdo_completion *completion)
{
  struct vdo *vdo = vdoFromLoadSubTask(completion);
  assertOnAdminThread(vdo, __func__);
  prepare_admin_sub_task(vdo, loadVDOComponents, abortLoad);
  loadSuperBlockAsync(completion, getFirstBlockOffset(vdo), &vdo->superBlock);
}

/**********************************************************************/
int prepareToLoadVDO(struct vdo *vdo, const VDOLoadConfig *loadConfig)
{
  vdo->loadConfig = *loadConfig;
  return perform_admin_operation(vdo, ADMIN_OPERATION_LOAD, NULL,
                                 preLoadCallback, preLoadCallback);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int decodeSynchronousVDO(struct vdo *vdo, bool validateConfig)
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
int loadVDOSuperblock(PhysicalLayer           *layer,
                      struct volume_geometry  *geometry,
                      bool                     validateConfig,
                      VDODecoder              *decoder,
                      struct vdo             **vdoPtr)
{
  struct vdo *vdo;
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
            struct vdo    **vdoPtr)
{
  struct volume_geometry geometry;
  int result = loadVolumeGeometry(layer, &geometry);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return loadVDOSuperblock(layer, &geometry, validateConfig, decoder, vdoPtr);
}
