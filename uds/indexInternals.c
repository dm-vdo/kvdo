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
 * $Id: //eng/uds-releases/krusty/src/uds/indexInternals.c#16 $
 */

#include "indexInternals.h"

#include "errors.h"
#include "indexCheckpoint.h"
#include "indexStateData.h"
#include "indexZone.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "openChapter.h"
#include "request.h"
#include "stringUtils.h"
#include "threads.h"
#include "typeDefs.h"
#include "volume.h"
#include "zone.h"

static const unsigned int MAX_COMPONENT_COUNT = 4;

/**********************************************************************/
int allocate_index(struct index_layout *layout,
		   const struct configuration *config,
		   const struct uds_parameters *user_params,
		   unsigned int zone_count,
		   enum load_type load_type,
		   struct index **new_index)
{
	struct index *index;
	int result;
	unsigned int i;
	unsigned int checkpoint_frequency =
		user_params == NULL ? 0 : user_params->checkpoint_frequency;
	if (checkpoint_frequency >= config->geometry->chapters_per_volume) {
		return UDS_BAD_CHECKPOINT_FREQUENCY;
	}

	result = ALLOCATE(1, struct index, "index", &index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	index->existed = (load_type != LOAD_CREATE);
	index->has_saved_open_chapter = true;
	index->loaded_type = LOAD_UNDEFINED;

	result = make_index_checkpoint(index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}
	set_index_checkpoint_frequency(index->checkpoint,
				       checkpoint_frequency);

	get_index_layout(layout, &index->layout);
	index->zone_count = zone_count;

	result = ALLOCATE(index->zone_count, struct index_zone *, "zones",
			  &index->zones);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_index_state(layout, index->zone_count,
				  MAX_COMPONENT_COUNT, &index->state);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = add_index_state_component(index->state, &INDEX_STATE_INFO,
					   index, NULL);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_volume(config, index->layout,
			     user_params,
			     VOLUME_CACHE_DEFAULT_MAX_QUEUED_READS,
			     index->zone_count, &index->volume);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}
	index->volume->lookup_mode = LOOKUP_NORMAL;

	for (i = 0; i < index->zone_count; i++) {
		result = make_index_zone(index, i);
		if (result != UDS_SUCCESS) {
			free_index(index);
			return log_error_strerror(result,
						  "Could not create index zone");
		}
	}

	result = add_index_state_component(index->state, &OPEN_CHAPTER_INFO,
					   index, NULL);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return log_error_strerror(result,
					  "Could not create open chapter");
	}

	*new_index = index;
	return UDS_SUCCESS;
}

/**********************************************************************/
void release_index(struct index *index)
{
	if (index == NULL) {
		return;
	}

	if (index->zones != NULL) {
		unsigned int i;
		for (i = 0; i < index->zone_count; i++) {
			free_index_zone(index->zones[i]);
		}
		FREE(index->zones);
	}

	free_volume(index->volume);

	free_index_state(index->state);
	free_index_checkpoint(index->checkpoint);
	put_index_layout(&index->layout);
	FREE(index);
}
