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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/actionManager.h#3 $
 */

#ifndef ACTION_MANAGER_H
#define ACTION_MANAGER_H

#include "adminState.h"
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
 * A scheduled action consists of four components:
 *   preamble:   an optional method to be run on the initator thread before
 *               applying the action to all zones
 *   zoneAction: an optional method to be applied to each of the zones
 *   conclusion: an optional method to be run on the initiator thread once the
 *               per-zone method has been applied to all zones
 *   parent:     an optional completion to be finished once the conclusion
 *               is done
 *
 * At least one of the three methods must be provided.
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
 * A function which will run on the action manager's initiator thread, either
 * as a preamble or conclusion of an action.
 *
 * @param context  The object which holds the per-zone context for the action
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int InitiatorAction(void *context);

/**
 * A function to schedule an action.
 *
 * @param context  The object which holds the per-zone context for the action
 *
 * @return <code>true</code> if an action was scheduled
 **/
typedef bool ActionScheduler(void *context);

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
 * @param [in]  scheduler          A function to schedule a next action after an
 *                                 action concludes if there is no pending
 *                                 action (may be NULL)
 * @param [in]  layer              The layer used to make completions
 * @param [out] managerPtr         A pointer to hold the new action manager
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeActionManager(ZoneCount          zones,
                      ZoneThreadGetter  *getZoneThreadID,
                      ThreadID           initiatorThreadID,
                      void              *context,
                      ActionScheduler   *scheduler,
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
 * Get the current operation an action manager is performing.
 *
 * @param manager  The manager to query
 *
 * @return The manager's current operation
 **/
AdminStateCode getCurrentManagerOperation(ActionManager *manager)
  __attribute__((warn_unused_result));

/**
 * Schedule an action to be applied to all zones. The action will be launched
 * immediately if there is no current action, or as soon as the current action
 * completes. If there is already a pending action, this action will not be
 * scheduled, and, if it has a parent, that parent will be notified. At least
 * one of the preamble, zoneAction, or conclusion must not be NULL.
 *
 * @param manager     The action manager to schedule the action on
 * @param preamble    A method to be invoked on the initiator thread once this
 *                    action is started but before applying to each zone; may
 *                    be NULL
 * @param zoneAction  The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param parent      The object to notify once the action is complete or if
 *                    the action can not be scheduled; may be NULL
 *
 * @return <code>true</code> if the action was scheduled
 **/
bool scheduleAction(ActionManager   *manager,
                    InitiatorAction *preamble,
                    ZoneAction      *zoneAction,
                    InitiatorAction *conclusion,
                    VDOCompletion   *parent);

/**
 * Schedule an operation to be applied to all zones. The operation's action
 * will be launched immediately if there is no current action, or as soon as
 * the current action completes. If there is already a pending action, this
 * operation will not be scheduled, and, if it has a parent, that parent will
 * be notified. At least one of the preamble, zoneAction, or conclusion must
 * not be NULL.
 *
 * @param manager     The action manager to schedule the action on
 * @param operation   The operation this action will perform
 * @param preamble    A method to be invoked on the initiator thread once this
 *                    action is started but before applying to each zone; may
 *                    be NULL
 * @param zoneAction  The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param parent      The object to notify once the action is complete or if
 *                    the action can not be scheduled; may be NULL
 *
 * @return <code>true</code> if the action was scheduled
 **/
bool scheduleOperation(ActionManager   *manager,
                       AdminStateCode   operation,
                       InitiatorAction *preamble,
                       ZoneAction      *zoneAction,
                       InitiatorAction *conclusion,
                       VDOCompletion   *parent);

#endif // ACTION_MANAGER_H
