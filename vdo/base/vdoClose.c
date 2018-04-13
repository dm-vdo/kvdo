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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoClose.c#1 $
 */

#include "vdoClose.h"

#include "logger.h"

#include "adminCompletion.h"
#include "blockMap.h"
#include "completion.h"
#include "logicalZone.h"
#include "recoveryJournal.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "threadConfig.h"
#include "vdoInternal.h"

/**
 * Extract the VDO from an AdminCompletion, checking that the current operation
 * is a close.
 *
 * @param completion  The AdminCompletion's sub-task completion
 *
 * @return The VDO
 **/
static inline VDO *vdoFromCloseSubTask(VDOCompletion *completion)
{
  return vdoFromAdminSubTask(completion, ADMIN_OPERATION_CLOSE);
}

/**
 * Handle a sub-task error by recording the error and continuing with the
 * close process.
 *
 * @param completion  The sub-task completion
 **/
static void handleSubTaskError(VDOCompletion *completion)
{
  logErrorWithStringError(completion->result, "Error closing VDO");
  setCompletionResult(completion->parent, completion->result);
  resetCompletion(completion);
  completeCompletion(completion);
}

/**
 * Prepare the sub-task completion.
 *
 * @param vdo       The VDO whose AdminCompletion's sub-task completion is to
 *                  be prepared
 * @param callback  The callback for the sub task
 * @param threadID  The ID of the thread on which to run the callback
 **/
static void prepareSubTask(VDO *vdo, VDOAction *callback, ThreadID threadID)
{
  prepareAdminSubTaskOnThread(vdo, callback, handleSubTaskError, threadID);
}

/**
 * Write a clean super block now that the reference counts have been saved.
 * This callback is registered in waitForReadOnlyMode().
 *
 * @param completion  The sub-task completion
 **/
static void writeSuperBlockForClose(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareToFinishParent(completion, completion->parent);
  VDOCompletion *parent = (VDOCompletion *) completion->parent;
  if (parent->result != VDO_SUCCESS) {
    // If we've gotten an error, don't save the super block.
    completeCompletion(completion);
    return;
  }

  switch (vdo->state) {
  case VDO_DIRTY:
  case VDO_NEW:
  case VDO_CLEAN:
    vdo->state = VDO_CLEAN;
    break;

  case VDO_READ_ONLY_MODE:
  case VDO_FORCE_REBUILD:
  case VDO_RECOVERING:
  case VDO_REBUILD_FOR_UPGRADE:
    break;

  case VDO_REPLAYING:
  default:
    finishCompletion(completion, UDS_BAD_STATE);
    return;
  }

  saveVDOComponentsAsync(vdo, completion);
}

/**
 * Make sure that entering read-only mode has finished in case any step in the
 * close process has caused the VDO to enter read-only mode. This callback is
 * registered in saveDepot().
 *
 * @param completion  The sub-task completion
 **/
static void waitForReadOnlyMode(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, writeSuperBlockForClose,
                 getAdminThread(getThreadConfig(vdo)));
  waitUntilNotEnteringReadOnlyMode(vdo, completion);
}

/**
 * Save out the block allocator (slab states and reference counts), now that
 * the block map has been flushed to storage. This is the callback registered
 * in closeJournal().
 *
 * @param completion The sub-task completion
 **/
static void saveDepot(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, waitForReadOnlyMode,
                 getAdminThread(getThreadConfig(vdo)));
  saveSlabDepot(vdo->depot, true, completion);
}

/**
 * Close the recovery journal now that the block map has been
 * flushed. This is the callback registered in closeBlockMap().
 *
 * @param completion The sub-task completion
 **/
static void closeJournal(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, saveDepot, getAdminThread(getThreadConfig(vdo)));
  closeRecoveryJournal(vdo->recoveryJournal, completion);
}

/**
 * Close the block map. This is the callback registered in
 * closeLogicalZones().
 *
 * @param completion  The sub-task completion
 **/
static void closeMap(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, closeJournal,
                 getJournalZoneThread(getThreadConfig(vdo)));
  closeBlockMap(vdo->blockMap, completion);
}

/**
 * Close the logical zones. This callback is registered in
 * closeCompressionPacker().
 *
 * @param completion  The sub-task completion
 **/
static void closeLogicalZones(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, closeMap, getAdminThread(getThreadConfig(vdo)));
  closeLogicalZone(vdo->logicalZones[0], completion);
}

/**
 * Close the compression block packer.
 *
 * @param completion  The sub-task completion
 **/
static void closeCompressionPacker(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  prepareSubTask(vdo, closeLogicalZones,
                 getLogicalZoneThread(getThreadConfig(vdo), 0));
  closePacker(vdo->packer, completion);
}

/**
 * Callback to initiate a close, registered in performVDOClose().
 *
 * @param completion  The sub-task completion
 **/
static void closeCallback(VDOCompletion *completion)
{
  VDO *vdo = vdoFromCloseSubTask(completion);
  assertOnAdminThread(vdo, __func__);
  vdo->closeRequested = true;
  if (!vdo->closeRequired) {
    finishParentCallback(completion);
    return;
  }

  prepareSubTask(vdo, closeCompressionPacker,
                 getPackerZoneThread(getThreadConfig(vdo)));
  waitUntilNotEnteringReadOnlyMode(vdo, completion);
}

/**********************************************************************/
int performVDOClose(VDO *vdo)
{
  return performAdminOperation(vdo, ADMIN_OPERATION_CLOSE, closeCallback);
}
