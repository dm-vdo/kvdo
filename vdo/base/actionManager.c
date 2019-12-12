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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/actionManager.c#11 $
 */

#include "actionManager.h"

#include "memoryAlloc.h"

#include "adminState.h"
#include "completion.h"
#include "types.h"

/** An action to be performed in each of a set of zones */
struct action {
  /** Whether this structure is in use */
  bool                      inUse;
  /** The admin operation associated with this action */
  AdminStateCode            operation;
  /**
   * The method to run on the initiator thread before the action is applied to
   * each zone.
   **/
  ActionPreamble           *preamble;
  /** The action to be performed in each zone */
  ZoneAction               *zoneAction;
  /**
   * The method to run on the initiator thread after the action has been
   * applied to each zone
   **/
  ActionConclusion         *conclusion;
  /** The object to notify when the action is complete */
  struct vdo_completion    *parent;
  /** The action specific context */
  void                     *context;
  /** The action to perform after this one */
  struct action            *next;
};

struct action_manager {
  /** The completion for performing actions */
  struct vdo_completion      completion;
  /** The state of this action manager */
  struct admin_state         state;
  /** The two action slots*/
  struct action              actions[2];
  /** The current action slot */
  struct action             *currentAction;
  /** The number of zones in which an action is to be applied */
  ZoneCount                  zones;
  /** A function to schedule a default next action */
  ActionScheduler           *scheduler;
  /**
   * A function to get the id of the thread on which to apply an action to a
   * zone
   **/
  ZoneThreadGetter          *getZoneThreadID;
  /** The ID of the thread on which actions may be initiated */
  ThreadID                   initiatorThreadID;
  /** Opaque data associated with this action manager */
  void                      *context;
  /** The zone currently being acted upon */
  ZoneCount                  actingZone;
};

/**
 * Convert a generic vdo_completion to a action_manager.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a action_manager
 **/
static inline struct action_manager *
asActionManager(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct action_manager, completion) == 0);
  assertCompletionType(completion->type, ACTION_COMPLETION);
  return (struct action_manager *) completion;
}

/**
 * An action scheduler which does not schedule an action.
 *
 * <p>Implements ActionScheduler.
 **/
static bool noDefaultAction(void *context __attribute__((unused)))
{
  return false;
}

/**
 * A default preamble which does nothing.
 *
 * <p>Implements ActionPreamble
 **/
static void noPreamble(void                  *context __attribute__((unused)),
                       struct vdo_completion *completion)
{
  completeCompletion(completion);
}

/**
 * A default conclusion which does nothing.
 *
 * <p>Implements ActionConclusion.
 **/
static int noConclusion(void *context __attribute__((unused))) {
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeActionManager(ZoneCount               zones,
                      ZoneThreadGetter       *getZoneThreadID,
                      ThreadID                initiatorThreadID,
                      void                   *context,
                      ActionScheduler        *scheduler,
                      PhysicalLayer          *layer,
                      struct action_manager **managerPtr)
{
  struct action_manager *manager;
  int result = ALLOCATE(1, struct action_manager, __func__, &manager);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *manager = (struct action_manager) {
    .zones             = zones,
    .scheduler         = ((scheduler == NULL) ? noDefaultAction : scheduler),
    .getZoneThreadID   = getZoneThreadID,
    .initiatorThreadID = initiatorThreadID,
    .context           = context,
  };

  manager->actions[0].next = &manager->actions[1];
  manager->currentAction = manager->actions[1].next = &manager->actions[0];

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
void freeActionManager(struct action_manager **managerPtr)
{
  struct action_manager *manager = *managerPtr;
  if (manager == NULL) {
    return;
  }

  destroyEnqueueable(&manager->completion);
  FREE(manager);
  *managerPtr = NULL;
}

/**********************************************************************/
AdminStateCode getCurrentManagerOperation(struct action_manager *manager)
{
  return manager->state.state;
}

/**********************************************************************/
void *getCurrentActionContext(struct action_manager *manager)
{
  return (manager->currentAction->inUse
          ? manager->currentAction->context : NULL);
}

/**********************************************************************/
static void finishActionCallback(struct vdo_completion *completion);
static void applyToZone(struct vdo_completion *completion);

/**
 * Get the thread ID for the current zone.
 *
 * @param manager  The action manager
 *
 * @return The ID of the thread on which to run actions for the current zone
 **/
static ThreadID getActingZoneThreadID(struct action_manager *manager)
{
  return manager->getZoneThreadID(manager->context, manager->actingZone);
}

/**
 * Prepare the manager's completion to run on the next zone.
 *
 * @param manager  The action manager
 **/
static void prepareForNextZone(struct action_manager *manager)
{
  prepareForRequeue(&manager->completion, applyToZone,
                    preserveErrorAndContinue, getActingZoneThreadID(manager),
                    manager->currentAction->parent);
}

/**
 * Prepare the manager's completion to run the conclusion on the initiator
 * thread.
 *
 * @param manager  The action manager
 **/
static void prepareForConclusion(struct action_manager *manager)
{
  prepareForRequeue(&manager->completion, finishActionCallback,
                    preserveErrorAndContinue, manager->initiatorThreadID,
                    manager->currentAction->parent);
}

/**
 * Perform an action on the next zone if there is one.
 *
 * @param completion  The action completion
 **/
static void applyToZone(struct vdo_completion *completion)
{
  struct action_manager *manager = asActionManager(completion);
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

  manager->currentAction->zoneAction(manager->context, zone, completion);
}

/**
 * The error handler for preamble errors.
 *
 * @param completion  The manager completion
 **/
static void handlePreambleError(struct vdo_completion *completion)
{
  // Skip the zone actions since the preamble failed.
  completion->callback = finishActionCallback;
  preserveErrorAndContinue(completion);
}

/**
 * Launch the current action.
 *
 * @param manager  The action manager
 **/
static void launchCurrentAction(struct action_manager *manager)
{
  struct action *action = manager->currentAction;
  int     result = startOperation(&manager->state, action->operation);
  if (result != VDO_SUCCESS) {
    if (action->parent != NULL) {
      setCompletionResult(action->parent, result);
    }

    // We aren't going to run the preamble, so don't run the conclusion
    action->conclusion = noConclusion;
    finishActionCallback(&manager->completion);
    return;
  }

  if (action->zoneAction == NULL) {
    prepareForConclusion(manager);
  } else {
    manager->actingZone = 0;
    prepareForRequeue(&manager->completion, applyToZone, handlePreambleError,
                      getActingZoneThreadID(manager),
                      manager->currentAction->parent);
  }

  action->preamble(manager->context, &manager->completion);
}

/**
 * Finish an action now that it has been applied to all zones. This
 * callback is registered in applyToZone().
 *
 * @param completion  The action manager completion
 **/
static void finishActionCallback(struct vdo_completion *completion)
{
  struct action_manager *manager  = asActionManager(completion);
  struct action          action   = *(manager->currentAction);
  manager->currentAction->inUse   = false;
  manager->currentAction          = manager->currentAction->next;

  // We need to check this now to avoid use-after-free issues if running the
  // conclusion or notifying the parent results in the manager being freed.
  bool hasNextAction = (manager->currentAction->inUse
                        || manager->scheduler(manager->context));
  int result = action.conclusion(manager->context);
  finishOperation(&manager->state);
  if (action.parent != NULL) {
    finishCompletion(action.parent, result);
  }

  if (hasNextAction) {
    launchCurrentAction(manager);
  }
}

/**********************************************************************/
bool scheduleAction(struct action_manager *manager,
                    ActionPreamble        *preamble,
                    ZoneAction            *zoneAction,
                    ActionConclusion      *conclusion,
                    struct vdo_completion *parent)
{
  return scheduleOperation(manager, ADMIN_STATE_OPERATING, preamble,
                           zoneAction, conclusion, parent);
}

/**********************************************************************/
bool scheduleOperation(struct action_manager *manager,
                       AdminStateCode         operation,
                       ActionPreamble        *preamble,
                       ZoneAction            *zoneAction,
                       ActionConclusion      *conclusion,
                       struct vdo_completion *parent)
{
  return scheduleOperationWithContext(manager, operation, preamble, zoneAction,
                                      conclusion, NULL, parent);
}

/**********************************************************************/
bool scheduleOperationWithContext(struct action_manager *manager,
                                  AdminStateCode         operation,
                                  ActionPreamble        *preamble,
                                  ZoneAction            *zoneAction,
                                  ActionConclusion      *conclusion,
                                  void                  *context,
                                  struct vdo_completion *parent)
{
  ASSERT_LOG_ONLY((getCallbackThreadID() == manager->initiatorThreadID),
                  "action initiated from correct thread");
  struct action *action;
  if (!manager->currentAction->inUse) {
    action = manager->currentAction;
  } else if (!manager->currentAction->next->inUse) {
    action = manager->currentAction->next;
  } else {
    if (parent != NULL) {
      finishCompletion(parent, VDO_COMPONENT_BUSY);
    }

    return false;
  }

  *action = (struct action) {
    .inUse      = true,
    .operation  = operation,
    .preamble   = (preamble == NULL) ? noPreamble : preamble,
    .zoneAction = zoneAction,
    .conclusion = (conclusion == NULL) ? noConclusion : conclusion,
    .context    = context,
    .parent     = parent,
    .next       = action->next,
  };

  if (action == manager->currentAction) {
    launchCurrentAction(manager);
  }

  return true;
}
