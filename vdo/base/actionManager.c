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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/actionManager.c#16 $
 */

#include "actionManager.h"

#include "memoryAlloc.h"

#include "adminState.h"
#include "completion.h"
#include "types.h"

/** An action to be performed in each of a set of zones */
struct action {
	/** Whether this structure is in use */
	bool in_use;
	/** The admin operation associated with this action */
	AdminStateCode operation;
	/**
	 * The method to run on the initiator thread before the action is
	 * applied to each zone.
	 **/
	action_preamble *preamble;
	/** The action to be performed in each zone */
	zone_action *zone_action;
	/**
	 * The method to run on the initiator thread after the action has been
	 * applied to each zone
	 **/
	action_conclusion *conclusion;
	/** The object to notify when the action is complete */
	struct vdo_completion *parent;
	/** The action specific context */
	void *context;
	/** The action to perform after this one */
	struct action *next;
};

struct action_manager {
	/** The completion for performing actions */
	struct vdo_completion completion;
	/** The state of this action manager */
	struct admin_state state;
	/** The two action slots */
	struct action actions[2];
	/** The current action slot */
	struct action *current_action;
	/** The number of zones in which an action is to be applied */
	ZoneCount zones;
	/** A function to schedule a default next action */
	action_scheduler *scheduler;
	/**
	 * A function to get the id of the thread on which to apply an action
	 * to a zone
	 **/
	zone_thread_getter *get_zone_thread_id;
	/** The ID of the thread on which actions may be initiated */
	ThreadID initiator_thread_id;
	/** Opaque data associated with this action manager */
	void *context;
	/** The zone currently being acted upon */
	ZoneCount acting_zone;
};

/**
 * Convert a generic vdo_completion to a action_manager.
 *
 * @param completion The completion to convert
 *
 * @return The completion as an action_manager
 **/
static inline struct action_manager *
as_action_manager(struct vdo_completion *completion)
{
	STATIC_ASSERT(offsetof(struct action_manager, completion) == 0);
	assertCompletionType(completion->type, ACTION_COMPLETION);
	return (struct action_manager *) completion;
}

/**
 * An action scheduler which does not schedule an action.
 *
 * <p>Implements action_scheduler.
 **/
static bool no_default_action(void *context __attribute__((unused)))
{
	return false;
}

/**
 * A default preamble which does nothing.
 *
 * <p>Implements action_preamble
 **/
static void no_preamble(void *context __attribute__((unused)),
			struct vdo_completion *completion)
{
	completeCompletion(completion);
}

/**
 * A default conclusion which does nothing.
 *
 * <p>Implements action_conclusion.
 **/
static int no_conclusion(void *context __attribute__((unused)))
{
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_action_manager(ZoneCount zones,
			zone_thread_getter *get_zone_thread_id,
			ThreadID initiator_thread_id,
			void *context,
			action_scheduler *scheduler,
			PhysicalLayer *layer,
			struct action_manager **manager_ptr)
{
	struct action_manager *manager;
	int result = ALLOCATE(1, struct action_manager, __func__, &manager);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*manager = (struct action_manager) {
		.zones = zones,
		.scheduler =
			((scheduler == NULL) ? no_default_action : scheduler),
		.get_zone_thread_id = get_zone_thread_id,
		.initiator_thread_id = initiator_thread_id,
		.context = context,
	};

	manager->actions[0].next = &manager->actions[1];
	manager->current_action = manager->actions[1].next =
		&manager->actions[0];

	result = initializeEnqueueableCompletion(&manager->completion,
						 ACTION_COMPLETION,
						 layer);
	if (result != VDO_SUCCESS) {
		free_action_manager(&manager);
		return result;
	}

	*manager_ptr = manager;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_action_manager(struct action_manager **manager_ptr)
{
	struct action_manager *manager = *manager_ptr;
	if (manager == NULL) {
		return;
	}

	destroyEnqueueable(&manager->completion);
	FREE(manager);
	*manager_ptr = NULL;
}

/**********************************************************************/
AdminStateCode get_current_manager_operation(struct action_manager *manager)
{
	return manager->state.state;
}

/**********************************************************************/
void *get_current_action_context(struct action_manager *manager)
{
	return (manager->current_action->in_use ?
		manager->current_action->context :
		NULL);
}

/**********************************************************************/
static void finish_action_callback(struct vdo_completion *completion);
static void apply_to_zone(struct vdo_completion *completion);

/**
 * Get the thread ID for the current zone.
 *
 * @param manager  The action manager
 *
 * @return The ID of the thread on which to run actions for the current zone
 **/
static ThreadID getActingZoneThreadID(struct action_manager *manager)
{
	return manager->get_zone_thread_id(manager->context,
					   manager->acting_zone);
}

/**
 * Prepare the manager's completion to run on the next zone.
 *
 * @param manager  The action manager
 **/
static void prepareForNextZone(struct action_manager *manager)
{
	prepareForRequeue(&manager->completion,
			  apply_to_zone,
			  preserveErrorAndContinue,
			  getActingZoneThreadID(manager),
			  manager->current_action->parent);
}

/**
 * Prepare the manager's completion to run the conclusion on the initiator
 * thread.
 *
 * @param manager  The action manager
 **/
static void prepareForConclusion(struct action_manager *manager)
{
	prepareForRequeue(&manager->completion,
			  finish_action_callback,
			  preserveErrorAndContinue,
			  manager->initiator_thread_id,
			  manager->current_action->parent);
}

/**
 * Perform an action on the next zone if there is one.
 *
 * @param completion  The action completion
 **/
static void apply_to_zone(struct vdo_completion *completion)
{
	struct action_manager *manager = as_action_manager(completion);
	ASSERT_LOG_ONLY((getCallbackThreadID() ==
			 getActingZoneThreadID(manager)),
			"apply_to_zone() called on acting zones's thread");

	ZoneCount zone = manager->acting_zone++;
	if (manager->acting_zone == manager->zones) {
		// We are about to apply to the last zone. Once that is
		// finished, we're done, so go back to the initiator thread and
		// finish up.
		prepareForConclusion(manager);
	} else {
		// Prepare to come back on the next zone
		prepareForNextZone(manager);
	}

	manager->current_action->zone_action(manager->context, zone, completion);
}

/**
 * The error handler for preamble errors.
 *
 * @param completion  The manager completion
 **/
static void handlePreambleError(struct vdo_completion *completion)
{
	// Skip the zone actions since the preamble failed.
	completion->callback = finish_action_callback;
	preserveErrorAndContinue(completion);
}

/**
 * Launch the current action.
 *
 * @param manager  The action manager
 **/
static void launchCurrentAction(struct action_manager *manager)
{
	struct action *action = manager->current_action;
	int result = start_operation(&manager->state, action->operation);
	if (result != VDO_SUCCESS) {
		if (action->parent != NULL) {
			setCompletionResult(action->parent, result);
		}

		// We aren't going to run the preamble, so don't run the
		// conclusion
		action->conclusion = no_conclusion;
		finish_action_callback(&manager->completion);
		return;
	}

	if (action->zone_action == NULL) {
		prepareForConclusion(manager);
	} else {
		manager->acting_zone = 0;
		prepareForRequeue(&manager->completion,
				  apply_to_zone,
				  handlePreambleError,
				  getActingZoneThreadID(manager),
				  manager->current_action->parent);
	}

	action->preamble(manager->context, &manager->completion);
}

/**
 * Attempt to schedule a next action.
 *
 * @param manager  The action manager
 *
 * @return <code>true</code> if an action was scheduled.
 **/
static bool schedule_default_action(struct action_manager *manager)
{
	// Don't schedule a default action if we are operating or not in normal
	// operation.
	return ((manager->state.state == ADMIN_STATE_NORMAL_OPERATION)
		&& manager->scheduler(manager->context));
}

/**
 * Finish an action now that it has been applied to all zones. This
 * callback is registered in apply_to_zone().
 *
 * @param completion  The action manager completion
 **/
static void finish_action_callback(struct vdo_completion *completion)
{
	struct action_manager *manager = as_action_manager(completion);
	struct action action = *(manager->current_action);
	manager->current_action->in_use = false;
	manager->current_action = manager->current_action->next;

	/*
	 * We need to check this now to avoid use-after-free issues if running
	 * the conclusion or notifying the parent results in the manager being
	 * freed.
	 */
	bool has_next_action = (manager->current_action->in_use
				|| schedule_default_action(manager));
	int result = action.conclusion(manager->context);
	finish_operation(&manager->state);
	if (action.parent != NULL) {
		finishCompletion(action.parent, result);
	}

	if (has_next_action) {
		launchCurrentAction(manager);
	}
}

/**********************************************************************/
bool schedule_action(struct action_manager *manager,
		     action_preamble *preamble,
		     zone_action *zone_action,
		     action_conclusion *conclusion,
		     struct vdo_completion *parent)
{
	return schedule_operation(manager,
				  ADMIN_STATE_OPERATING,
				  preamble,
				  zone_action,
				  conclusion,
				  parent);
}

/**********************************************************************/
bool schedule_operation(struct action_manager *manager,
			AdminStateCode operation,
			action_preamble *preamble,
			zone_action *zone_action,
			action_conclusion *conclusion,
			struct vdo_completion *parent)
{
	return schedule_operation_with_context(manager,
					       operation,
					       preamble,
					       zone_action,
					       conclusion,
					       NULL,
					       parent);
}

/**********************************************************************/
bool schedule_operation_with_context(struct action_manager *manager,
				     AdminStateCode operation,
				     action_preamble *preamble,
				     zone_action *zone_action,
				     action_conclusion *conclusion,
				     void *context,
				     struct vdo_completion *parent)
{
	ASSERT_LOG_ONLY((getCallbackThreadID() == manager->initiator_thread_id),
			"action initiated from correct thread");
	struct action *action;
	if (!manager->current_action->in_use) {
		action = manager->current_action;
	} else if (!manager->current_action->next->in_use) {
		action = manager->current_action->next;
	} else {
		if (parent != NULL) {
			finishCompletion(parent, VDO_COMPONENT_BUSY);
		}

		return false;
	}

	*action = (struct action) {
		.in_use = true,
		.operation = operation,
		.preamble = (preamble == NULL) ? no_preamble : preamble,
		.zone_action = zone_action,
		.conclusion = (conclusion == NULL) ? no_conclusion : conclusion,
		.context = context,
		.parent = parent,
		.next = action->next,
	};

	if (action == manager->current_action) {
		launchCurrentAction(manager);
	}

	return true;
}
