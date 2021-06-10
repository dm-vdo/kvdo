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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.c#133 $
 */

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdoInternal.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "blockMap.h"
#include "deviceRegistry.h"
#include "hashZone.h"
#include "header.h"
#include "instanceNumber.h"
#include "logicalZone.h"
#include "numUtils.h"
#include "packer.h"
#include "physicalZone.h"
#include "readOnlyNotifier.h"
#include "recoveryJournal.h"
#include "releaseVersions.h"
#include "slabDepot.h"
#include "slabSummary.h"
#include "statistics.h"
#include "statusCodes.h"
#include "superBlock.h"
#include "superBlockCodec.h"
#include "threadConfig.h"
#include "vdoComponentStates.h"
#include "vdoLayout.h"

#include "dedupeIndex.h"
#include "kernelVDO.h"
#include "workQueue.h"

/**********************************************************************/
void destroy_vdo(struct vdo *vdo)
{
	int i;
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);

	unregister_vdo(vdo);
	free_vdo_dedupe_index(&vdo->dedupe_index);
	free_vdo_flusher(&vdo->flusher);
	free_vdo_packer(&vdo->packer);
	free_vdo_recovery_journal(&vdo->recovery_journal);
	free_vdo_slab_depot(&vdo->depot);
	free_vdo_layout(&vdo->layout);
	free_super_block(&vdo->super_block);
	free_vdo_block_map(&vdo->block_map);

	if (vdo->hash_zones != NULL) {
		zone_count_t zone;
		for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
			free_vdo_hash_zone(&vdo->hash_zones[zone]);
		}
	}
	FREE(vdo->hash_zones);
	vdo->hash_zones = NULL;

	free_logical_zones(&vdo->logical_zones);

	if (vdo->physical_zones != NULL) {
		zone_count_t zone;
		for (zone = 0; zone < thread_config->physical_zone_count; zone++) {
			free_vdo_physical_zone(&vdo->physical_zones[zone]);
		}
	}

	FREE(vdo->physical_zones);
	vdo->physical_zones = NULL;
	free_vdo_read_only_notifier(&vdo->read_only_notifier);
	free_vdo_thread_config(&vdo->thread_config);

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		free_work_queue(&vdo->threads[i].request_queue);
	}
	FREE(vdo->threads);
	vdo->threads = NULL;

	for (i = 0; i < vdo->device_config->thread_counts.cpu_threads; i++) {
		FREE(FORGET(vdo->compression_context[i]));
	}

	FREE(FORGET(vdo->compression_context));
	release_vdo_instance(vdo->instance);

	/*
	 * The call to kobject_put on the kobj sysfs node will decrement its
	 * reference count; when the count goes to zero the VDO object will be
	 * freed as a side effect.
	 */
	kobject_put(&vdo->work_queue_directory);
	kobject_put(&vdo->vdo_directory);
}

/**********************************************************************/
void vdo_wait_for_no_requests_active(struct vdo *vdo)
{
	bool was_compressing;

	// Do nothing if there are no requests active.  This check is not
	// necessary for correctness but does reduce log message traffic.
	if (limiter_is_idle(&vdo->request_limiter)) {
		return;
	}

	/*
	 * We have to make sure to flush the packer before waiting. We do this
	 * by turning off compression, which also means no new entries coming
	 * in while waiting will end up in the packer.
	 */
	was_compressing = set_kvdo_compressing(vdo, false);
	// Now wait for there to be no active requests
	limiter_wait_for_idle(&vdo->request_limiter);
	// Reset the compression state after all requests are done
	if (was_compressing) {
		set_kvdo_compressing(vdo, true);
	}
}


/**********************************************************************/
struct block_device *get_vdo_backing_device(const struct vdo *vdo)
{
	return vdo->device_config->owned_device->bdev;
}

/**********************************************************************/
enum vdo_state get_vdo_state(const struct vdo *vdo)
{
	enum vdo_state state = atomic_read(&vdo->state);
	smp_rmb();
	return state;
}

/**********************************************************************/
void set_vdo_state(struct vdo *vdo, enum vdo_state state)
{
	smp_wmb();
	atomic_set(&vdo->state, state);
}

/**
 * Record the state of the VDO for encoding in the super block.
 **/
static void record_vdo(struct vdo *vdo)
{
	vdo->states.release_version = vdo->geometry.release_version;
	vdo->states.vdo.state = get_vdo_state(vdo);
	vdo->states.block_map = record_vdo_block_map(vdo->block_map);
	vdo->states.recovery_journal =
		record_vdo_recovery_journal(vdo->recovery_journal);
	vdo->states.slab_depot = record_vdo_slab_depot(vdo->depot);
	vdo->states.layout = get_vdo_fixed_layout(vdo->layout);
}

/**********************************************************************/
void save_vdo_components(struct vdo *vdo, struct vdo_completion *parent)
{
	int result;

	struct buffer *buffer
		= get_vdo_super_block_codec(vdo->super_block)->component_buffer;
	record_vdo(vdo);
	result = encode_vdo_component_states(buffer, &vdo->states);
	if (result != VDO_SUCCESS) {
		finish_vdo_completion(parent, result);
		return;
	}

	save_vdo_super_block(vdo->super_block, get_vdo_first_block_offset(vdo),
			     parent);
}

/**
 * Notify a vdo that it is going read-only. This will save the read-only state
 * to the super block.
 *
 * <p>Implements vdo_read_only_notification.
 *
 * @param listener  The vdo
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void notify_vdo_of_read_only_mode(void *listener,
				    struct vdo_completion *parent)
{
	struct vdo *vdo = listener;
	if (in_read_only_mode(vdo)) {
		complete_vdo_completion(parent);
	}

	set_vdo_state(vdo, VDO_READ_ONLY_MODE);
	save_vdo_components(vdo, parent);
}

/**********************************************************************/
int enable_read_only_entry(struct vdo *vdo)
{
	return register_vdo_read_only_listener(vdo->read_only_notifier,
					       vdo,
					       notify_vdo_of_read_only_mode,
					       vdo_get_admin_thread(get_vdo_thread_config(vdo)));
}

/**********************************************************************/
bool in_read_only_mode(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_READ_ONLY_MODE);
}

/**********************************************************************/
bool vdo_was_new(const struct vdo *vdo)
{
	return (vdo->load_state == VDO_NEW);
}

/**********************************************************************/
bool requires_read_only_rebuild(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_FORCE_REBUILD) ||
		(vdo->load_state == VDO_REBUILD_FOR_UPGRADE));
}

/**********************************************************************/
bool requires_rebuild(const struct vdo *vdo)
{
	switch (get_vdo_state(vdo)) {
	case VDO_DIRTY:
	case VDO_FORCE_REBUILD:
	case VDO_REPLAYING:
	case VDO_REBUILD_FOR_UPGRADE:
		return true;

	default:
		return false;
	}
}

/**********************************************************************/
bool requires_recovery(const struct vdo *vdo)
{
	return ((vdo->load_state == VDO_DIRTY) ||
		(vdo->load_state == VDO_REPLAYING) ||
		(vdo->load_state == VDO_RECOVERING));
}

/**********************************************************************/
bool is_replaying(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_REPLAYING);
}

/**********************************************************************/
bool in_recovery_mode(const struct vdo *vdo)
{
	return (get_vdo_state(vdo) == VDO_RECOVERING);
}

/**********************************************************************/
void enter_recovery_mode(struct vdo *vdo)
{
	assert_on_admin_thread(vdo, __func__);

	if (in_read_only_mode(vdo)) {
		return;
	}

	uds_log_info("Entering recovery mode");
	set_vdo_state(vdo, VDO_RECOVERING);
}

/**********************************************************************/
bool set_vdo_compressing(struct vdo *vdo, bool enable_compression)
{
	bool was_enabled = vdo->compressing;
	WRITE_ONCE(vdo->compressing, enable_compression);
	if (was_enabled && !enable_compression) {
		// Flushing the packer is asynchronous, but we don't care when
		// it finishes.
		flush_vdo_packer(vdo->packer);
	}

	uds_log_info("compression is %s",
		     (enable_compression ? "enabled" : "disabled"));
	return was_enabled;
}

/**********************************************************************/
bool get_vdo_compressing(struct vdo *vdo)
{
	return READ_ONCE(vdo->compressing);
}

/**********************************************************************/
static size_t get_block_map_cache_size(const struct vdo *vdo)
{
	return ((size_t) vdo->device_config->cache_size) * VDO_BLOCK_SIZE;
}

/**
 * Tally the hash lock statistics from all the hash zones.
 *
 * @param vdo  The vdo to query
 *
 * @return The sum of the hash lock statistics from all hash zones
 **/
static struct hash_lock_statistics
get_hash_lock_statistics(const struct vdo *vdo)
{
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);
	zone_count_t zone;
	struct hash_lock_statistics totals;
	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
		struct hash_lock_statistics stats =
			get_vdo_hash_zone_statistics(vdo->hash_zones[zone]);
		totals.dedupe_advice_valid += stats.dedupe_advice_valid;
		totals.dedupe_advice_stale += stats.dedupe_advice_stale;
		totals.concurrent_data_matches +=
			stats.concurrent_data_matches;
		totals.concurrent_hash_collisions +=
			stats.concurrent_hash_collisions;
	}

	return totals;
}

/**
 * Get the current error statistics from a vdo.
 *
 * @param vdo  The vdo to query
 *
 * @return a copy of the current vdo error counters
 **/
static struct error_statistics get_vdo_error_statistics(const struct vdo *vdo)
{
	/*
	 * The error counts can be incremented from arbitrary threads and so
	 * must be incremented atomically, but they are just statistics with no
	 * semantics that could rely on memory order, so unfenced reads are
	 * sufficient.
	 */
	const struct atomic_error_statistics *atoms = &vdo->error_stats;
	return (struct error_statistics) {
		.invalid_advice_pbn_count =
			atomic64_read(&atoms->invalid_advice_pbn_count),
		.no_space_error_count =
			atomic64_read(&atoms->no_space_error_count),
		.read_only_error_count =
			atomic64_read(&atoms->read_only_error_count),
	};
}

/**********************************************************************/
void get_vdo_statistics(const struct vdo *vdo,
			struct vdo_statistics *stats)
{
	enum vdo_state state;
	slab_count_t slab_total;

	// These are immutable properties of the vdo object, so it is safe to
	// query them from any thread.
	struct recovery_journal *journal = vdo->recovery_journal;
	struct slab_depot *depot = vdo->depot;
	// XXX config.physical_blocks is actually mutated during resize and is in
	// a packed structure, but resize runs on the admin thread so we're
	// usually OK.
	stats->version = STATISTICS_VERSION;
	stats->release_version = CURRENT_RELEASE_VERSION_NUMBER;
	stats->logical_blocks = vdo->states.vdo.config.logical_blocks;
	stats->physical_blocks = vdo->states.vdo.config.physical_blocks;
	stats->block_size = VDO_BLOCK_SIZE;
	stats->complete_recoveries = vdo->states.vdo.complete_recoveries;
	stats->read_only_recoveries = vdo->states.vdo.read_only_recoveries;
	stats->block_map_cache_size = get_block_map_cache_size(vdo);

	// The callees are responsible for thread-safety.
	stats->data_blocks_used = get_vdo_physical_blocks_allocated(vdo);
	stats->overhead_blocks_used = get_vdo_physical_blocks_overhead(vdo);
	stats->logical_blocks_used =
		get_vdo_recovery_journal_logical_blocks_used(journal);
	stats->allocator = get_vdo_slab_depot_block_allocator_statistics(depot);
	stats->journal = get_vdo_recovery_journal_statistics(journal);
	stats->packer = get_vdo_packer_statistics(vdo->packer);
	stats->slab_journal = get_vdo_slab_depot_slab_journal_statistics(depot);
	stats->slab_summary =
		get_vdo_slab_summary_statistics(get_vdo_slab_summary(depot));
	stats->ref_counts = get_vdo_slab_depot_ref_counts_statistics(depot);
	stats->block_map = get_vdo_block_map_statistics(vdo->block_map);
	stats->hash_lock = get_hash_lock_statistics(vdo);
	stats->errors = get_vdo_error_statistics(vdo);
	slab_total = get_vdo_slab_depot_slab_count(depot);
	stats->recovery_percentage =
		(slab_total - get_vdo_slab_depot_unrecovered_slab_count(depot)) * 100 /
		slab_total;

	state = get_vdo_state(vdo);
	stats->in_recovery_mode = (state == VDO_RECOVERING);
	snprintf(stats->mode,
		 sizeof(stats->mode),
		 "%s",
		 describe_vdo_state(state));
}

/**********************************************************************/
block_count_t get_vdo_physical_blocks_allocated(const struct vdo *vdo)
{
	return (get_vdo_slab_depot_allocated_blocks(vdo->depot) -
		vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**********************************************************************/
block_count_t get_vdo_physical_blocks_free(const struct vdo *vdo)
{
	return get_vdo_slab_depot_free_blocks(vdo->depot);
}

/**********************************************************************/
block_count_t get_vdo_physical_blocks_overhead(const struct vdo *vdo)
{
	// XXX config.physical_blocks is actually mutated during resize and is in
	// a packed structure, but resize runs on admin thread so we're usually
	// OK.
	return (vdo->states.vdo.config.physical_blocks -
		get_vdo_slab_depot_data_blocks(vdo->depot) +
		vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**********************************************************************/
block_count_t get_vdo_physical_block_count(const struct vdo *vdo)
{
	return vdo->device_config->physical_blocks;
}

/**********************************************************************/
const struct device_config *get_vdo_device_config(const struct vdo *vdo)
{
	return vdo->device_config;
}

/**********************************************************************/
const struct thread_config *get_vdo_thread_config(const struct vdo *vdo)
{
	return vdo->thread_config;
}

/**********************************************************************/
block_count_t get_vdo_configured_block_map_maximum_age(const struct vdo *vdo)
{
	return vdo->device_config->block_map_maximum_age;
}

/**********************************************************************/
page_count_t get_vdo_configured_cache_size(const struct vdo *vdo)
{
	return vdo->device_config->cache_size;
}

/**********************************************************************/
physical_block_number_t get_vdo_first_block_offset(const struct vdo *vdo)
{
	return vdo_get_data_region_start(vdo->geometry);
}

/**********************************************************************/
struct block_map *get_block_map(const struct vdo *vdo)
{
	return vdo->block_map;
}

/**********************************************************************/
struct slab_depot *get_slab_depot(struct vdo *vdo)
{
	return vdo->depot;
}

/**********************************************************************/
struct recovery_journal *get_recovery_journal(struct vdo *vdo)
{
	return vdo->recovery_journal;
}

/**********************************************************************/
void dump_vdo_status(const struct vdo *vdo)
{
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);
	zone_count_t zone;

	dump_vdo_flusher(vdo->flusher);
	dump_vdo_recovery_journal_statistics(vdo->recovery_journal);
	dump_vdo_packer(vdo->packer);
	dump_vdo_slab_depot(vdo->depot);

	for (zone = 0; zone < thread_config->logical_zone_count; zone++) {
		dump_vdo_logical_zone(get_vdo_logical_zone(vdo->logical_zones,
							   zone));
	}

	for (zone = 0; zone < thread_config->physical_zone_count; zone++) {
		dump_vdo_physical_zone(vdo->physical_zones[zone]);
	}

	for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
		dump_vdo_hash_zone(vdo->hash_zones[zone]);
	}
}

/**********************************************************************/
void assert_on_admin_thread(struct vdo *vdo, const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo_get_admin_thread(get_vdo_thread_config(vdo))),
			"%s called on admin thread",
			name);
}

/**********************************************************************/
void assert_on_logical_zone_thread(const struct vdo *vdo,
				   zone_count_t logical_zone,
				   const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo_get_logical_zone_thread(get_vdo_thread_config(vdo),
						     logical_zone)),
			"%s called on logical thread",
			name);
}

/**********************************************************************/
void assert_on_physical_zone_thread(const struct vdo *vdo,
				    zone_count_t physical_zone,
				    const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo_get_physical_zone_thread(get_vdo_thread_config(vdo),
						      physical_zone)),
			"%s called on physical thread",
			name);
}

/**********************************************************************/
struct hash_zone *select_hash_zone(const struct vdo *vdo,
				   const struct uds_chunk_name *name)
{
	/*
	 * Use a fragment of the chunk name as a hash code. To ensure uniform
	 * distributions, it must not overlap with fragments used elsewhere.
	 * Eight bits of hash should suffice since the number of hash zones is
	 * small.
	 */
	// XXX Make a central repository for these offsets ala hashUtils.
	// XXX Verify that the first byte is independent enough.
	uint32_t hash = name->name[0];

	/*
	 * Scale the 8-bit hash fragment to a zone index by treating it as a
	 * binary fraction and multiplying that by the zone count. If the hash
	 * is uniformly distributed over [0 .. 2^8-1], then (hash * count / 2^8)
	 * should be uniformly distributed over [0 .. count-1]. The multiply and
	 * shift is much faster than a divide (modulus) on X86 CPUs.
	 */
	return vdo->hash_zones[(hash * get_vdo_thread_config(vdo)->hash_zone_count) >> 8];
}

/**********************************************************************/
int get_physical_zone(const struct vdo *vdo,
		      physical_block_number_t pbn,
		      struct physical_zone **zone_ptr)
{
	struct vdo_slab *slab;
	int result;

	if (pbn == VDO_ZERO_BLOCK) {
		*zone_ptr = NULL;
		return VDO_SUCCESS;
	}

	// Used because it does a more restrictive bounds check than
	// get_vdo_slab(), and done first because it won't trigger read-only
	// mode on an invalid PBN.
	if (!vdo_is_physical_data_block(vdo->depot, pbn)) {
		return VDO_OUT_OF_RANGE;
	}

	// With the PBN already checked, we should always succeed in finding a
	// slab.
	slab = get_vdo_slab(vdo->depot, pbn);
	result =
		ASSERT(slab != NULL, "get_vdo_slab must succeed on all valid PBNs");
	if (result != VDO_SUCCESS) {
		return result;
	}

	*zone_ptr = vdo->physical_zones[get_vdo_slab_zone_number(slab)];
	return VDO_SUCCESS;
}

/**********************************************************************/
struct zoned_pbn
vdo_validate_dedupe_advice(struct vdo *vdo,
			   const struct data_location *advice,
			   logical_block_number_t lbn)
{
	struct zoned_pbn no_advice = { .pbn = VDO_ZERO_BLOCK };
	struct physical_zone *zone;
	int result;

	if (advice == NULL) {
		return no_advice;
	}

	// Don't use advice that's clearly meaningless.
	if ((advice->state == MAPPING_STATE_UNMAPPED) ||
	    (advice->pbn == VDO_ZERO_BLOCK)) {
		uds_log_debug("Invalid advice from deduplication server: pbn %llu, state %u. Giving up on deduplication of logical block %llu",
			      advice->pbn, advice->state, lbn);
		atomic64_inc(&vdo->error_stats.invalid_advice_pbn_count);
		return no_advice;
	}

	result = get_physical_zone(vdo, advice->pbn, &zone);
	if ((result != VDO_SUCCESS) || (zone == NULL)) {
		uds_log_debug("Invalid physical block number from deduplication server: %llu, giving up on deduplication of logical block %llu",
			      advice->pbn, lbn);
		atomic64_inc(&vdo->error_stats.invalid_advice_pbn_count);
		return no_advice;
	}

	return (struct zoned_pbn) {
		.pbn = advice->pbn,
		.state = advice->state,
		.zone = zone,
	};
}
