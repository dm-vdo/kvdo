// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "volume-index-ops.h"

#include "compiler.h"
#include "config.h"
#include "errors.h"
#include "geometry.h"
#include "logger.h"
#include "volume-index005.h"
#include "volume-index006.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

static INLINE bool uses_sparse(const struct configuration *config)
{
	return is_sparse_geometry(config->geometry);
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
	*block_count = DIV_ROUND_UP(num_bytes, block_size) + MAX_ZONES;
	return UDS_SUCCESS;
}

int save_volume_index(struct volume_index *volume_index,
		      struct buffered_writer **writers,
		      unsigned int num_writers)
{
	int result = UDS_SUCCESS;
	unsigned int zone;

	for (zone = 0; zone < num_writers; ++zone) {
		result = start_saving_volume_index(volume_index,
						   zone,
						   writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = finish_saving_volume_index(volume_index, zone);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = write_guard_delta_list(writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = flush_buffered_writer(writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}
	}

	return result;
}

int load_volume_index(struct volume_index *volume_index,
		      struct buffered_reader **readers,
		      unsigned int num_readers)
{
	/* Start by reading the "header" section of the stream */
	int result = start_restoring_volume_index(volume_index,
						  readers,
						  num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = finish_restoring_volume_index(volume_index,
					       readers,
					       num_readers);
	if (result != UDS_SUCCESS) {
		abort_restoring_volume_index(volume_index);
		return result;
	}

	/* Check the final guard lists to make sure we read everything. */
	result = check_guard_delta_lists(readers, num_readers);
	if (result != UDS_SUCCESS) {
		abort_restoring_volume_index(volume_index);
	}

	return result;
}
