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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/actionManager.h#1 $
 */

#ifndef ACTION_MANAGER_H
#define ACTION_MANAGER_H

#include "completion.h"
#include "types.h"

/**
 * ActionManager provides a generic mechanism for applying actions to
 * multi-zone entities (such as the block map or slab depot). Each action
 * manager is tied to a specific context for which it manages actions. The
 * manager ensures that only one action is active on that context at a time,
 * and supports at most one pending action. Calls to schedule an action when
 * there is already a pending action will result in VDO_COMPONENT_BUSY errors.
 * Actions may only be submitted to the action manager from a single thread
 * (which thread is determined when the action manager is constructed).
 *
 * An scheduled action consists of three components:
 *   action:     an optional method to be applied to each of the zones
 *   conclusion: an optional method to be run on the initiator thread once the
 *               per-zone method has been applied to all zones
 *   parent:     an optional completion to be finished once the conclusion
 *               is done
 *
 * Either an action method or a conclusion method must be provided (or both).
 *
 * If a given operation requires multiple per-zone operations, the conclusion
 * which runs after the first action application may call continueAction() to
 * launch the next round of per-zone operations (and supply a new conclusion).
 **/

/**
 * A function which is to be applied asynchronously to a set of zones.
 *
 * @param context     The object which holds the per-zone context for the
 *                    action
 * @param zoneNumber  The number of zone to which the action is being applied
 * @param parent      The object to notify when the action is complete
 **/
typedef void ZoneAction(void          *context,
                        ZoneCount      zoneNumber,
                        VDOCompletion *parent);

/**
 * A function to conclude an action which will run on the action manager's
 * initiator thread.
 *
 * @param context  The object which holds the per-zone context for the action
 **/
typedef void ActionConclusion(void *context);

/**
 * Get the id of the thread associated with a given zone.
 *
 * @param context     The action context
 * @param zoneNumber  The number of the zone for which the thread ID is desired
 **/
typedef ThreadID ZoneThreadGetter(void *context, ZoneCount zoneNumber);

/**
 * Make an action manager.
 *
 * @param [in]  zones              The number of zones to which actions will be
 *                                 applied
 * @param [in]  getZoneThreadID    A function to get the thread id associated
 *                                 with a zone
 * @param [in]  initiatorThreadID  The thread on which actions may initiated
 * @param [in]  context            The object which holds the per-zone context
 *                                 for the action
 * @param [in]  layer              The layer used to make completions
 * @param [out] managerPtr         A pointer to hold the new action manager
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeActionManager(ZoneCount          zones,
                      ZoneThreadGetter  *getZoneThreadID,
                      ThreadID           initiatorThreadID,
                      void              *context,
                      PhysicalLayer     *layer,
                      ActionManager    **managerPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy an action manager and null out the reference to it.
 *
 * @param managerPtr  The reference to the manager to destroy
 **/
void freeActionManager(ActionManager **managerPtr);

/**
 * Check whether an action manager has a pending action.
 *
 * @param manager  The action manager to query
 *
 * @return <code>true</code> if the manager has a next action
 **/
bool hasNextAction(ActionManager *manager)
  __attribute__((warn_unused_result));

/**
 * Get the status of the current action. This method should only be called from
 * the thread the action is currently acting on.
 *
 * @param manager  The action manager to query
 *
 * @return VDO_SUCCESS or any error in the current action
 **/
int getCurrentActionStatus(ActionManager *manager)
  __attribute__((warn_unused_result));

/**
 * Schedule an action to be applied to all zones. If some other action is in
 * progress, the scheduled action will be run once the current action
 * completes.  It is an error to schedule an action if there is already a
 * pending action.
 *
 * @param manager     The action manager to schedule the action on
 * @param action      The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param parent      The object to notify once the action is complete; may be
 *                    NULL
 **/
void scheduleAction(ActionManager    *manager,
                    ZoneAction       *action,
                    ActionConclusion *conclusion,
                    VDOCompletion    *parent);

/**
 * Continue the currently running action with a new action and conclusion.
 *
 * @param manager     The action manager running the action
 * @param action      The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 **/
void continueAction(ActionManager    *manager,
                    ZoneAction       *action,
                    ActionConclusion *conclusion);

#endif // ACTION_MANAGER_H
