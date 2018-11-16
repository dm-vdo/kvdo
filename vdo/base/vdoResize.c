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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResize.c#6 $
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

/**
 * Extract the VDO from an AdminCompletion, checking that the current operation
 * is a grow physical.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The VDO
 **/
static inline VDO *vdoFromGrowPhysicalSubTask(VDOCompletion *completion)
{
  return vdoFromAdminSubTask(completion, ADMIN_OPERATION_GROW_PHYSICAL);
}

/**
 * Handle an error while reverting a failed resize or any other unrecoverable
 * state.
 *
 * @param completion  The sub-task completion
 **/
static void handleUnrecoverableError(VDOCompletion *completion)
{
  // We failed to revert a failed resize, give up.
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  enterReadOnlyMode(&vdo->readOnlyContext, completion->result);
  finishParentCallback(completion);
}

/**
 * Request the slab depot resume all the slab summary zones now that the super
 * block is written.
 *
 * @param completion  The sub-task completion
 **/
static void resumeSummaryForRevert(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  prepareAdminSubTask(vdo, finishParentCallback, handleUnrecoverableError);
  resumeSlabSummary(vdo->depot, completion, finishParentCallback,
                    finishParentCallback);
}

/**
 * Abort a resize by reverting in-memory changes to VDO components and moving
 * the slab summary back to its old location.
 *
 * @param completion  The sub-task completion
 **/
static void abortResize(VDOCompletion *completion)
{
  VDO       *vdo   = vdoFromGrowPhysicalSubTask(completion);
  SlabDepot *depot = vdo->depot;

  // Preserve the error.
  setCompletionResult(completion->parent, completion->result);

  // Revert to the old state in memory.
  vdo->config.physicalBlocks = revertVDOLayout(vdo->layout);
  updateSlabDepotSize(depot, true);
  abandonNewSlabs(depot);
  prepareAdminSubTask(vdo, resumeSummaryForRevert, handleUnrecoverableError);
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Finish the resize now that the VDO is wholly ready for using the new slabs
 * by replacing the recovery journal partition and freeing the old layout.
 *
 * @param completion  The sub-task completion
 **/
static void finishVDOResize(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  setRecoveryJournalPartition(vdo->recoveryJournal,
                              getVDOPartition(vdo->layout,
                                              RECOVERY_JOURNAL_PARTITION));
  completeCompletion(completion->parent);
}

/**
 * Request the slab depot resume all slab summary zones now that they can be
 * used.
 *
 * @param completion  The sub-task completion
 **/
static void resumeSummary(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  setSlabSummaryOrigin(getSlabSummary(vdo->depot),
                       getVDOPartition(vdo->layout, SLAB_SUMMARY_PARTITION));
  prepareAdminSubTask(vdo, finishVDOResize, handleUnrecoverableError);
  resumeSlabSummary(vdo->depot, completion, finishParentCallback,
                    finishParentCallback);
}

/**
 * Request the slab depot distribute the new slabs to all the allocators
 * now that the super block is written and they can be used.
 *
 * @param completion  The sub-task completion
 **/
static void addNewSlabs(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  prepareAdminSubTask(vdo, resumeSummary, handleUnrecoverableError);
  useNewSlabs(vdo->depot, completion, finishParentCallback,
              finishParentCallback);
}

/**
 * Update VDO components in memory that are dependent on data partition size.
 * This is the callback registered in copySuspendedSummary().
 *
 * @param completion  The sub-task completion
 **/
static void updateVDOComponentsForResize(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);

  // Update the layout and config.
  vdo->config.physicalBlocks = growVDOLayout(vdo->layout);
  updateSlabDepotSize(vdo->depot, false);
  prepareAdminSubTask(vdo, addNewSlabs, abortResize);
  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Copy the slab summary now that it's suspended. This is the callback
 * registered in suspendSummary().
 *
 * @param completion  The sub-task completion
 **/
static void copySuspendedSummary(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  prepareAdminSubTask(vdo, updateVDOComponentsForResize, abortResize);
  copyPartition(vdo->layout, SLAB_SUMMARY_PARTITION, completion);
}

/**
 * Suspend the slab summary. This callback is registered in
 * growPhysicalCallback().
 *
 * @param completion  The sub-task completion
 **/
static void suspendSummary(VDOCompletion *completion)
{
  // Set the error handler before we make the first async change that would
  // need to be undone.
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  prepareAdminSubTask(vdo, copySuspendedSummary, abortResize);
  suspendSlabSummary(vdo->depot, completion, finishParentCallback,
                     finishParentCallback);
}

/**
 * Callback to initiate a grow physical, registered in performGrowPhysical().
 *
 * @param completion  The sub-task completion
 **/
static void growPhysicalCallback(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowPhysicalSubTask(completion);
  assertOnAdminThread(vdo, __func__);

  // This check can only be done from a base code thread.
  if (isReadOnly(&vdo->readOnlyContext)) {
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  // This check should only be done from a base code thread.
  if (inRecoveryMode(vdo)) {
    finishCompletion(completion->parent, VDO_RETRY_AFTER_REBUILD);
    return;
  }

  // Copy the journal into the new layout.
  prepareAdminSubTask(vdo, suspendSummary, finishParentCallback);
  copyPartition(vdo->layout, RECOVERY_JOURNAL_PARTITION, completion);
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
                                     growPhysicalCallback);
  finishVDOLayoutGrowth(vdo->layout);
  if (result != VDO_SUCCESS) {
    return result;
  }

  logInfo("Physical block count was %" PRIu64 ", now %" PRIu64,
          oldPhysicalBlocks, newPhysicalBlocks);
  return VDO_SUCCESS;
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

  int result = prepareToGrowVDOLayout(vdo->layout, currentPhysicalBlocks,
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
