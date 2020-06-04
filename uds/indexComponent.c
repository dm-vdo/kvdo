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
 * $Id: //eng/uds-releases/krusty/src/uds/indexComponent.c#11 $
 */

#include "indexComponent.h"

#include "compiler.h"
#include "errors.h"
#include "indexLayout.h"
#include "indexState.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "typeDefs.h"

/*****************************************************************************/
int make_index_component(struct index_state *state,
			 const struct index_component_info *info,
			 unsigned int zone_count,
			 void *data,
			 void *context,
			 struct index_component **component_ptr)
{
	if ((info == NULL) || (info->name == NULL)) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "invalid component or directory specified");
	}
	if (info->loader == NULL) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "no .loader function specified for component %s",
					       info->name);
	}
	if ((info->saver == NULL) && (info->incremental == NULL)) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "neither .saver function nor .incremental function specified for component %s",
					       info->name);
	}

	struct index_component *component = NULL;
	int result = ALLOCATE(1, struct index_component, "index component",
			      &component);
	if (result != UDS_SUCCESS) {
		return result;
	}

	component->component_data = data;
	component->context = context;
	component->info = info;
	component->num_zones = info->multi_zone ? zone_count : 1;
	component->state = state;
	component->write_zones = NULL;
	*component_ptr = component;
	return UDS_SUCCESS;
}

/*****************************************************************************/
static void free_write_zones(struct index_component *component)
{
	if (component->write_zones != NULL) {
		unsigned int z;
		for (z = 0; z < component->num_zones; ++z) {
			struct write_zone *wz = component->write_zones[z];
			if (wz == NULL) {
				continue;
			}
			free_buffered_writer(wz->writer);
			FREE(wz);
		}
		FREE(component->write_zones);
		component->write_zones = NULL;
	}
}

/*****************************************************************************/
void free_index_component(struct index_component **component_ptr)
{
	if (component_ptr == NULL) {
		return;
	}
	struct index_component *component = *component_ptr;
	if (component == NULL) {
		return;
	}
	*component_ptr = NULL;

	free_write_zones(component);
	FREE(component);
}

/**
 * Destroy, deallocate, and expunge a read portal.
 *
 * @param read_portal     the readzone array
 **/
static void free_read_portal(struct read_portal *read_portal)
{
	if (read_portal == NULL) {
		return;
	}
	unsigned int z;
	for (z = 0; z < read_portal->zones; ++z) {
		if (read_portal->readers[z] != NULL) {
			free_buffered_reader(read_portal->readers[z]);
		}
	}
	FREE(read_portal->readers);
	FREE(read_portal);
}

/*****************************************************************************/
int get_buffered_reader_for_portal(struct read_portal *portal,
				   unsigned int part,
				   struct buffered_reader **reader_ptr)
{
	if (part >= portal->zones) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "%s: cannot access zone %u of %u",
					       __func__,
					       part,
					       portal->zones);
	}
	struct index_component *component = portal->component;
	if (component->info->io_storage && (portal->readers[part] == NULL)) {
		int result = open_state_buffered_reader(component->state,
							component->info->kind,
							part,
							&portal->readers[part]);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(result,
						       "%s: cannot make buffered reader for zone %u",
						       __func__,
						       part);
		}
	}
	*reader_ptr = portal->readers[part];
	return UDS_SUCCESS;
}

/*****************************************************************************/
int read_index_component(struct index_component *component)
{
	struct read_portal *portal;
	int result = ALLOCATE(1, struct read_portal,
			      "index component read portal", &portal);
	if (result != UDS_SUCCESS) {
		return result;
	}
	int read_zones = component->state->load_zones;
	result = ALLOCATE(read_zones,
			  struct buffered_reader *,
			  "read zone buffered readers",
			  &portal->readers);
	if (result != UDS_SUCCESS) {
		FREE(portal);
		return result;
	}

	portal->component = component;
	portal->zones = read_zones;
	result = (*component->info->loader)(portal);
	free_read_portal(portal);
	return result;
}

/**
 * Determine the write_zone structure for the specified component and zone.
 *
 * @param [in]  component       the index component
 * @param [in]  zone            the zone number
 * @param [out] write_zone_ptr  the resulting write zone instance
 *
 * @return UDS_SUCCESS or an error code
 **/
static int resolve_write_zone(const struct index_component *component,
			      unsigned int zone,
			      struct write_zone **write_zone_ptr)
{
	int result = ASSERT(write_zone_ptr != NULL, "output parameter is null");
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (component->write_zones == NULL) {
		return logErrorWithStringError(UDS_BAD_STATE,
					       "cannot resolve index component write zone: not allocated");
	}

	if (zone >= component->num_zones) {
		return logErrorWithStringError(UDS_INVALID_ARGUMENT,
					       "cannot resolve index component write zone: zone out of range");
	}
	*write_zone_ptr = component->write_zones[zone];
	return UDS_SUCCESS;
}

/**
 * Non-incremental save function used to emulate a regular save
 * using an incremental save function as a basis.
 *
 * @param component    the index component
 * @param writer       the buffered writer
 * @param zone         the zone number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
index_component_saver_incremental_wrapper(struct index_component *component,
					  struct buffered_writer *writer,
					  unsigned int zone)
{
	incremental_writer_t incr_func = component->info->incremental;
	bool completed = false;

	int result = (*incr_func)(component, writer, zone, IWC_START,
				  &completed);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!completed) {
		result = (*incr_func)(component, writer, zone, IWC_FINISH,
				      &completed);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	result = flush_buffered_writer(writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**
 * Specify that writing to a specific zone file has finished.
 *
 * If a syncer has been registered with the index component, the file
 * descriptor will be enqueued upon it for fsyncing and closing.
 * If not, or if the enqueue fails, the file will be fsynced and closed
 * immediately.
 *
 * @param write_zone    the index component write zone
 *
 * @return UDS_SUCCESS or an error code
 **/
static int done_with_zone(struct write_zone *write_zone)
{
	const struct index_component *component = write_zone->component;
	if (write_zone->writer != NULL) {
		int result = flush_buffered_writer(write_zone->writer);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(result,
						       "cannot flush buffered writer for %s component (zone %u)",
						       component->info->name,
						       write_zone->zone);
		}
	}
	return UDS_SUCCESS;
}

/**
 * Construct the array of write_zone instances for this component.
 *
 * @param component    the index component
 *
 * @return UDS_SUCCESS or an error code
 *
 * If this is a multizone component, each zone will be fully defined,
 * otherwise zone 0 stands in for the single state file.
 **/
static int make_write_zones(struct index_component *component)
{
	unsigned int z;
	if (component->write_zones != NULL) {
		// just reinitialize states
		for (z = 0; z < component->num_zones; ++z) {
			struct write_zone *wz = component->write_zones[z];
			wz->phase = IWC_IDLE;
		}
		return UDS_SUCCESS;
	}

	int result = ALLOCATE(component->num_zones,
			      struct write_zone *,
			      "index component write zones",
			      &component->write_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (z = 0; z < component->num_zones; ++z) {
		result = ALLOCATE(1,
				  struct write_zone,
				  "plain write zone",
				  &component->write_zones[z]);
		if (result != UDS_SUCCESS) {
			free_write_zones(component);
			return result;
		}
		*component->write_zones[z] = (struct write_zone){
			.component = component,
			.phase = IWC_IDLE,
			.zone = z,
		};
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
static int open_buffered_writers(struct index_component *component)
{
	int result = UDS_SUCCESS;
	struct write_zone **wzp;
	for (wzp = component->write_zones;
	     wzp < component->write_zones + component->num_zones;
	     ++wzp) {
		struct write_zone *wz = *wzp;
		wz->phase = IWC_START;

		result = ASSERT(wz->writer == NULL,
				"write zone writer already exists");
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (component->info->io_storage) {
			int result =
				open_state_buffered_writer(component->state,
							   component->info->kind,
							   wz->zone,
							   &wz->writer);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
static int start_index_component_save(struct index_component *component)
{
	int result = make_write_zones(component);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_buffered_writers(component);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/*****************************************************************************/
int start_index_component_incremental_save(struct index_component *component)
{
	return start_index_component_save(component);
}

/*****************************************************************************/
int write_index_component(struct index_component *component)
{
	saver_t saver = component->info->saver;
	if ((saver == NULL) && (component->info->incremental != NULL)) {
		saver = index_component_saver_incremental_wrapper;
	}

	int result = start_index_component_save(component);
	if (result != UDS_SUCCESS) {
		return result;
	}

	unsigned int z;
	for (z = 0; z < component->num_zones; ++z) {
		struct write_zone *write_zone = component->write_zones[z];

		result = (*saver)(component, write_zone->writer, z);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = done_with_zone(write_zone);
		if (result != UDS_SUCCESS) {
			break;
		}

		free_buffered_writer(write_zone->writer);
		write_zone->writer = NULL;
	}

	if (result != UDS_SUCCESS) {
		free_write_zones(component);
		return logErrorWithStringError(result,
					       "index component write failed");
	}

	return UDS_SUCCESS;
}

/**
 * Close a specific buffered writer in a component write zone.
 *
 * @param write_zone    the write zone
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note closing a buffered writer causes its file descriptor to be
 *       passed to done_with_zone
 **/
static int close_buffered_writer(struct write_zone *write_zone)
{
	if (write_zone->writer == NULL) {
		return UDS_SUCCESS;
	}

	int result = done_with_zone(write_zone);
	free_buffered_writer(write_zone->writer);
	write_zone->writer = NULL;

	return result;
}

/**
 * Faux incremental saver function for index components which only define
 * a simple saver.  Conforms to incremental_writer_t signature.
 *
 * @param [in]  component      the index component
 * @param [in]  writer         the buffered writer that does the output
 * @param [in]  zone           the zone number
 * @param [in]  command        the incremental writer command
 * @param [out] completed      if non-NULL, set to whether the save is complete
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note This wrapper always calls the non-incremental saver when
 *       the IWC_START command is issued, and always reports that
 *       the save is complete unless the saver failed.
 **/
static int wrap_saver_as_incremental(struct index_component *component,
				     struct buffered_writer *writer,
				     unsigned int zone,
				     enum incremental_writer_command command,
				     bool *completed)
{
	int result = UDS_SUCCESS;

	if ((command >= IWC_START) && (command <= IWC_FINISH)) {
		result = (*component->info->saver)(component, writer, zone);
		if ((result == UDS_SUCCESS) && (writer != NULL)) {
			note_buffered_writer_used(writer);
		}
	}
	if ((result == UDS_SUCCESS) && (completed != NULL)) {
		*completed = true;
	}
	return result;
}

/**
 * Return the appropriate incremental writer function depending on
 * the component's type and whether this is the first zone.
 *
 * @param component    the index component
 *
 * @return the correct incremental_writer_t function to use, or
 *         NULL signifying no progress can be made at this time.
 **/
static incremental_writer_t
get_incremental_writer(struct index_component *component)
{
	incremental_writer_t incr_func = component->info->incremental;

	if (incr_func == NULL) {
		incr_func = &wrap_saver_as_incremental;
	}

	return incr_func;
}

/*****************************************************************************/
int perform_index_component_zone_save(struct index_component *component,
				      unsigned int zone,
				      enum completion_status *completed)
{
	enum completion_status comp = CS_NOT_COMPLETED;

	struct write_zone *wz = NULL;
	int result = resolve_write_zone(component, zone, &wz);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (wz->phase == IWC_IDLE) {
		comp = CS_COMPLETED_PREVIOUSLY;
	} else if (wz->phase == IWC_DONE) {
		comp = CS_JUST_COMPLETED;
		wz->phase = IWC_IDLE;
	} else if (!component->info->chapter_sync) {
		bool done = false;
		incremental_writer_t incr_func =
			get_incremental_writer(component);
		int result = (*incr_func)(component, wz->writer, zone,
					  wz->phase, &done);
		if (result != UDS_SUCCESS) {
			if (wz->phase == IWC_ABORT) {
				wz->phase = IWC_IDLE;
			} else {
				wz->phase = IWC_ABORT;
			}
			return result;
		}
		if (done) {
			comp = CS_JUST_COMPLETED;
			wz->phase = IWC_IDLE;
		} else if (wz->phase == IWC_START) {
			wz->phase = IWC_CONTINUE;
		}
	}

	if (completed != NULL) {
		*completed = comp;
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int
perform_index_component_chapter_writer_save(struct index_component *component)
{
	struct write_zone *wz = NULL;
	int result = resolve_write_zone(component, 0, &wz);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
		bool done = false;
		incremental_writer_t incr_func =
			get_incremental_writer(component);
		int result = ASSERT(incr_func != NULL, "no writer function");
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = (*incr_func)(component, wz->writer, 0, wz->phase,
				      &done);
		if (result != UDS_SUCCESS) {
			if (wz->phase == IWC_ABORT) {
				wz->phase = IWC_IDLE;
			} else {
				wz->phase = IWC_ABORT;
			}
			return result;
		}
		if (done) {
			wz->phase = IWC_DONE;
		} else if (wz->phase == IWC_START) {
			wz->phase = IWC_CONTINUE;
		}
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int finish_index_component_zone_save(struct index_component *component,
				     unsigned int zone,
				     enum completion_status *completed)
{
	struct write_zone *wz = NULL;
	int result = resolve_write_zone(component, zone, &wz);
	if (result != UDS_SUCCESS) {
		return result;
	}

	enum completion_status comp;
	switch (wz->phase) {
	case IWC_IDLE:
		comp = CS_COMPLETED_PREVIOUSLY;
		break;

	case IWC_DONE:
		comp = CS_JUST_COMPLETED;
		break;

	default:
		comp = CS_NOT_COMPLETED;
	}

	incremental_writer_t incr_func = get_incremental_writer(component);
	if ((wz->phase >= IWC_START) && (wz->phase < IWC_ABORT)) {
		bool done = false;
		int result = (*incr_func)(component, wz->writer, zone,
					  IWC_FINISH, &done);
		if (result != UDS_SUCCESS) {
			wz->phase = IWC_ABORT;
			return result;
		}
		if (!done) {
			logWarning("finish incremental save did not complete for %s zone %u",
				   component->info->name,
				   zone);
			return UDS_CHECKPOINT_INCOMPLETE;
		}
		wz->phase = IWC_IDLE;
		comp = CS_JUST_COMPLETED;
	}

	if (completed != NULL) {
		*completed = comp;
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int finish_index_component_incremental_save(struct index_component *component)
{
	unsigned int zone;
	for (zone = 0; zone < component->num_zones; ++zone) {
		struct write_zone *wz = component->write_zones[zone];
		incremental_writer_t incr_func =
			get_incremental_writer(component);
		if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
			// Note: this is only safe if no other threads are
			// currently processing this particular index
			bool done = false;
			int result = (*incr_func)(component, wz->writer, zone,
						  IWC_FINISH, &done);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (!done) {
				logWarning("finishing incremental save did not complete for %s zone %u",
					   component->info->name,
					   zone);
				return UDS_UNEXPECTED_RESULT;
			}
			wz->phase = IWC_IDLE;
		}

		if ((wz->writer != NULL) &&
		    !was_buffered_writer_used(wz->writer)) {
			return logErrorWithStringError(UDS_CHECKPOINT_INCOMPLETE,
						       "component %s zone %u did not get written",
						       component->info->name,
						       zone);
		}

		int result = close_buffered_writer(wz);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/*****************************************************************************/
int abort_index_component_zone_save(struct index_component *component,
				    unsigned int zone,
				    enum completion_status *status)
{
	struct write_zone *wz = NULL;
	int result = resolve_write_zone(component, zone, &wz);
	if (result != UDS_SUCCESS) {
		return result;
	}

	enum completion_status comp = CS_COMPLETED_PREVIOUSLY;

	incremental_writer_t incr_func = get_incremental_writer(component);
	if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
		result = (*incr_func)(component, wz->writer, zone, IWC_ABORT,
				      NULL);
		wz->phase = IWC_IDLE;
		if (result != UDS_SUCCESS) {
			return result;
		}
		comp = CS_JUST_COMPLETED;
	}

	if (status != NULL) {
		*status = comp;
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int abort_index_component_incremental_save(struct index_component *component)
{
	int result = UDS_SUCCESS;
	unsigned int zone;
	for (zone = 0; zone < component->num_zones; ++zone) {
		struct write_zone *wz = component->write_zones[zone];
		incremental_writer_t incr_func =
			get_incremental_writer(component);
		if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
			// Note: this is only safe if no other threads are
			// currently processing this particular index
			result = (*incr_func)(component, wz->writer, zone,
					      IWC_ABORT, NULL);
			wz->phase = IWC_IDLE;
			if (result != UDS_SUCCESS) {
				return result;
			}
		}

		int result = close_buffered_writer(wz);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/*****************************************************************************/
int discard_index_component(struct index_component *component)
{
	if (!component->info->io_storage) {
		return UDS_INVALID_ARGUMENT;
	}

	unsigned int num_zones = 0;
	unsigned int save_slot = 0;
	int result = find_latest_index_save_slot(component->state->layout,
						 &num_zones, &save_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	unsigned int old_save_slot = component->state->save_slot;
	component->state->save_slot = save_slot;

	unsigned int z;
	for (z = 0; z < num_zones; ++z) {
		struct buffered_writer *writer;
		int result = open_state_buffered_writer(component->state,
							component->info->kind,
							z, &writer);
		if (result != UDS_SUCCESS) {
			break;
		}
		result =
			write_zeros_to_buffered_writer(writer, UDS_BLOCK_SIZE);
		if (result != UDS_SUCCESS) {
			break;
		}
		result = flush_buffered_writer(writer);
		if (result != UDS_SUCCESS) {
			break;
		}
		free_buffered_writer(writer);
	}

	component->state->save_slot = old_save_slot;
	return result;
}
