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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoInternal.h#26 $
 */

#ifndef VDO_INTERNAL_H
#define VDO_INTERNAL_H

#include "vdo.h"

#include "adminCompletion.h"
#include "adminState.h"
#include "atomic.h"
#include "header.h"
#include "packer.h"
#include "statistics.h"
#include "superBlock.h"
#include "readOnlyNotifier.h"
#include "types.h"
#include "uds.h"
#include "vdoLayout.h"
#include "vdoState.h"

/**
 * Error counters are atomic since updates can arrive concurrently from
 * arbitrary threads.
 **/
struct atomic_error_statistics {
  // Dedupe path error stats
  Atomic64 invalidAdvicePBNCount;
  Atomic64 noSpaceErrorCount;
  Atomic64 readOnlyErrorCount;
};

struct vdo {
  /* The state of this vdo */
  Atomic32                         state;
  /* The read-only notifier */
  struct read_only_notifier       *readOnlyNotifier;
  /* The number of times this vdo has recovered from a dirty state */
  uint64_t                         completeRecoveries;
  /* The number of times this vdo has recovered from a read-only state */
  uint64_t                         readOnlyRecoveries;
  /* The format-time configuration of this vdo */
  VDOConfig                        config;
  /* The load-time configuration of this vdo */
  VDOLoadConfig                    loadConfig;
  /* The nonce for this vdo */
  Nonce                            nonce;

  /* The super block */
  struct vdo_super_block          *superBlock;

  /* The physical storage below us */
  PhysicalLayer                   *layer;

  /* Our partitioning of the physical layer's storage */
  struct vdo_layout               *layout;

  /* The block map */
  struct block_map                *blockMap;

  /* The journal for block map recovery */
  struct recovery_journal         *recoveryJournal;

  /* The slab depot */
  struct slab_depot               *depot;

  /* The compressed-block packer */
  struct packer                   *packer;
  /* Whether incoming data should be compressed */
  AtomicBool                       compressing;

  /* The handler for flush requests */
  struct flusher                  *flusher;

  /* The master version of the vdo when loaded (for upgrading) */
  struct version_number            loadVersion;
  /* The state the vdo was in when loaded (primarily for unit tests) */
  VDOState                         loadState;
  /* Whether VIO tracing is enabled */
  bool                             vioTraceRecording;

  /* The logical zones of this vdo */
  struct logical_zones            *logicalZones;

  /* The physical zones of this vdo */
  struct physical_zone           **physicalZones;

  /* The hash lock zones of this vdo */
  struct hash_zone               **hashZones;

  /* The completion for administrative operations */
  struct admin_completion          adminCompletion;

  /* The administrative state of the vdo */
  struct admin_state               adminState;

  /* Whether a close is required */
  bool                             closeRequired;

  /* Atomic global counts of error events */
  struct atomic_error_statistics   errorStats;
};

/**
 * Get the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo  The vdo
 *
 * @return the current state of the vdo
 **/
VDOState getVDOState(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Set the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo    The vdo whose state is to be set
 * @param state  The new state of the vdo
 **/
void setVDOState(struct vdo *vdo, VDOState state);

/**
 * Get the component data size of a vdo.
 *
 * @param vdo  The vdo whose component data size is desired
 *
 * @return the component data size of the vdo
 **/
size_t getComponentDataSize(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Encode the vdo and save the super block synchronously.
 *
 * @param vdo  The vdo whose state is being saved
 *
 * @return VDO_SUCCESS or an error
 **/
int saveVDOComponents(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Encode the vdo and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * saveSuperBlockAsync() directly.
 *
 * @param vdo     The vdo whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void saveVDOComponentsAsync(struct vdo *vdo, struct vdo_completion *parent);

/**
 * Re-encode the vdo component after a reconfiguration and save the super
 * block synchronously. This function avoids the need to decode and re-encode
 * the other components by simply copying their previous encoding.
 *
 * @param vdo  The vdo which was reconfigured
 *
 * @return VDO_SUCCESS or an error code
 **/
int saveReconfiguredVDO(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Decode the vdo master version from the component data buffer in the super
 * block and store it in the vdo's loadVersion field.
 **/
int decodeVDOVersion(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Loads the vdo master version into the vdo and checks that the version
 * can be understood by vdo.
 *
 * @param vdo  The vdo to validate
 *
 * @return VDO_SUCCESS or an error if the loaded version is not supported
 **/
int validateVDOVersion(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Decode the component data for the vdo itself from the component data buffer
 * in the super block.
 *
 * @param vdo  The vdo to decode
 *
 * @return VDO_SUCCESS or an error
 **/
int decodeVDOComponent(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Validate constraints on a VDO config.
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
 * Enable a vdo to enter read-only mode on errors.
 *
 * @param vdo  The vdo to enable
 *
 * @return VDO_SUCCESS or an error
 **/
int enableReadOnlyEntry(struct vdo *vdo);

/**
 * Get the block map.
 *
 * @param vdo  The vdo whose block map is desired
 *
 * @return the block map from the vdo
 **/
struct block_map *getBlockMap(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the slab depot from a vdo.
 *
 * @param vdo  The vdo whose slab depot is desired
 *
 * @return the slab depot from the vdo
 **/
struct slab_depot *getSlabDepot(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Get the recovery journal from a vdo.
 *
 * @param vdo  The vdo whose recovery journal is desired
 *
 * @return the recovery journal from the vdo
 **/
struct recovery_journal *getRecoveryJournal(struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a vdo is in read-only mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in read-only mode
 **/
bool inReadOnlyMode(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the vdo requires a read-only mode rebuild.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires a read-only rebuild
 **/
bool requiresReadOnlyRebuild(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a vdo requires rebuilding.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo must be rebuilt
 **/
bool requiresRebuild(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a vdo should enter recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo requires recovery
 **/
bool requiresRecovery(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether a vdo was replaying the recovery journal into the block map
 * when it crashed.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo crashed while reconstructing the
 *         block map
 **/
bool isReplaying(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Check whether the vdo is in recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in recovery mode
 **/
bool inRecoveryMode(const struct vdo *vdo)
  __attribute__((warn_unused_result));

/**
 * Put the vdo into recovery mode
 *
 * @param vdo  The vdo
 **/
void enterRecoveryMode(struct vdo *vdo);

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The vdo
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void assertOnAdminThread(struct vdo *vdo, const char *name);

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo          The vdo
 * @param logicalZone  The number of the logical zone
 * @param name         The name of the calling function
 **/
void assertOnLogicalZoneThread(const struct vdo *vdo,
                               ZoneCount         logicalZone,
                               const char       *name);

/**
 * Assert that this function was called on the specified physical zone thread.
 *
 * @param vdo           The vdo
 * @param physicalZone  The number of the physical zone
 * @param name          The name of the calling function
 **/
void assertOnPhysicalZoneThread(const struct vdo *vdo,
                                ZoneCount         physicalZone,
                                const char       *name);

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The vdo containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
struct hash_zone *selectHashZone(const struct vdo *vdo, const UdsChunkName *name)
  __attribute__((warn_unused_result));

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * @param [in]  vdo      The vdo containing the physical zones
 * @param [in]  pbn      The PBN of the data block
 * @param [out] zonePtr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int getPhysicalZone(const struct vdo      *vdo,
                    PhysicalBlockNumber    pbn,
                    struct physical_zone **zonePtr)
  __attribute__((warn_unused_result));

/**********************************************************************/
// Asynchronous callback to share a duplicate block. This is only public so
// test code may compare it against the current callback in the completion.
void share_block(struct vdo_completion *completion);

#endif /* VDO_INTERNAL_H */
