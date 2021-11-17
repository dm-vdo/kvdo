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

#include "index-component.h"

#include "compiler.h"
#include "errors.h"
#include "index-layout.h"
#include "index-state.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "type-defs.h"

/**********************************************************************/
int make_index_component(struct index_state *state,
			 const struct index_component_info *info,
			 unsigned int zone_count,
			 void *data,
			 void *context,
			 struct index_component **component_ptr)
{
	struct index_component *component = NULL;
	int result;
	if ((info == NULL) || (info->name == NULL)) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "invalid component or directory specified");
	}
	if (info->loader == NULL) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "no .loader function specified for component %s",
					      info->name);
	}
	if (info->saver == NULL) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "no .saver function specified for component %s",
					      info->name);
	}

	result = UDS_ALLOCATE(1, struct index_component, "index component",
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

/**********************************************************************/
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
			UDS_FREE(wz);
		}
		UDS_FREE(component->write_zones);
		component->write_zones = NULL;
	}
}

/**********************************************************************/
void free_index_component(struct index_component *component)
{
	if (component == NULL) {
		return;
	}

	free_write_zones(component);
	UDS_FREE(component);
}

/**
 * Destroy, deallocate, and expunge a read portal.
 *
 * @param read_portal     the readzone array
 **/
static void free_read_portal(struct read_portal *read_portal)
{
	unsigned int z;
	if (read_portal == NULL) {
		return;
	}
	for (z = 0; z < read_portal->zones; ++z) {
		if (read_portal->readers[z] != NULL) {
			free_buffered_reader(read_portal->readers[z]);
		}
	}
	UDS_FREE(read_portal->readers);
	UDS_FREE(read_portal);
}

/**********************************************************************/
int get_buffered_reader_for_portal(struct read_portal *portal,
				   unsigned int part,
				   struct buffered_reader **reader_ptr)
{
	struct index_component *component;
	if (part >= portal->zones) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					  "%s: cannot access zone %u of %u",
					  __func__,
					  part,
					  portal->zones);
	}
	component = portal->component;
	if (component->info->io_storage && (portal->readers[part] == NULL)) {
		int result = open_state_buffered_reader(component->state,
							component->info->kind,
							part,
							&portal->readers[part]);
		if (result != UDS_SUCCESS) {
			return uds_log_error_strerror(result,
						      "%s: cannot make buffered reader for zone %u",
						      __func__,
						      part);
		}
	}
	*reader_ptr = portal->readers[part];
	return UDS_SUCCESS;
}

/**********************************************************************/
int read_index_component(struct index_component *component)
{
	struct read_portal *portal;
	int read_zones, result;
	result = UDS_ALLOCATE(1, struct read_portal,
			      "index component read portal", &portal);
	if (result != UDS_SUCCESS) {
		return result;
	}
	read_zones = component->state->load_zones;
	result = UDS_ALLOCATE(read_zones,
			      struct buffered_reader *,
			      "read zone buffered readers",
			      &portal->readers);
	if (result != UDS_SUCCESS) {
		UDS_FREE(portal);
		return result;
	}

	portal->component = component;
	portal->zones = read_zones;
	result = (*component->info->loader)(portal);
	free_read_portal(portal);
	return result;
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
			return uds_log_error_strerror(result,
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
	int result;

	if (component->write_zones != NULL) {
		return UDS_SUCCESS;
	}

	result = UDS_ALLOCATE(component->num_zones,
			      struct write_zone *,
			      "index component write zones",
			      &component->write_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (z = 0; z < component->num_zones; ++z) {
		result = UDS_ALLOCATE(1,
				      struct write_zone,
				      "plain write zone",
				      &component->write_zones[z]);
		if (result != UDS_SUCCESS) {
			free_write_zones(component);
			return result;
		}
		*component->write_zones[z] = (struct write_zone){
			.component = component,
			.zone = z,
		};
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
static int open_buffered_writers(struct index_component *component)
{
	int result = UDS_SUCCESS;
	struct write_zone **wzp;
	for (wzp = component->write_zones;
	     wzp < component->write_zones + component->num_zones;
	     ++wzp) {
		struct write_zone *wz = *wzp;

		result = ASSERT(wz->writer == NULL,
				"write zone writer already exists");
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (component->info->io_storage) {
			result =
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

/**********************************************************************/
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

/**********************************************************************/
int write_index_component(struct index_component *component)
{
	int result;
	unsigned int z;
	saver_t saver = component->info->saver;

	result = start_index_component_save(component);
	if (result != UDS_SUCCESS) {
		return result;
	}

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
		return uds_log_error_strerror(result,
					      "index component write failed");
	}

	return UDS_SUCCESS;
}

/**********************************************************************/
int discard_index_component(struct index_component *component)
{
	unsigned int num_zones = 0, save_slot = 0, old_save_slot, z;
	int result;
	if (!component->info->io_storage) {
		return UDS_INVALID_ARGUMENT;
	}

	result = find_latest_uds_index_save_slot(component->state->layout,
						 &num_zones, &save_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	old_save_slot = component->state->save_slot;
	component->state->save_slot = save_slot;

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
