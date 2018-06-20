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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoInternal.h#5 $
 */

#ifndef VDO_INTERNAL_H
#define VDO_INTERNAL_H

#include "vdo.h"

#include "atomic.h"
#include "header.h"
#include "packer.h"
#include "readOnlyModeContextInternals.h"
#include "statistics.h"
#include "superBlock.h"
#include "threadData.h"
#include "uds.h"
#include "vdoLayout.h"

/**
 * Error counters are atomic since updates can arrive concurrently from
 * arbitrary threads.
 **/
typedef struct atomicErrorStatistics {
  // Dedupe path error stats
  Atomic64 invalidAdvicePBNCount;
  Atomic64 noSpaceErrorCount;
  Atomic64 readOnlyErrorCount;
} AtomicErrorStatistics;

struct vdo {
  /* The state of this VDO */
  VDOState              state;
  /* The context for entering read-only mode */
  ReadOnlyModeContext   readOnlyContext;
  /* The number of times this VDO has recovered from a dirty state */
  uint64_t              completeRecoveries;
  /* The number of times this VDO has recovered from a read-only state */
  uint64_t              readOnlyRecoveries;
  /* The format-time configuration of this VDO */
  VDOConfig             config;
  /* The load-time configuration of this VDO */
  VDOLoadConfig         loadConfig;
  /* The nonce for this VDO */
  Nonce                 nonce;

  /* The super block */
  SuperBlock           *superBlock;

  /* The physical storage below us */
  PhysicalLayer        *layer;

  /* Our partitioning of the physical layer's storage */
  VDOLayout            *layout;

  /* The block map */
  BlockMap             *blockMap;

  /* The journal for block map recovery */
  RecoveryJournal      *recoveryJournal;

  /* The slab depot */
  SlabDepot            *depot;

  /* The compressed-block packer */
  Packer               *packer;
  /* Whether incoming data should be compressed */
  AtomicBool            compressing;

  /* The handler for flush requests */
  Flusher              *flusher;

  /* The master version of the VDO when loaded (for upgrading) */
  VersionNumber         loadVersion;
  /* The state the VDO was in when loaded (primarily for unit tests) */
  VDOState              loadState;
  /* Whether VIO tracing is enabled */
  bool                  vioTraceRecording;

  /* The per-thread data for this VDO */
  ThreadData            *threadData;

  /* The logical zones of this VDO */
  LogicalZone          **logicalZones;

  /* The physical zones of this VDO */
  PhysicalZone         **physicalZones;

  /* The hash lock zones of this VDO */
  HashZone             **hashZones;

  /* The completion for administrative operations */
  AdminCompletion       *adminCompletion;

  /* Whether a close is required */
  bool                   closeRequired;

  /* Whether a close has been requested */
  bool                   closeRequested;

  /* Atomic global counts of error events */
  AtomicErrorStatistics  errorStats;
};

/**
 * Get the component data size of a VDO.
 *
 * @param vdo  The VDO whose component data size is desired
 *
 * @return the component data size of the VDO
 **/
size_t getComponentDataSize(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Encode the VDO and save the super block synchronously.
 *
 * @param vdo  The VDO whose state is being saved
 *
 * @return VDO_SUCCESS or an error
 **/
int saveVDOComponents(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Encode the VDO and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * saveSuperBlockAsync() directly.
 *
 * @param vdo     The VDO whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void saveVDOComponentsAsync(VDO *vdo, VDOCompletion *parent);

/**
 * Re-encode the VDO component after a reconfiguration and save the super
 * block synchronously. This function avoids the need to decode and re-encode
 * the other components by simply copying their previous encoding.
 *
 * @param vdo  The VDO which was reconfigured
 *
 * @return VDO_SUCCESS or an error code
 **/
int saveReconfiguredVDO(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Decode the VDO master version from the component data buffer in the super
 * block and store it in the VDO's loadVersion field.
 **/
int decodeVDOVersion(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Loads the VDO master version into the VDO and checks that the version
 * can be understood by VDO.
 *
 * @param vdo  The VDO to validate
 *
 * @return VDO_SUCCESS or an error if the loaded version is not supported
 **/
int validateVDOVersion(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Decode the component data for the VDO itself from the component data buffer
 * in the super block.
 *
 * @param vdo  The VDO to decode
 *
 * @return VDO_SUCCESS or an error
 **/
int decodeVDOComponent(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Validate constraints on VDO config.
 *
 * @param config          The VDO config
 * @param blockCount      The block count of the VDO
 * @param requireLogical  Set to <code>true</code> if the number logical blocks
 *                        must be configured (otherwise, it may be zero)
 *
 * @return a success or error code
 **/
int validateVDOConfig(const VDOConfig *config,
                      BlockCount       blockCount,
                      bool             requireLogical)
  __attribute__((warn_unused_result));

/**
 * Get the VDO's thread data for the current thread.
 *
 * @param vdo  The VDO
 **/
ThreadData *getThreadData(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the block map.
 *
 * @param vdo  The VDO whose block map is desired
 *
 * @return the block map from the VDO
 **/
BlockMap *getBlockMap(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the slab depot from a VDO.
 *
 * @param vdo  The VDO whose slab depot is desired
 *
 * @return the slab depot from the VDO
 **/
SlabDepot *getSlabDepot(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the recovery journal from a VDO.
 *
 * @param vdo  The VDO whose recovery journal is desired
 *
 * @return the recovery journal from the VDO
 **/
RecoveryJournal *getRecoveryJournal(VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a VDO is in read-only mode.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO is in read-only mode
 **/
bool inReadOnlyMode(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the VDO is in a clean state.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO is clean
 **/
bool isClean(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the VDO was in a clean state when it was loaded.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO was clean
 **/
bool wasClean(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the VDO requires a read-only mode rebuild.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO requires a read-only rebuild
 **/
bool requiresReadOnlyRebuild(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a VDO requires rebuilding.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO must be rebuilt
 **/
bool requiresRebuild(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a VDO should enter recovery mode.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO requires recovery
 **/
bool requiresRecovery(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a VDO was replaying the recovery journal into the block map
 * when it crashed.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO crashed while reconstructing the
 *         block map
 **/
bool isReplaying(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the VDO is in recovery mode.
 *
 * @param vdo  The VDO to query
 *
 * @return <code>true</code> if the VDO is in recovery mode
 **/
bool inRecoveryMode(const VDO *vdo)
  __attribute__((warn_unused_result));

/**
 * Put the VDO into recovery mode
 *
 * @param vdo  The VDO
 **/
void enterRecoveryMode(VDO *vdo);

/**
 * Leave recovery mode if slab scrubbing has actually finished.
 *
 * @param vdo  The VDO
 **/
void leaveRecoveryMode(VDO *vdo);

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The VDO
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void assertOnAdminThread(VDO *vdo, const char *name);

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo          The VDO
 * @param logicalZone  The number of the logical zone
 * @param name         The name of the calling function
 **/
void assertOnLogicalZoneThread(const VDO  *vdo,
                               ZoneCount   logicalZone,
                               const char *name);

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The VDO containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
HashZone *selectHashZone(const VDO *vdo, const UdsChunkName *name)
  __attribute__((warn_unused_result));

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this VDO instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the VDO
 * into read-only mode.
 *
 * @param [in]  vdo      The VDO containing the physical zones
 * @param [in]  pbn      The PBN of the data block
 * @param [out] zonePtr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int getPhysicalZone(const VDO            *vdo,
                    PhysicalBlockNumber   pbn,
                    PhysicalZone        **zonePtr)
  __attribute__((warn_unused_result));

/**********************************************************************/
// Asynchronous callback to share a duplicate block. This is only public so
// test code may compare it against the current callback in the completion.
void shareBlock(VDOCompletion *completion);

#endif /* VDO_INTERNAL_H */
