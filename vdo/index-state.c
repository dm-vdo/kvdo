// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "index-state.h"

#include "errors.h"
#include "index-component.h"
#include "index-layout.h"
#include "logger.h"
#include "memory-alloc.h"


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
	state->zone_count = num_zones;

	*state_ptr = state;
	return UDS_SUCCESS;
}

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

int load_index_state(struct index_state *state)
{
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
			state->load_zones = 0;
			state->load_slot = UINT_MAX;
			return uds_log_error_strerror(result,
						      "index component %s",
						      index_component_name(component));
		}
	}

	state->load_zones = 0;
	state->load_slot = UINT_MAX;
	return UDS_SUCCESS;
}

int prepare_to_save_index_state(struct index_state *state)
{
	int result = setup_uds_index_save_slot(state->layout,
					       state->zone_count,
					       &state->save_slot);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot prepare index save");
	}

	return UDS_SUCCESS;
}

/**
 *  Complete the saving of an index state.
 *
 *  @param state  the index state
 *
 *  @return UDS_SUCCESS or an error code
 **/
static int complete_index_saving(struct index_state *state)
{
	int result = commit_uds_index_save(state->layout, state->save_slot);

	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot commit index state");
	}
	return UDS_SUCCESS;
}

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

int save_index_state(struct index_state *state)
{
	unsigned int i;
	int result = prepare_to_save_index_state(state);

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

int discard_index_state_data(struct index_state *state)
{
	int result = discard_uds_index_saves(state->layout);

	state->save_slot = UINT_MAX;
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "%s: cannot destroy all index saves",
					      __func__);
	}
	return UDS_SUCCESS;
}

struct buffer *get_state_index_state_buffer(struct index_state *state,
					    enum io_access_mode  mode)
{
	unsigned int slot =
		mode == IO_READ ? state->load_slot : state->save_slot;
	return get_uds_index_state_buffer(state->layout, slot);
}

int open_state_buffered_reader(struct index_state   *state,
			       enum region_kind         kind,
			       unsigned int             zone,
			       struct buffered_reader **reader_ptr)
{
	return open_uds_index_buffered_reader(state->layout, state->load_slot,
					      kind, zone, reader_ptr);
}

int open_state_buffered_writer(struct index_state *state,
			       enum region_kind kind,
			       unsigned int zone,
			       struct buffered_writer **writer_ptr)
{
	return open_uds_index_buffered_writer(state->layout, state->save_slot,
					      kind, zone, writer_ptr);
}
