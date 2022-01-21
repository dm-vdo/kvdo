/*
 * Copyright Red Hat
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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/actionManager.c#14 $
 */

#include "actionManager.h"

#include "memoryAlloc.h"
#include "permassert.h"

#include "adminState.h"
#include "completion.h"
#include "statusCodes.h"
#include "types.h"
#include "vdo.h"

/** An action to be performed in each of a set of zones */
struct action {
	/** Whether this structure is in use */
	bool in_use;
	/** The admin operation associated with this action */
	const struct admin_state_code *operation;
	/**
	 * The method to run on the initiator thread before the action is
	 * applied to each zone.
	 **/
	vdo_action_preamble *preamble;
	/** The action to be performed in each zone */
	vdo_zone_action *zone_action;
	/**
	 * The method to run on the initiator thread after the action has been
	 * applied to each zone
	 **/
	vdo_action_conclusion *conclusion;
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
	zone_count_t zones;
	/** A function to schedule a default next action */
	vdo_action_scheduler *scheduler;
	/**
	 * A function to get the id of the thread on which to apply an action
	 * to a zone
	 **/
	vdo_zone_thread_getter *get_zone_thread_id;
	/** The ID of the thread on which actions may be initiated */
	thread_id_t initiator_thread_id;
	/** Opaque data associated with this action manager */
	void *context;
	/** The zone currently being acted upon */
	zone_count_t acting_zone;
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
	assert_vdo_completion_type(completion->type, VDO_ACTION_COMPLETION);
	return container_of(completion, struct action_manager, completion);
}

/**
 * An action scheduler which does not schedule an action.
 *
 * <p>Implements vdo_action_scheduler.
 **/
static bool no_default_action(void *context __always_unused)
{
	return false;
}

/**
 * A default preamble which does nothing.
 *
 * <p>Implements vdo_action_preamble
 **/
static void no_preamble(void *context __always_unused,
			struct vdo_completion *completion)
{
	complete_vdo_completion(completion);
}

/**
 * A default conclusion which does nothing.
 *
 * <p>Implements vdo_action_conclusion.
 **/
static int no_conclusion(void *context __always_unused)
{
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_vdo_action_manager(zone_count_t zones,
			    vdo_zone_thread_getter *get_zone_thread_id,
			    thread_id_t initiator_thread_id,
			    void *context,
			    vdo_action_scheduler *scheduler,
			    struct vdo *vdo,
			    struct action_manager **manager_ptr)
{
	struct action_manager *manager;
	int result = UDS_ALLOCATE(1, struct action_manager, __func__, &manager);
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
	set_vdo_admin_state_code(&manager->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);
	initialize_vdo_completion(&manager->completion, vdo,
				  VDO_ACTION_COMPLETION);
	*manager_ptr = manager;
	return VDO_SUCCESS;
}

/**********************************************************************/
const struct admin_state_code *
get_current_vdo_manager_operation(struct action_manager *manager)
{
	return get_vdo_admin_state_code(&manager->state);
}

/**********************************************************************/
void *get_current_vdo_action_context(struct action_manager *manager)
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
static thread_id_t get_acting_zone_thread_id(struct action_manager *manager)
{
	return manager->get_zone_thread_id(manager->context,
					   manager->acting_zone);
}

/**
 * Prepare the manager's completion to run on the next zone.
 *
 * @param manager  The action manager
 **/
static void prepare_for_next_zone(struct action_manager *manager)
{
	prepare_vdo_completion_for_requeue(&manager->completion,
					   apply_to_zone,
					   preserve_vdo_completion_error_and_continue,
					   get_acting_zone_thread_id(manager),
					   manager->current_action->parent);
}

/**
 * Prepare the manager's completion to run the conclusion on the initiator
 * thread.
 *
 * @param manager  The action manager
 **/
static void prepare_for_conclusion(struct action_manager *manager)
{
	prepare_vdo_completion_for_requeue(&manager->completion,
					   finish_action_callback,
					   preserve_vdo_completion_error_and_continue,
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
	zone_count_t zone;
	struct action_manager *manager = as_action_manager(completion);
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 get_acting_zone_thread_id(manager)),
			"apply_to_zone() called on acting zones's thread");

	zone = manager->acting_zone++;
	if (manager->acting_zone == manager->zones) {
		// We are about to apply to the last zone. Once that is
		// finished, we're done, so go back to the initiator thread and
		// finish up.
		prepare_for_conclusion(manager);
	} else {
		// Prepare to come back on the next zone
		prepare_for_next_zone(manager);
	}

	manager->current_action->zone_action(manager->context, zone, completion);
}

/**
 * The error handler for preamble errors.
 *
 * @param completion  The manager completion
 **/
static void handle_preamble_error(struct vdo_completion *completion)
{
	// Skip the zone actions since the preamble failed.
	completion->callback = finish_action_callback;
	preserve_vdo_completion_error_and_continue(completion);
}

/**
 * Launch the current action.
 *
 * @param manager  The action manager
 **/
static void launch_current_action(struct action_manager *manager)
{
	struct action *action = manager->current_action;
	int result = start_vdo_operation(&manager->state, action->operation);
	if (result != VDO_SUCCESS) {
		if (action->parent != NULL) {
			set_vdo_completion_result(action->parent, result);
		}

		// We aren't going to run the preamble, so don't run the
		// conclusion
		action->conclusion = no_conclusion;
		finish_action_callback(&manager->completion);
		return;
	}

	if (action->zone_action == NULL) {
		prepare_for_conclusion(manager);
	} else {
		manager->acting_zone = 0;
		prepare_vdo_completion_for_requeue(&manager->completion,
						   apply_to_zone,
						   handle_preamble_error,
						   get_acting_zone_thread_id(manager),
						   manager->current_action->parent);
	}

	action->preamble(manager->context, &manager->completion);
}

/**********************************************************************/
bool schedule_vdo_default_action(struct action_manager *manager)
{
	// Don't schedule a default action if we are operating or not in normal
	// operation.
	const struct admin_state_code *code
		= get_current_vdo_manager_operation(manager);
	return ((code == VDO_ADMIN_STATE_NORMAL_OPERATION)
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
	bool has_next_action;
	int result;
	struct action_manager *manager = as_action_manager(completion);
	struct action action = *(manager->current_action);
	manager->current_action->in_use = false;
	manager->current_action = manager->current_action->next;

	/*
	 * We need to check this now to avoid use-after-free issues if running
	 * the conclusion or notifying the parent results in the manager being
	 * freed.
	 */
	has_next_action = (manager->current_action->in_use
			   || schedule_vdo_default_action(manager));
	result = action.conclusion(manager->context);
	finish_vdo_operation(&manager->state, VDO_SUCCESS);
	if (action.parent != NULL) {
		finish_vdo_completion(action.parent, result);
	}

	if (has_next_action) {
		launch_current_action(manager);
	}
}

/**********************************************************************/
bool schedule_vdo_action(struct action_manager *manager,
			 vdo_action_preamble *preamble,
			 vdo_zone_action *action,
			 vdo_action_conclusion *conclusion,
			 struct vdo_completion *parent)
{
	return schedule_vdo_operation(manager,
				      VDO_ADMIN_STATE_OPERATING,
				      preamble,
				      action,
				      conclusion,
				      parent);
}

/**********************************************************************/
bool schedule_vdo_operation(struct action_manager *manager,
			    const struct admin_state_code *operation,
			    vdo_action_preamble *preamble,
			    vdo_zone_action *action,
			    vdo_action_conclusion *conclusion,
			    struct vdo_completion *parent)
{
	return schedule_vdo_operation_with_context(manager,
						   operation,
						   preamble,
						   action,
						   conclusion,
						   NULL,
						   parent);
}

/**********************************************************************/
bool
schedule_vdo_operation_with_context(struct action_manager *manager,
				    const struct admin_state_code *operation,
				    vdo_action_preamble *preamble,
				    vdo_zone_action *action,
				    vdo_action_conclusion *conclusion,
				    void *context,
				    struct vdo_completion *parent)
{
	struct action *current_action;
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 manager->initiator_thread_id),
			"action initiated from correct thread");
	if (!manager->current_action->in_use) {
		current_action = manager->current_action;
	} else if (!manager->current_action->next->in_use) {
		current_action = manager->current_action->next;
	} else {
		if (parent != NULL) {
			finish_vdo_completion(parent, VDO_COMPONENT_BUSY);
		}

		return false;
	}

	*current_action = (struct action) {
		.in_use = true,
		.operation = operation,
		.preamble = (preamble == NULL) ? no_preamble : preamble,
		.zone_action = action,
		.conclusion = (conclusion == NULL) ? no_conclusion : conclusion,
		.context = context,
		.parent = parent,
		.next = current_action->next,
	};

	if (current_action == manager->current_action) {
		launch_current_action(manager);
	}

	return true;
}
