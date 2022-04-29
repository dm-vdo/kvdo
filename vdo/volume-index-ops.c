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
#include "volume-index-ops.h"

#include "compiler.h"
#include "config.h"
#include "errors.h"
#include "geometry.h"
#include "index-component.h"
#include "logger.h"
#include "volume-index005.h"
#include "volume-index006.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

static INLINE bool uses_sparse(const struct configuration *config)
{
	return is_sparse(config->geometry);
}

void get_volume_index_combined_stats(const struct volume_index *volume_index,
				     struct volume_index_stats *stats)
{
	struct volume_index_stats dense, sparse;

	get_volume_index_stats(volume_index, &dense, &sparse);
	stats->memory_allocated =
		dense.memory_allocated + sparse.memory_allocated;
	stats->rebalance_time = dense.rebalance_time + sparse.rebalance_time;
	stats->rebalance_count =
		dense.rebalance_count + sparse.rebalance_count;
	stats->record_count = dense.record_count + sparse.record_count;
	stats->collision_count =
		dense.collision_count + sparse.collision_count;
	stats->discard_count = dense.discard_count + sparse.discard_count;
	stats->overflow_count = dense.overflow_count + sparse.overflow_count;
	stats->num_lists = dense.num_lists + sparse.num_lists;
	stats->early_flushes = dense.early_flushes + sparse.early_flushes;
}

int make_volume_index(const struct configuration *config,
		      uint64_t volume_nonce,
		      struct volume_index **volume_index)
{
	if (uses_sparse(config)) {
		return make_volume_index006(config, volume_nonce,
					    volume_index);
	} else {
		return make_volume_index005(config, volume_nonce,
					    volume_index);
	}
}

int compute_volume_index_save_blocks(const struct configuration *config,
				     size_t block_size,
				     uint64_t *block_count)
{
	size_t num_bytes;
	int result = (uses_sparse(config) ?
			      compute_volume_index_save_bytes006(config,
								 &num_bytes) :
			      compute_volume_index_save_bytes005(config,
								 &num_bytes));
	if (result != UDS_SUCCESS) {
		return result;
	}
	num_bytes += sizeof(struct delta_list_save_info);
	*block_count = (num_bytes + block_size - 1) / block_size + MAX_ZONES;
	return UDS_SUCCESS;
}

static int read_volume_index(struct read_portal *portal)
{
	struct volume_index *volume_index =
		index_component_context(portal->component);
	unsigned int num_zones = portal->zones;
	struct buffered_reader *readers[MAX_ZONES];
	unsigned int z;

	if (num_zones > MAX_ZONES) {
		return uds_log_error_strerror(UDS_BAD_STATE,
					      "zone count %u must not exceed MAX_ZONES",
					      num_zones);
	}

	for (z = 0; z < num_zones; ++z) {
		int result =
			get_buffered_reader_for_portal(portal, z, &readers[z]);
		if (result != UDS_SUCCESS) {
			return uds_log_error_strerror(result,
						      "cannot read component for zone %u",
						      z);
		}
	}
	return restore_volume_index(readers, num_zones, volume_index);
}

static int write_volume_index(struct index_component *component,
			      struct buffered_writer *writer,
			      unsigned int zone)
{
	struct volume_index *volume_index = index_component_context(component);
	int result;

	result = start_saving_volume_index(volume_index, zone, writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = finish_saving_volume_index(volume_index, zone);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = write_guard_delta_list(writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return flush_buffered_writer(writer);
}


static const struct index_component_info VOLUME_INDEX_INFO_DATA = {
	.kind = RL_KIND_VOLUME_INDEX,
	.name = "volume index",
	.multi_zone = true,
	.io_storage = true,
	.loader = read_volume_index,
	.saver = write_volume_index,
};
const struct index_component_info *const VOLUME_INDEX_INFO =
	&VOLUME_INDEX_INFO_DATA;

static int restore_volume_index_body(struct buffered_reader **buffered_readers,
				     unsigned int num_readers,
				     struct volume_index *volume_index,
				     byte dl_data[DELTA_LIST_MAX_BYTE_COUNT])
{
	unsigned int z;
	/* Start by reading the "header" section of the stream */
	int result = start_restoring_volume_index(volume_index,
						  buffered_readers,
						  num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * Loop to read the delta lists, stopping when they have all been
	 * processed.
	 */
	for (z = 0; z < num_readers; z++) {
		for (;;) {
			struct delta_list_save_info dlsi;

			result = read_saved_delta_list(&dlsi, dl_data,
						       buffered_readers[z]);
			if (result == UDS_END_OF_FILE) {
				break;
			} else if (result != UDS_SUCCESS) {
				abort_restoring_volume_index(volume_index);
				return result;
			}
			result = restore_delta_list_to_volume_index(volume_index,
								    &dlsi,
								    dl_data);
			if (result != UDS_SUCCESS) {
				abort_restoring_volume_index(volume_index);
				return result;
			}
		}
	}
	if (!is_restoring_volume_index_done(volume_index)) {
		abort_restoring_volume_index(volume_index);
		return uds_log_warning_strerror(UDS_CORRUPT_COMPONENT,
						"incomplete delta list data");
	}
	return UDS_SUCCESS;
}

int restore_volume_index(struct buffered_reader **buffered_readers,
			 unsigned int num_readers,
			 struct volume_index *volume_index)
{
	byte *dl_data;
	int result =
		UDS_ALLOCATE(DELTA_LIST_MAX_BYTE_COUNT, byte, __func__, &dl_data);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = restore_volume_index_body(buffered_readers, num_readers,
					   volume_index, dl_data);
	UDS_FREE(dl_data);
	return result;
}
