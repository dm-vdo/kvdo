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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/actionManager.h#1 $
 */

#ifndef ACTION_MANAGER_H
#define ACTION_MANAGER_H

#include "adminState.h"
#include "completion.h"
#include "types.h"

/**
 * An action_manager provides a generic mechanism for applying actions to
 * multi-zone entities (such as the block map or slab depot). Each action
 * manager is tied to a specific context for which it manages actions. The
 * manager ensures that only one action is active on that context at a time,
 * and supports at most one pending action. Calls to schedule an action when
 * there is already a pending action will result in VDO_COMPONENT_BUSY errors.
 * Actions may only be submitted to the action manager from a single thread
 * (which thread is determined when the action manager is constructed).
 *
 * A scheduled action consists of four components:
 *   preamble:    an optional method to be run on the initator thread before
 *                applying the action to all zones
 *   zone_action: an optional method to be applied to each of the zones
 *   conclusion:  an optional method to be run on the initiator thread once the
 *                per-zone method has been applied to all zones
 *   parent:      an optional completion to be finished once the conclusion
 *                is done
 *
 * At least one of the three methods must be provided.
 **/

/**
 * A function which is to be applied asynchronously to a set of zones.
 *
 * @param context      The object which holds the per-zone context for the
 *                     action
 * @param zone_number  The number of zone to which the action is being applied
 * @param parent       The object to notify when the action is complete
 **/
typedef void vdo_zone_action(void *context,
			     zone_count_t zone_number,
			     struct vdo_completion *parent);

/**
 * A function which is to be applied asynchronously on an action manager's
 * initiator thread as the preamble of an action.
 *
 * @param context  The object which holds the per-zone context for the action
 * @param parent   The object to notify when the action is complete
 **/
typedef void vdo_action_preamble(void *context, struct vdo_completion *parent);

/**
 * A function which will run on the action manager's initiator thread as the
 * conclusion of an action.
 *
 * @param context  The object which holds the per-zone context for the action
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int vdo_action_conclusion(void *context);

/**
 * A function to schedule an action.
 *
 * @param context  The object which holds the per-zone context for the action
 *
 * @return <code>true</code> if an action was scheduled
 **/
typedef bool vdo_action_scheduler(void *context);

/**
 * Get the id of the thread associated with a given zone.
 *
 * @param context      The action context
 * @param zone_number  The number of the zone for which the thread ID is desired
 **/
typedef thread_id_t vdo_zone_thread_getter(void *context, zone_count_t zone_number);

/**
 * Make an action manager.
 *
 * @param [in]  zones                The number of zones to which actions will
 *                                   be applied
 * @param [in]  get_zone_thread_id   A function to get the thread id associated
 *                                   with a zone
 * @param [in]  initiator_thread_id  The thread on which actions may initiated
 * @param [in]  context              The object which holds the per-zone context
 *                                   for the action
 * @param [in]  scheduler            A function to schedule a next action after
 *                                   an action concludes if there is no pending
 *                                   action (may be NULL)
 * @param [in]  vdo                  The vdo used to initialize completions
 * @param [out] manager_ptr          A pointer to hold the new action manager
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check make_vdo_action_manager(zone_count_t zones,
					 vdo_zone_thread_getter *get_zone_thread_id,
					 thread_id_t initiator_thread_id,
					 void *context,
					 vdo_action_scheduler *scheduler,
					 struct vdo *vdo,
					 struct action_manager **manager_ptr);

/**
 * Destroy an action manager and null out the reference to it.
 *
 * @param manager_ptr  The reference to the manager to destroy
 **/
void free_vdo_action_manager(struct action_manager **manager_ptr);

/**
 * Get the current operation an action manager is performing.
 *
 * @param manager  The manager to query
 *
 * @return The manager's current operation
 **/
enum admin_state_code __must_check
get_current_vdo_manager_operation(struct action_manager *manager);

/**
 * Get the action-specific context for the operation an action manager is
 * currently performing.
 *
 * @param manager  The manager to query
 *
 * @return The action-specific context for the manager's current action or
 *         NULL if there is no context or no current action
 **/
void * __must_check get_current_vdo_action_context(struct action_manager *manager);

/**
 * Attempt to schedule the default action. If the manager is not operating
 * normally, the action will not be scheduled.
 *
 * @param manager  The action manager
 *
 * @return <code>true</code> if an action was scheduled.
 **/
bool schedule_vdo_default_action(struct action_manager *manager);

/**
 * Schedule an action to be applied to all zones. The action will be launched
 * immediately if there is no current action, or as soon as the current action
 * completes. If there is already a pending action, this action will not be
 * scheduled, and, if it has a parent, that parent will be notified. At least
 * one of the preamble, action, or conclusion must not be NULL.
 *
 * @param manager     The action manager to schedule the action on
 * @param preamble    A method to be invoked on the initiator thread once this
 *                    action is started but before applying to each zone; may
 *                    be NULL
 * @param action      The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param parent      The object to notify once the action is complete or if
 *                    the action can not be scheduled; may be NULL
 *
 * @return <code>true</code> if the action was scheduled
 **/
bool schedule_vdo_action(struct action_manager *manager,
			 vdo_action_preamble *preamble,
			 vdo_zone_action *action,
			 vdo_action_conclusion *conclusion,
			 struct vdo_completion *parent);

/**
 * Schedule an operation to be applied to all zones. The operation's action
 * will be launched immediately if there is no current action, or as soon as
 * the current action completes. If there is already a pending action, this
 * operation will not be scheduled, and, if it has a parent, that parent will
 * be notified. At least one of the preamble, action, or conclusion must not
 * be NULL.
 *
 * @param manager     The action manager to schedule the action on
 * @param operation   The operation this action will perform
 * @param preamble    A method to be invoked on the initiator thread once this
 *                    action is started but before applying to each zone; may
 *                    be NULL
 * @param action      The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param parent      The object to notify once the action is complete or if
 *                    the action can not be scheduled; may be NULL
 *
 * @return <code>true</code> if the action was scheduled
 **/
bool schedule_vdo_operation(struct action_manager *manager,
			    enum admin_state_code operation,
			    vdo_action_preamble *preamble,
			    vdo_zone_action *action,
			    vdo_action_conclusion *conclusion,
			    struct vdo_completion *parent);

/**
 * Schedule an operation to be applied to all zones. The operation's action
 * will be launched immediately if there is no current action, or as soon as
 * the current action completes. If there is already a pending action, this
 * operation will not be scheduled, and, if it has a parent, that parent will
 * be notified. At least one of the preamble, action, or conclusion must not
 * be NULL.
 *
 * @param manager     The action manager to schedule the action on
 * @param operation   The operation this action will perform
 * @param preamble    A method to be invoked on the initiator thread once this
 *                    action is started but before applying to each zone; may
 *                    be NULL
 * @param action      The action to apply to each zone; may be NULL
 * @param conclusion  A method to be invoked back on the initiator thread once
 *                    the action has been applied to all zones; may be NULL
 * @param context     An action-specific context which may be retrieved via
 *                    get_current_vdo_action_context(); may be NULL
 * @param parent      The object to notify once the action is complete or if
 *                    the action can not be scheduled; may be NULL
 *
 * @return <code>true</code> if the action was scheduled
 **/
bool schedule_vdo_operation_with_context(struct action_manager *manager,
					 enum admin_state_code operation,
					 vdo_action_preamble *preamble,
					 vdo_zone_action *action,
					 vdo_action_conclusion *conclusion,
					 void *context,
					 struct vdo_completion *parent);

#endif // ACTION_MANAGER_H
