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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/upgrade.c#4 $
 */

#include "upgrade.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "statusCodes.h"
#include "superBlock.h"
#include "vdoInternal.h"
#include "volumeGeometry.h"

/* The latest supported Sodium version */
/* Commented out because not currently used.
 * static const VersionNumber SODIUM_MASTER_VERSION_67_0 = {
 * .majorVersion = 67,
 * .minorVersion =  0,
 * };
 */

/* The component data version for current Sodium */
static const VersionNumber SODIUM_COMPONENT_DATA_41_0 = {
  .majorVersion = 41,
  .minorVersion =  0,
};

/**
 * Current Sodium's configuration of the VDO component.
 **/
typedef struct {
  VDOState  state;
  uint64_t  completeRecoveries;
  uint64_t  readOnlyRecoveries;
  VDOConfig config;
  Nonce     nonce;
} __attribute__((packed)) SodiumComponent41_0;

/**
 * Checks whether the release version loaded in the superblock is the
 * current VDO version.
 *
 * @param vdo  The VDO to validate
 *
 * @return true if the release version number is the current version
 **/
static bool isCurrentReleaseVersion(VDO *vdo)
{
  ReleaseVersionNumber loadedVersion
    = getLoadedReleaseVersion(vdo->superBlock);

  return (loadedVersion == CURRENT_RELEASE_VERSION_NUMBER);
}

/**
 * Loads the VDO master version into the VDO and checks that the version
 * can be understood by VDO.
 *
 * @param vdo  The VDO to validate
 *
 * @return VDO_SUCCESS or an error if the loaded version is not supported
 **/
static int validateSodiumVersion(VDO *vdo)
{
  int result = decodeVDOVersion(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (isCurrentReleaseVersion(vdo)) {
    return VDO_SUCCESS;
  }

  ReleaseVersionNumber loadedVersion
    = getLoadedReleaseVersion(vdo->superBlock);
  return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                 "Release version %d, load version %d.%d"
                                 " cannot be upgraded", loadedVersion,
                                 vdo->loadVersion.majorVersion,
                                 vdo->loadVersion.minorVersion);
}

/**
 * Decode a SodiumComponent41_0.
 *
 * @param buffer        The component data buffer
 * @param component     The component structure to decode into
 *
 * @return VDO_SUCCESS or an error code
 **/
static int decodeSodium41_0Component(Buffer              *buffer,
                                     SodiumComponent41_0 *component)
{
  return getBytesFromBuffer(buffer, sizeof(*component), component);
}

/**
 * Decode the component data for the VDO itself from the component data
 * buffer in the super block.
 *
 * @param vdo     The VDO to decode
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int decodeSodiumComponent(VDO *vdo)
{
  Buffer *buffer = getComponentBuffer(vdo->superBlock);
  VersionNumber version;
  int result = decodeVersionNumber(buffer, &version);
  if (result != VDO_SUCCESS) {
    return result;
  }

  SodiumComponent41_0 component;
  if (areSameVersion(SODIUM_COMPONENT_DATA_41_0, version)) {
    result = decodeSodium41_0Component(buffer, &component);
  } else {
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "VDO component data version mismatch,"
                                   " expected 41.0, got %d.%d",
                                   version.majorVersion,
                                   version.minorVersion);
  }
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

  logInfo("Converted VDO component data version %d.%d",
          version.majorVersion, version.minorVersion);
  return VDO_SUCCESS;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static int finishSodiumDecode(VDO *vdo)
{
  Buffer *buffer = getComponentBuffer(vdo->superBlock);
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

  result = decodeSodiumRecoveryJournal(vdo->recoveryJournal, buffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeSodiumSlabDepot(buffer, threadConfig, vdo->nonce, vdo->layer,
                                 getVDOPartition(vdo->layout,
                                                 SLAB_SUMMARY_PARTITION),
                                 &vdo->readOnlyContext, vdo->recoveryJournal,
                                 &vdo->depot);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = decodeSodiumBlockMap(buffer, vdo->config.logicalBlocks,
                                threadConfig, &vdo->blockMap);
  if (result != VDO_SUCCESS) {
    return result;
  }

  ASSERT_LOG_ONLY((contentLength(buffer) == 0),
                  "All decoded component data was used");
  return VDO_SUCCESS;
}

/**********************************************************************/
int upgradePriorVDO(PhysicalLayer *layer)
{
  VolumeGeometry geometry;
  int result = loadVolumeGeometry(layer, &geometry);
  if (result != VDO_SUCCESS) {
    return result;
  }

  VDO *vdo;
  result = makeVDO(layer, &vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = loadSuperBlock(vdo->layer, getDataRegionOffset(geometry),
                          &vdo->superBlock);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return logErrorWithStringError(result, "Could not load VDO super block");
  }

  // Load the necessary pieces to save again.
  result = validateSodiumVersion(vdo);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  if (isCurrentReleaseVersion(vdo)) {
    logInfo("VDO already up-to-date");
    freeVDO(&vdo);
    return VDO_SUCCESS;
  }

  result = decodeSodiumComponent(vdo);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  if (requiresRebuild(vdo)) {
    // Do not attempt to upgrade a dirty prior version.
    freeVDO(&vdo);
    return logErrorWithStringError(VDO_UNSUPPORTED_VERSION,
                                   "Cannot upgrade a dirty VDO.");
  }

  result = decodeVDOLayout(getComponentBuffer(vdo->superBlock), &vdo->layout);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  result = makeThreadDataArray((vdo->state == VDO_READ_ONLY_MODE),
                               getThreadConfig(vdo), vdo->layer,
                               &vdo->threadData);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  result = finishSodiumDecode(vdo);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  // Saving will automatically change the release version to current.
  result = saveVDOComponents(vdo);
  if (result != VDO_SUCCESS) {
    freeVDO(&vdo);
    return result;
  }

  logInfo("Successfully saved upgraded VDO");
  freeVDO(&vdo);

  return result;
}
