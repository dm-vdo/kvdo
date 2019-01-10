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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/actionManager.c#2 $
 */

#include "actionManager.h"

#include "memoryAlloc.h"

#include "completion.h"
#include "types.h"

/** An action to be performed in each of a set of zones */
typedef struct {
  /** The action to be performed in each zone */
  ZoneAction       *action;
  /**
   * The method to run on the initiator thread after the action has been
   * applied to each zone
   **/
  ActionConclusion *conclusion;
  /** The object to notify when the action is complete */
  VDOCompletion    *parent;
} Action;

struct actionManager {
  /** The completion for performing actions */
  VDOCompletion     completion;
  /** The currently running action */
  Action            currentAction;
  /** The next action to run */
  Action            nextAction;
  /** The number of zones in which an action is to be applied */
  ZoneCount         zones;
  /**
   * A function to get the id of the thread on which to apply an action to a
   * zone
   **/
  ZoneThreadGetter *getZoneThreadID;
  /** The ID of the thread on which actions may be initiated */
  ThreadID          initiatorThreadID;
  /** Opaque data associated with this action manager */
  void             *context;
  /** The zone currently being acted upon */
  ZoneCount         actingZone;
  /** Whether the action manager is currently acting */
  bool              acting;
};

/**
 * Convert a generic VDOCompletion to a ActionManager.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a ActionManager
 **/
static inline ActionManager *asActionManager(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(ActionManager, completion) == 0);
  assertCompletionType(completion->type, ACTION_COMPLETION);
  return (ActionManager *) completion;
}

/**********************************************************************/
int makeActionManager(ZoneCount          zones,
                      ZoneThreadGetter  *getZoneThreadID,
                      ThreadID           initiatorThreadID,
                      void              *context,
                      PhysicalLayer     *layer,
                      ActionManager    **managerPtr)
{
  ActionManager *manager;
  int result = ALLOCATE(1, ActionManager, __func__, &manager);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *manager = (ActionManager) {
    .zones             = zones,
    .getZoneThreadID   = getZoneThreadID,
    .initiatorThreadID = initiatorThreadID,
    .context           = context,
  };

  result = initializeEnqueueableCompletion(&manager->completion,
                                           ACTION_COMPLETION, layer);
  if (result != VDO_SUCCESS) {
    freeActionManager(&manager);
    return result;
  }

  *managerPtr = manager;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeActionManager(ActionManager **managerPtr)
{
  ActionManager *manager = *managerPtr;
  if (manager == NULL) {
    return;
  }

  destroyEnqueueable(&manager->completion);
  FREE(manager);
  *managerPtr = NULL;
}

/**********************************************************************/
bool hasNextAction(ActionManager *manager)
{
  return ((manager->nextAction.action != NULL)
          || (manager->nextAction.conclusion != NULL));
}

/**********************************************************************/
int getCurrentActionStatus(ActionManager *manager)
{
  return ((manager->acting && (manager->currentAction.parent != NULL))
          ? manager->currentAction.parent->result : VDO_SUCCESS);
}

/**
 * Handle an error when applying an action to a zone.
 *
 * @param completion  The manager completion
 **/
static void handleZoneError(VDOCompletion *completion)
{
  // Preserve the error.
  if (completion->parent != NULL) {
    setCompletionResult(completion->parent, completion->result);
  }

  // Carry on.
  resetCompletion(completion);
  invokeCallback(completion);
}

/**********************************************************************/
static void finishActionCallback(VDOCompletion *completion);
static void applyToZone(VDOCompletion *completion);

/**
 * Get the thread ID for the current zone.
 *
 * @param manager  The action manager
 *
 * @return The ID of the thread on which to run actions for the current zone
 **/
static ThreadID getActingZoneThreadID(ActionManager *manager)
{
  return manager->getZoneThreadID(manager->context, manager->actingZone);
}

/**
 * Prepare the manager's completion to run on the next zone.
 *
 * @param manager  The action manager
 **/
static void prepareForNextZone(ActionManager *manager)
{
  prepareForRequeue(&manager->completion, applyToZone, handleZoneError,
                    getActingZoneThreadID(manager),
                    manager->currentAction.parent);
}

/**
 * Prepare the manager's completion to run the conclusion on the initiator
 * thread.
 *
 * @param manager  The action manager
 **/
static void prepareForConclusion(ActionManager *manager)
{
  prepareForRequeue(&manager->completion, finishActionCallback,
                    handleZoneError, manager->initiatorThreadID,
                    manager->currentAction.parent);
}

/**
 * Perform an action on the next zone if there is one.
 *
 * @param completion  The action completion
 **/
static void applyToZone(VDOCompletion *completion)
{
  ActionManager *manager = asActionManager(completion);
  ASSERT_LOG_ONLY((getCallbackThreadID() == getActingZoneThreadID(manager)),
                  "applyToZone() called on acting zones's thread");

  ZoneCount zone = manager->actingZone++;
  if (manager->actingZone == manager->zones) {
    // We are about to apply to the last zone. Once that is finished,
    // we're done, so go back to the initiator thread and finish up.
    prepareForConclusion(manager);
  } else {
    // Prepare to come back on the next zone
    prepareForNextZone(manager);
  }

  manager->currentAction.action(manager->context, zone, completion);
}

/**
 * Launch the current action.
 *
 * @param manager  The action manager
 **/
static void launchCurrentAction(ActionManager *manager)
{
  manager->acting     = true;
  manager->actingZone = 0;
  if (manager->currentAction.action == NULL) {
    prepareForConclusion(manager);
  } else {
    prepareForNextZone(manager);
  }

  invokeCallback(&manager->completion);
}

/**
 * Launch the next action if there isn't one currently in progress.
 *
 * @param manager  The action manager
 **/
static void launchNextAction(ActionManager *manager)
{
  if (manager->acting || !hasNextAction(manager)) {
    return;
  }

  manager->currentAction = manager->nextAction;
  manager->nextAction = (Action) {
    .action     = NULL,
    .conclusion = NULL,
    .parent     = NULL,
  };

  launchCurrentAction(manager);
}

/**
 * Finish an action now that it has been applied to all zones. This
 * callback is registered in applyToZone().
 *
 * @param completion  The action manager completion
 **/
static void finishActionCallback(VDOCompletion *completion)
{
  ActionManager *manager            = asActionManager(completion);
  Action         action             = manager->currentAction;
  manager->currentAction.action     = NULL;
  manager->currentAction.conclusion = NULL;

  if (action.conclusion != NULL) {
    action.conclusion(manager->context);
  }

  if ((manager->currentAction.action != NULL)
      || (manager->currentAction.conclusion != NULL)) {
    // If currentAction's action or conclusion are no longer NULL,
    // the conclusion must have continued the action, so keep going.
    launchCurrentAction(manager);
    return;
  }

  manager->acting = false;

  // This will requeue so we'll be back momentarily.
  launchNextAction(manager);

  if (action.parent != NULL) {
    completeCompletion(action.parent);
  }
}

/**********************************************************************/
void scheduleAction(ActionManager    *manager,
                    ZoneAction       *action,
                    ActionConclusion *conclusion,
                    VDOCompletion    *parent)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == manager->initiatorThreadID),
                  "action initiated from correct thread");
  if (hasNextAction(manager)) {
    finishCompletion(parent, VDO_COMPONENT_BUSY);
    return;
  }

  manager->nextAction = (Action) {
    .action     = action,
    .conclusion = conclusion,
    .parent     = parent,
  };
  launchNextAction(manager);
}

/**********************************************************************/
void continueAction(ActionManager    *manager,
                    ZoneAction       *action,
                    ActionConclusion *conclusion)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == manager->initiatorThreadID),
                  "action initiated from correct thread");

  manager->currentAction.action     = action;
  manager->currentAction.conclusion = conclusion;
}
