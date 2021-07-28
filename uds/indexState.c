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
 * $Id: //eng/uds-releases/krusty/src/uds/indexState.c#23 $
 */

#include "indexState.h"

#include "errors.h"
#include "indexComponent.h"
#include "indexLayout.h"
#include "logger.h"
#include "memoryAlloc.h"


/**********************************************************************/
int make_index_state(struct index_layout *layout,
		     unsigned int num_zones,
		     unsigned int max_components,
		     struct index_state **state_ptr)
{
	struct index_state *state = NULL;
	int result;

	if (max_components == 0) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "cannot make index state with max_components 0");
	}

	result = UDS_ALLOCATE_EXTENDED(struct index_state,
				       max_components,
				       struct index_component *,
				       "index state",
				       &state);
	if (result != UDS_SUCCESS) {
		return result;
	}

	state->count = 0;
	state->layout = layout;
	state->length = max_components;
	state->load_zones = 0;
	state->load_slot = UINT_MAX;
	state->save_slot = UINT_MAX;
	state->saving = false;
	state->zone_count = num_zones;

	*state_ptr = state;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_index_state(struct index_state *state)
{
	unsigned int i;

	if (state == NULL) {
		return;
	}

	for (i = 0; i < state->count; ++i) {
		free_index_component(UDS_FORGET(state->entries[i]));
	}
	UDS_FREE(state);
}

/**********************************************************************/
/**
 * Add a component to the index state.
 *
 * @param state         The index state.
 * @param component     The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
static int add_component_to_index_state(struct index_state *state,
					struct index_component *component)
{
	if (find_index_component(state, component->info) != NULL) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "cannot add state component %s: already present",
					      component->info->name);
	}

	if (state->count >= state->length) {
		return uds_log_error_strerror(UDS_RESOURCE_LIMIT_EXCEEDED,
					      "cannot add state component %s, %u components already added",
					      component->info->name,
					      state->count);
	}

	state->entries[state->count] = component;
	++state->count;
	return UDS_SUCCESS;
}

/**********************************************************************/
int add_index_state_component(struct index_state *state,
			      const struct index_component_info *info,
			      void *data,
			      void *context)
{
	struct index_component *component = NULL;
	int result = make_index_component(state, info, state->zone_count, data,
					  context, &component);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot make region index component");
	}

	result = add_component_to_index_state(state, component);
	if (result != UDS_SUCCESS) {
		free_index_component(component);
		return result;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
struct index_component *
find_index_component(const struct index_state *state,
		     const struct index_component_info *info)
{
	unsigned int i;
	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		if (info == component->info) {
			return component;
		}
	}
	return NULL;
}

/**********************************************************************/
static const char *index_save_type_name(enum index_save_type save_type)
{
	return save_type == IS_SAVE ? "save" : "checkpoint";
}

/**********************************************************************/
int load_index_state(struct index_state *state, bool *replay_ptr)
{
	bool replay_required = false;
	unsigned int i;
	int result = find_latest_uds_index_save_slot(state->layout,
						     &state->load_zones,
						     &state->load_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		result = read_index_component(component);
		if (result != UDS_SUCCESS) {
			if (!missing_index_component_requires_replay(component)) {
				state->load_zones = 0;
				state->load_slot = UINT_MAX;
				return uds_log_error_strerror(result,
							      "index component %s",
							      index_component_name(component));
			}
			replay_required = true;
		}
	}

	state->load_zones = 0;
	state->load_slot = UINT_MAX;
	if (replay_ptr != NULL) {
		*replay_ptr = replay_required;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int prepare_to_save_index_state(struct index_state *state,
				enum index_save_type save_type)
{
	int result;

	if (state->saving) {
		return uds_log_error_strerror(
			UDS_BAD_STATE, "already saving the index state");
	}
	result = setup_uds_index_save_slot(state->layout, state->zone_count,
					   save_type, &state->save_slot);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot prepare index %s",
					      index_save_type_name(save_type));
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
/**
 *  Complete the saving of an index state.
 *
 *  @param state  the index state
 *
 *  @return UDS_SUCCESS or an error code
 **/
static int complete_index_saving(struct index_state *state)
{
	int result;
	state->saving = false;
	result = commit_uds_index_save(state->layout, state->save_slot);
	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot commit index state");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
static int cleanup_save(struct index_state *state)
{
	int result = cancel_uds_index_save(state->layout, state->save_slot);
	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot cancel index save");
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int save_index_state(struct index_state *state)
{
	unsigned int i;
	int result = prepare_to_save_index_state(state, IS_SAVE);
	if (result != UDS_SUCCESS) {
		return result;
	}
	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		result = write_index_component(component);
		if (result != UDS_SUCCESS) {
			cleanup_save(state);
			return result;
		}
	}
	return complete_index_saving(state);
}

/**********************************************************************/
int write_index_state_checkpoint(struct index_state *state)
{
	unsigned int i;
	int result = prepare_to_save_index_state(state, IS_CHECKPOINT);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		if (skip_index_component_on_checkpoint(component)) {
			continue;
		}
		result = write_index_component(component);
		if (result != UDS_SUCCESS) {
			cleanup_save(state);
			return result;
		}
	}

	return complete_index_saving(state);
}

/**********************************************************************/
int start_index_state_checkpoint(struct index_state *state)
{
	unsigned int i;
	int result = prepare_to_save_index_state(state, IS_CHECKPOINT);
	if (result != UDS_SUCCESS) {
		return result;
	}

	state->saving = true;

	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		if (skip_index_component_on_checkpoint(component)) {
			continue;
		}
		result = start_index_component_incremental_save(component);
		if (result != UDS_SUCCESS) {
			abort_index_state_checkpoint(state);
			return result;
		}
	}

	return result;
}

/**********************************************************************/
int perform_index_state_checkpoint_chapter_synchronized_saves(struct index_state *state)
{
	unsigned int i;
	int result;
	if (!state->saving) {
		return UDS_SUCCESS;
	}

	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		if (skip_index_component_on_checkpoint(component) ||
		    !defer_index_component_checkpoint_to_chapter_writer(component)) {
			continue;
		}
		result =
			perform_index_component_chapter_writer_save(component);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 *  Wrapper function to do a zone-based checkpoint operation.
 *
 *  @param [in]  state          the index state
 *  @param [in]  zone           the zone number
 *  @param [in]  comp_func      the index component function to use
 *  @param [out] completed      if non-NULL, where to save the completion status
 *
 *  @return UDS_SUCCESS or an error code
 *
 **/
static int
do_index_state_checkpoint_in_zone(struct index_state *state,
				  unsigned int zone,
				  int (*comp_func)(struct index_component *,
				  		   unsigned int,
			 			   enum completion_status *),
				  enum completion_status *completed)
{
	enum completion_status status = CS_COMPLETED_PREVIOUSLY;
	unsigned int i;

	if (!state->saving) {
		if (completed != NULL) {
			*completed = CS_COMPLETED_PREVIOUSLY;
		}
		return UDS_SUCCESS;
	}

	for (i = 0; i < state->count; ++i) {
		enum completion_status component_status = CS_NOT_COMPLETED;
		struct index_component *component = state->entries[i];
		int result;
		if (skip_index_component_on_checkpoint(component)) {
			continue;
		}
		if (zone > 0 && !component->info->multi_zone) {
			continue;
		}
		result = (*comp_func)(component, zone, &component_status);
		if (result != UDS_SUCCESS) {
			return result;
		}
		// compute rolling least status
		if (component_status < status) {
			status = component_status;
		}
	}

	if (completed != NULL) {
		*completed = status;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int perform_index_state_checkpoint_in_zone(struct index_state *state,
					   unsigned int zone,
					   enum completion_status *completed)
{
	return do_index_state_checkpoint_in_zone(state, zone,
						 &perform_index_component_zone_save,
						 completed);
}

/**********************************************************************/
int finish_index_state_checkpoint_in_zone(struct index_state *state,
					  unsigned int zone,
					  enum completion_status *completed)
{
	return do_index_state_checkpoint_in_zone(state, zone,
						 &finish_index_component_zone_save,
						 completed);
}

/**********************************************************************/
int abort_index_state_checkpoint_in_zone(struct index_state *state,
					 unsigned int zone,
					 enum completion_status *completed)
{
	return do_index_state_checkpoint_in_zone(state, zone,
						 &abort_index_component_zone_save,
						 completed);
}

/**********************************************************************/
int finish_index_state_checkpoint(struct index_state *state)
{
	unsigned int i;
	int result;

	if (!state->saving) {
		return UDS_SUCCESS;
	}

	for (i = 0; i < state->count; ++i) {
		struct index_component *component = state->entries[i];
		if (skip_index_component_on_checkpoint(component)) {
			continue;
		}
		result = finish_index_component_incremental_save(component);
		if (result != UDS_SUCCESS) {
			abort_index_state_checkpoint(state);
			return result;
		}
	}

	result = complete_index_saving(state);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int abort_index_state_checkpoint(struct index_state *state)
{
	int result = UDS_SUCCESS;
	unsigned int i;
	if (!state->saving) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "not saving the index state");
	}

	uds_log_error("aborting index state checkpoint");

	for (i = 0; i < state->count; ++i) {
		int tmp;
		struct index_component *component = state->entries[i];
		if (skip_index_component_on_checkpoint(component)) {
			continue;
		}
		tmp = abort_index_component_incremental_save(component);
		if (result == UDS_SUCCESS) {
			result = tmp;
		}
	}

	cleanup_save(state);
	state->saving = false;

	return result;
}

/**********************************************************************/
int discard_index_state_data(struct index_state *state)
{
	int result = discard_uds_index_saves(state->layout, true);
	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "%s: cannot destroy all index saves",
					      __func__);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int discard_last_index_state_save(struct index_state *state)
{
	int result = discard_uds_index_saves(state->layout, false);
	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "%s: cannot destroy latest index save",
					      __func__);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
struct buffer *get_state_index_state_buffer(struct index_state *state,
					    enum io_access_mode  mode)
{
	unsigned int slot =
		mode == IO_READ ? state->load_slot : state->save_slot;
	return get_uds_index_state_buffer(state->layout, slot);
}

/**********************************************************************/
int open_state_buffered_reader(struct index_state   *state,
			       enum region_kind         kind,
			       unsigned int             zone,
			       struct buffered_reader **reader_ptr)
{
	return open_uds_index_buffered_reader(state->layout, state->load_slot,
					      kind, zone, reader_ptr);
}

/**********************************************************************/
int open_state_buffered_writer(struct index_state *state,
			       enum region_kind kind,
			       unsigned int zone,
			       struct buffered_writer **writer_ptr)
{
	return open_uds_index_buffered_writer(state->layout, state->save_slot,
					      kind, zone, writer_ptr);
}
