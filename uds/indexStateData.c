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
 * $Id: //eng/uds-releases/krusty/src/uds/indexStateData.c#14 $
 */

#include "indexStateData.h"

#include "buffer.h"
#include "errors.h"
#include "index.h"
#include "logger.h"
#include "uds.h"

/* The index state version header */
struct index_state_version {
	int32_t signature;
	int32_t version_id;
};

/* The version 301 index state */
struct index_state_data301 {
	uint64_t newest_chapter;
	uint64_t oldest_chapter;
	uint64_t last_checkpoint;
	uint32_t unused;
	uint32_t padding;
};

static const struct index_state_version INDEX_STATE_VERSION_301 = {
	.signature  = -1,
	.version_id = 301,
};

/**
 * The index state index component reader.
 *
 * @param portal the read_portal that handles the read of the component
 *
 * @return UDS_SUCCESS or an error code
 **/
static int read_index_state_data(struct read_portal *portal)
{
	struct buffer *buffer =
		get_state_index_state_buffer(portal->component->state,
					     IO_READ);
	int result = rewind_buffer(buffer, uncompacted_amount(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct index_state_version file_version;
	result = get_int32_le_from_buffer(buffer, &file_version.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_int32_le_from_buffer(buffer, &file_version.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (file_version.signature != -1 || file_version.version_id != 301) {
		return log_error_strerror(UDS_UNSUPPORTED_VERSION,
					  "index state version %d,%d is unsupported",
					  file_version.signature,
					  file_version.version_id);
	}

	struct index_state_data301 state;
	result = get_uint64_le_from_buffer(buffer, &state.newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &state.oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &state.last_checkpoint);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &state.unused);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &state.padding);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if ((state.unused != 0) || (state.padding != 0)) {
		return UDS_CORRUPT_COMPONENT;
	}

	struct index *index = index_component_data(portal->component);
	index->newest_virtual_chapter = state.newest_chapter;
	index->oldest_virtual_chapter = state.oldest_chapter;
	index->last_checkpoint = state.last_checkpoint;
	return UDS_SUCCESS;
}

/**
 * The index state index component writer.
 *
 * @param component The component whose state is to be saved (an index)
 * @param writer    The buffered writer.
 * @param zone      The zone to write.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
write_index_state_data(struct index_component *component,
		       struct buffered_writer *writer __always_unused,
		       unsigned int zone __always_unused)
{
	struct buffer *buffer =
		get_state_index_state_buffer(component->state, IO_WRITE);
	int result = reset_buffer_end(buffer, 0);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct index *index = index_component_data(component);
	struct index_state_data301 state = {
		.newest_chapter  = index->newest_virtual_chapter,
		.oldest_chapter  = index->oldest_virtual_chapter,
		.last_checkpoint = index->last_checkpoint,
	};

	result = put_uint64_le_into_buffer(buffer, state.newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, state.oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, state.last_checkpoint);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, state.unused);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, state.padding);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/

const struct index_component_info INDEX_STATE_INFO = {
	.kind         = RL_KIND_INDEX_STATE,
	.name         = "index state",
	.save_only    = false,
	.chapter_sync = true,
	.multi_zone   = false,
	.io_storage   = false,
	.loader       = read_index_state_data,
	.saver        = write_index_state_data,
	.incremental  = NULL,
};
