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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoResizeLogical.c#3 $
 */

#include "vdoResizeLogical.h"

#include "logger.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "vdoInternal.h"

/**
 * Extract the VDO from an AdminCompletion, checking that the current operation
 * is a grow logical.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The VDO
 **/
static inline VDO *vdoFromGrowLogicalSubTask(VDOCompletion *completion)
{
  return vdoFromAdminSubTask(completion, ADMIN_OPERATION_GROW_LOGICAL);
}

/**
 * Expand the block map now that the VDOConfig has been updated on disk.
 * We are relying on the fact that expanding the block map at this point can
 * not have any errors. If it could, we'd need to find a way to back out the
 * config update. This callback is registered in suspendBlockMap().
 *
 * @param completion  The sub-task completion
 **/
static void resizeBlockMap(VDOCompletion *completion)
{
  BlockMap *blockMap = getBlockMap(vdoFromGrowLogicalSubTask(completion));
  growBlockMap(blockMap);

  // XXX: Once the VDO has a real suspend transaction, this resume must go away.
  prepareToFinishParent(completion, completion->parent);
  resumeBlockMap(blockMap, completion);
}

/**
 * Handle an error attempting to suspend and grow the block map. This
 * error handler is registered in suspendBlockMap().
 *
 * @param completion  The sub-task completion
 **/
static void handleBlockMapError(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowLogicalSubTask(completion);
  enterReadOnlyMode(vdo->readOnlyNotifier, completion->result);
  finishParentCallback(completion);
}

/**
 * Suspend the block map so that we can expand it.
 *
 * XXX: This method is a stop-gap until there is a whole VDO suspend
 * transaction.
 *
 * @param completion  The sub-task completion
 **/
static void suspendBlockMap(VDOCompletion *completion)
{
  prepareCompletion(completion, resizeBlockMap, handleBlockMapError,
                    completion->callbackThreadID, completion->parent);
  drainBlockMap(getBlockMap(vdoFromGrowLogicalSubTask(completion)),
                ADMIN_STATE_SUSPENDING, completion);
}

/**
 * Handle an error attempting to save the updated VDOConfig. This error handler
 * is registered in growLogicalCallback().
 *
 * @param completion  The sub-task completion
 **/
static void handleSaveError(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowLogicalSubTask(completion);
  BlockMap *blockMap = getBlockMap(vdo);
  abandonBlockMapGrowth(blockMap);
  vdo->config.logicalBlocks = getNumberOfBlockMapEntries(blockMap);
  finishParentCallback(completion);
}

/**
 * Callback to initiate a grow logical, registered in performGrowLogical().
 *
 * @param completion  The sub-task completion
 **/
static void growLogicalCallback(VDOCompletion *completion)
{
  VDO *vdo = vdoFromGrowLogicalSubTask(completion);
  assertOnAdminThread(vdo, __func__);

  // This check can only be done from a base thread.
  if (isReadOnly(vdo->readOnlyNotifier)) {
    abandonBlockMapGrowth(getBlockMap(vdo));
    logErrorWithStringError(VDO_READ_ONLY,
                            "Can't grow logical size of a read-only VDO");
    finishCompletion(completion->parent, VDO_READ_ONLY);
    return;
  }

  vdo->config.logicalBlocks = getNewEntryCount(getBlockMap(vdo));
  prepareAdminSubTask(vdo, suspendBlockMap, handleSaveError);
  saveVDOComponentsAsync(vdo, completion);
}

/**********************************************************************/
int performGrowLogical(VDO *vdo, BlockCount newLogicalBlocks)
{
  if (getNewEntryCount(getBlockMap(vdo)) != newLogicalBlocks) {
    return VDO_PARAMETER_MISMATCH;
  }

  return performAdminOperation(vdo, ADMIN_OPERATION_GROW_LOGICAL,
                               growLogicalCallback);
}

/**********************************************************************/
int prepareToGrowLogical(VDO *vdo, BlockCount newLogicalBlocks)
{
  if (newLogicalBlocks < vdo->config.logicalBlocks) {
    return logErrorWithStringError(VDO_PARAMETER_MISMATCH,
                                   "Can't shrink VDO logical size from its "
                                   "current value of %" PRIu64,
                                   vdo->config.logicalBlocks);
  }

  if (newLogicalBlocks == vdo->config.logicalBlocks) {
    return logErrorWithStringError(VDO_PARAMETER_MISMATCH,
                                   "Can't grow VDO logical size to its "
                                   "current value of %" PRIu64,
                                   vdo->config.logicalBlocks);
  }

  return prepareToGrowBlockMap(getBlockMap(vdo), newLogicalBlocks);
}
