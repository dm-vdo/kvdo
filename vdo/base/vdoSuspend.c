/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoSuspend.c#5 $
 */

#include "vdoSuspend.h"

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

typedef enum {
  SUSPEND_PHASE_START = 0,
  SUSPEND_PHASE_PACKER,
  SUSPEND_PHASE_LOGICAL_ZONES,
  SUSPEND_PHASE_BLOCK_MAP,
  SUSPEND_PHASE_JOURNAL,
  SUSPEND_PHASE_DEPOT,
  SUSPEND_PHASE_WRITE_SUPER_BLOCK,
  SUSPEND_PHASE_END,
} SuspendPhase;

static const char *SUSPEND_PHASE_NAMES[] = {
  "SUSPEND_PHASE_START",
  "SUSPEND_PHASE_PACKER",
  "SUSPEND_PHASE_LOGICAL_ZONES",
  "SUSPEND_PHASE_BLOCK_MAP",
  "SUSPEND_PHASE_JOURNAL",
  "SUSPEND_PHASE_DEPOT",
  "SUSPEND_PHASE_WRITE_SUPER_BLOCK",
  "SUSPEND_PHASE_END",
};

/**
 * Implements ThreadIDGetterForPhase.
 **/
__attribute__((warn_unused_result))
static ThreadID getThreadIDForPhase(struct admin_completion *adminCompletion)
{
  const ThreadConfig *threadConfig
    = getThreadConfig(adminCompletion->completion.parent);
  switch (adminCompletion->phase) {
  case SUSPEND_PHASE_PACKER:
    return getPackerZoneThread(threadConfig);

  case SUSPEND_PHASE_JOURNAL:
    return getJournalZoneThread(threadConfig);

  default:
    return getAdminThread(threadConfig);
  }
}

/**
 * Update the VDO state and save the super block.
 *
 * @param vdo         The VDO being suspended
 * @param completion  The admin_completion's sub-task completion
 **/
static void writeSuperBlock(VDO *vdo, VDOCompletion *completion)
{
  switch (getVDOState(vdo)) {
  case VDO_DIRTY:
  case VDO_NEW:
    setVDOState(vdo, VDO_CLEAN);
    break;

  case VDO_CLEAN:
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
 * Callback to initiate a suspend, registered in performVDOSuspend().
 *
 * @param completion  The sub-task completion
 **/
static void suspendCallback(VDOCompletion *completion)
{
  struct admin_completion *adminCompletion
    = adminCompletionFromSubTask(completion);
  ASSERT_LOG_ONLY(((adminCompletion->type == ADMIN_OPERATION_SUSPEND)
                   || (adminCompletion->type == ADMIN_OPERATION_SAVE)),
                  "unexpected admin operation type %u is neither "
                  "suspend nor save", adminCompletion->type);
  assertAdminPhaseThread(adminCompletion, __func__, SUSPEND_PHASE_NAMES);

  VDO *vdo = adminCompletion->completion.parent;
  switch (adminCompletion->phase++) {
  case SUSPEND_PHASE_START:
    if (!startDraining(&vdo->adminState,
                       ((adminCompletion->type == ADMIN_OPERATION_SUSPEND)
                        ? ADMIN_STATE_SUSPENDING : ADMIN_STATE_SAVING),
                       &adminCompletion->completion, NULL)) {
      return;
    }

    if (!vdo->closeRequired) {
      // There's nothing to do.
      break;
    }

    waitUntilNotEnteringReadOnlyMode(vdo->readOnlyNotifier,
                                     resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_PACKER:
    /*
     * If the VDO was already resumed from a prior suspend while read-only,
     * some of the components may not have been resumed. By setting a read-only
     * error here, we guarantee that the result of this suspend will be
     * VDO_READ_ONLY and not VDO_INVALID_ADMIN_STATE in that case.
     */
    if (inReadOnlyMode(vdo)) {
      setCompletionResult(&adminCompletion->completion, VDO_READ_ONLY);
    }

    drainPacker(vdo->packer, resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_LOGICAL_ZONES:
    drainLogicalZones(vdo->logicalZones, vdo->adminState.state,
                      resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_BLOCK_MAP:
    drainBlockMap(vdo->blockMap, vdo->adminState.state,
                  resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_JOURNAL:
    drainRecoveryJournal(vdo->recoveryJournal, vdo->adminState.state,
                         resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_DEPOT:
    drainSlabDepot(vdo->depot, vdo->adminState.state,
                   resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
    if (isSuspending(&vdo->adminState)
        || (adminCompletion->completion.result != VDO_SUCCESS)) {
      // If we didn't save the VDO or there was an error, we're done.
      break;
    }

    writeSuperBlock(vdo, resetAdminSubTask(completion));
    return;

  case SUSPEND_PHASE_END:
    break;

  default:
    setCompletionResult(completion, UDS_BAD_STATE);
  }

  finishDrainingWithResult(&vdo->adminState, completion->result);
}

/**********************************************************************/
int performVDOSuspend(VDO *vdo, bool save)
{
  return performAdminOperation(vdo, (save
                                     ? ADMIN_OPERATION_SAVE
                                     : ADMIN_OPERATION_SUSPEND),
                               getThreadIDForPhase, suspendCallback,
                               preserveErrorAndContinue);
}
