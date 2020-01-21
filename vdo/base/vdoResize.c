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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResize.c#15 $
 */

#include "vdoResize.h"

#include "logger.h"

#include "adminCompletion.h"
#include "completion.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "vdoInternal.h"
#include "vdoLayout.h"

typedef enum {
  GROW_PHYSICAL_PHASE_START = 0,
  GROW_PHYSICAL_PHASE_COPY_SUMMARY,
  GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS,
  GROW_PHYSICAL_PHASE_USE_NEW_SLABS,
  GROW_PHYSICAL_PHASE_END,
  GROW_PHYSICAL_PHASE_ERROR,
} GrowPhysicalPhase;

static const char *GROW_PHYSICAL_PHASE_NAMES[] = {
  "GROW_PHYSICAL_PHASE_START",
  "GROW_PHYSICAL_PHASE_COPY_SUMMARY",
  "GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS",
  "GROW_PHYSICAL_PHASE_USE_NEW_SLABS",
  "GROW_PHYSICAL_PHASE_END",
  "GROW_PHYSICAL_PHASE_ERROR",
};

/**
 * Implements ThreadIDGetterForPhase.
 **/
__attribute__((warn_unused_result))
static ThreadID getThreadIDForPhase(AdminCompletion *adminCompletion)
{
  return getAdminThread(getThreadConfig(adminCompletion->completion.parent));
}

/**
 * Callback to initiate a grow physical, registered in performGrowPhysical().
 *
 * @param completion  The sub-task completion
 **/
static void growPhysicalCallback(VDOCompletion *completion)
{
  AdminCompletion *adminCompletion = adminCompletionFromSubTask(completion);
  assertAdminOperationType(adminCompletion, ADMIN_OPERATION_GROW_PHYSICAL);
  assertAdminPhaseThread(adminCompletion, __func__, GROW_PHYSICAL_PHASE_NAMES);

  VDO *vdo = adminCompletion->completion.parent;
  switch (adminCompletion->phase++) {
  case GROW_PHYSICAL_PHASE_START:
    if (isReadOnly(vdo->readOnlyNotifier)) {
      logErrorWithStringError(VDO_READ_ONLY,
                              "Can't grow physical size of a read-only VDO");
      setCompletionResult(resetAdminSubTask(completion), VDO_READ_ONLY);
      break;
    }

    if (startOperationWithWaiter(&vdo->adminState,
                                 ADMIN_STATE_SUSPENDED_OPERATION,
                                 &adminCompletion->completion, NULL)) {
      // Copy the journal into the new layout.
      copyPartition(vdo->layout, RECOVERY_JOURNAL_PARTITION,
                    resetAdminSubTask(completion));
    }
    return;

  case GROW_PHYSICAL_PHASE_COPY_SUMMARY:
    copyPartition(vdo->layout, SLAB_SUMMARY_PARTITION,
                  resetAdminSubTask(completion));
    return;

  case GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS:
    vdo->config.physicalBlocks = growVDOLayout(vdo->layout);
    updateSlabDepotSize(vdo->depot);
    saveVDOComponentsAsync(vdo, resetAdminSubTask(completion));
    return;

  case GROW_PHYSICAL_PHASE_USE_NEW_SLABS:
    useNewSlabs(vdo->depot, resetAdminSubTask(completion));
    return;

  case GROW_PHYSICAL_PHASE_END:
    setSlabSummaryOrigin(getSlabSummary(vdo->depot),
                         getVDOPartition(vdo->layout, SLAB_SUMMARY_PARTITION));
    setRecoveryJournalPartition(vdo->recoveryJournal,
                                getVDOPartition(vdo->layout,
                                                RECOVERY_JOURNAL_PARTITION));
    break;

  case GROW_PHYSICAL_PHASE_ERROR:
    enterReadOnlyMode(vdo->readOnlyNotifier, completion->result);
    break;

  default:
    setCompletionResult(resetAdminSubTask(completion), UDS_BAD_STATE);
  }

  finishVDOLayoutGrowth(vdo->layout);
  finishOperationWithResult(&vdo->adminState, completion->result);
}

/**
 * Handle an error during the grow physical process.
 *
 * @param completion  The sub-task completion
 **/
static void handleGrowthError(VDOCompletion *completion)
{
  adminCompletionFromSubTask(completion)->phase = GROW_PHYSICAL_PHASE_ERROR;
  growPhysicalCallback(completion);
}

/**********************************************************************/
int performGrowPhysical(VDO *vdo, BlockCount newPhysicalBlocks)
{
  BlockCount oldPhysicalBlocks = vdo->config.physicalBlocks;

  // Skip any noop grows.
  if (oldPhysicalBlocks == newPhysicalBlocks) {
    return VDO_SUCCESS;
  }

  if (newPhysicalBlocks != getNextVDOLayoutSize(vdo->layout)) {
    /*
     * Either the VDO isn't prepared to grow, or it was prepared to grow
     * to a different size. Doing this check here relies on the fact that
     * the call to this method is done under the dmsetup message lock.
     */
    finishVDOLayoutGrowth(vdo->layout);
    abandonNewSlabs(vdo->depot);
    return VDO_PARAMETER_MISMATCH;
  }

  // Validate that we are prepared to grow appropriately.
  BlockCount newDepotSize = getNextBlockAllocatorPartitionSize(vdo->layout);
  BlockCount preparedDepotSize = getNewDepotSize(vdo->depot);
  if (preparedDepotSize != newDepotSize) {
    return VDO_PARAMETER_MISMATCH;
  }

  int result = performAdminOperation(vdo, ADMIN_OPERATION_GROW_PHYSICAL,
                                     getThreadIDForPhase, growPhysicalCallback,
                                     handleGrowthError);
  if (result != VDO_SUCCESS) {
    return result;
  }

  logInfo("Physical block count was %" PRIu64 ", now %" PRIu64,
          oldPhysicalBlocks, newPhysicalBlocks);
  return VDO_SUCCESS;
}

/**
 * Callback to check that we're not in recovery mode, used in
 * prepareToGrowPhysical().
 *
 * @param completion  The sub-task completion
 **/
static void checkMayGrowPhysical(VDOCompletion *completion)
{
  AdminCompletion *adminCompletion = adminCompletionFromSubTask(completion);
  assertAdminOperationType(adminCompletion,
                           ADMIN_OPERATION_PREPARE_GROW_PHYSICAL);

  VDO *vdo = adminCompletion->completion.parent;
  assertOnAdminThread(vdo, __func__);

  resetAdminSubTask(completion);

  // This check can only be done from a base code thread.
  if (isReadOnly(vdo->readOnlyNotifier)) {
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  // This check should only be done from a base code thread.
  if (inRecoveryMode(vdo)) {
    finishCompletion(completion->parent, VDO_RETRY_AFTER_REBUILD);
    return;
  }

  completeCompletion(completion->parent);
}

/**********************************************************************/
int prepareToGrowPhysical(VDO *vdo, BlockCount newPhysicalBlocks)
{
  BlockCount currentPhysicalBlocks = vdo->config.physicalBlocks;
  if (newPhysicalBlocks < currentPhysicalBlocks) {
    return logErrorWithStringError(VDO_NOT_IMPLEMENTED,
                                   "Removing physical storage from a VDO is "
                                   "not supported");
  }

  if (newPhysicalBlocks == currentPhysicalBlocks) {
    logWarning("Requested physical block count %" PRIu64
               " not greater than %" PRIu64,
               newPhysicalBlocks, currentPhysicalBlocks);
    finishVDOLayoutGrowth(vdo->layout);
    abandonNewSlabs(vdo->depot);
    return VDO_PARAMETER_MISMATCH;
  }

  int result = performAdminOperation(vdo,
                                     ADMIN_OPERATION_PREPARE_GROW_PHYSICAL,
                                     getThreadIDForPhase, checkMayGrowPhysical,
                                     finishParentCallback);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = prepareToGrowVDOLayout(vdo->layout, currentPhysicalBlocks,
                                  newPhysicalBlocks, vdo->layer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  BlockCount newDepotSize = getNextBlockAllocatorPartitionSize(vdo->layout);
  result = prepareToGrowSlabDepot(vdo->depot, newDepotSize);
  if (result != VDO_SUCCESS) {
    finishVDOLayoutGrowth(vdo->layout);
    return result;
  }

  return VDO_SUCCESS;
}
