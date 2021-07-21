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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo.c#162 $
 */

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdoInternal.h"

#include <linux/device-mapper.h>

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
#include "syncCompletion.h"
#include "threadConfig.h"
#include "vdoComponentStates.h"
#include "vdoLayout.h"

#include "bio.h"
#include "dedupeIndex.h"
#include "ioSubmitter.h"
#include "kernelVDO.h"
#include "vdoCommon.h"
#include "workQueue.h"

/**********************************************************************/
static void finish_vdo(struct vdo *vdo)
{
	int i;

	finish_work_queue(vdo->cpu_queue);
	finish_work_queue(vdo->bio_ack_queue);
	cleanup_vdo_io_submitter(vdo->io_submitter);
	for (i = 0; i < vdo->initialized_thread_count; i++) {
		finish_work_queue(vdo->threads[i].request_queue);
	}

	free_buffer_pool(UDS_FORGET(vdo->data_vio_pool));
	finish_vdo_dedupe_index(vdo->dedupe_index);
	free_batch_processor(UDS_FORGET(vdo->data_vio_releaser));
}

/**********************************************************************/
void destroy_vdo(struct vdo *vdo)
{
	int i;
	const struct thread_config *thread_config = get_vdo_thread_config(vdo);

	finish_vdo(vdo);
	unregister_vdo(vdo);
	free_work_queue(UDS_FORGET(vdo->cpu_queue));
	free_work_queue(UDS_FORGET(vdo->bio_ack_queue));
	free_vdo_io_submitter(UDS_FORGET(vdo->io_submitter));
	free_vdo_dedupe_index(UDS_FORGET(vdo->dedupe_index));
	free_vdo_flusher(UDS_FORGET(vdo->flusher));
	free_vdo_packer(UDS_FORGET(vdo->packer));
	free_vdo_recovery_journal(UDS_FORGET(vdo->recovery_journal));
	free_vdo_slab_depot(UDS_FORGET(vdo->depot));
	free_vdo_layout(UDS_FORGET(vdo->layout));
	free_vdo_super_block(UDS_FORGET(vdo->super_block));
	free_vdo_block_map(UDS_FORGET(vdo->block_map));

	if (vdo->hash_zones != NULL) {
		zone_count_t zone;
		for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
			free_vdo_hash_zone(UDS_FORGET(vdo->hash_zones[zone]));
		}
	}
	UDS_FREE(vdo->hash_zones);
	vdo->hash_zones = NULL;

	free_vdo_logical_zones(UDS_FORGET(vdo->logical_zones));

	if (vdo->physical_zones != NULL) {
		zone_count_t zone;
		for (zone = 0; zone < thread_config->physical_zone_count; zone++) {
			free_vdo_physical_zone(UDS_FORGET(vdo->physical_zones[zone]));
		}
	}

	UDS_FREE(vdo->physical_zones);
	vdo->physical_zones = NULL;
	free_vdo_read_only_notifier(UDS_FORGET(vdo->read_only_notifier));
	free_vdo_thread_config(UDS_FORGET(vdo->thread_config));

	for (i = 0; i < vdo->initialized_thread_count; i++) {
		free_work_queue(UDS_FORGET(vdo->threads[i].request_queue));
	}
	UDS_FREE(vdo->threads);
	vdo->threads = NULL;

	for (i = 0; i < vdo->device_config->thread_counts.cpu_threads; i++) {
		UDS_FREE(UDS_FORGET(vdo->compression_context[i]));
	}

	UDS_FREE(UDS_FORGET(vdo->compression_context));
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
	was_compressing = set_vdo_compressing(vdo, false);
	// Now wait for there to be no active requests
	limiter_wait_for_idle(&vdo->request_limiter);
	// Reset the compression state after all requests are done
	if (was_compressing) {
		set_vdo_compressing(vdo, true);
	}
}

/**********************************************************************/
struct block_device *get_vdo_backing_device(const struct vdo *vdo)
{
	return vdo->device_config->owned_device->bdev;
}

/**********************************************************************/
int vdo_synchronous_flush(struct vdo *vdo)
{
	int result;
	struct bio bio;

	bio_init(&bio, 0, 0);
	bio_set_dev(&bio, get_vdo_backing_device(vdo));
	bio.bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
	submit_bio_wait(&bio);
	result = blk_status_to_errno(bio.bi_status);

	atomic64_inc(&vdo->stats.flush_out);
	if (result != 0) {
		uds_log_error_strerror(result, "synchronous flush failed");
		result = -EIO;
	}

	bio_uninit(&bio);
	return result;
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

/**
 * Callback to turn compression on or off.
 *
 * @param completion  The completion
 **/
static void set_compression_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	bool *enable = completion->parent;
	bool was_enabled = get_vdo_compressing(vdo);

	if (*enable != was_enabled) {
		WRITE_ONCE(vdo->compressing, *enable);
		if (was_enabled) {
			// Signal the packer to flush since compression has
			// been disabled.
			flush_vdo_packer(vdo->packer);
		}
	}

	uds_log_info("compression is %s", (*enable ? "enabled" : "disabled"));
	*enable = was_enabled;
	complete_vdo_completion(completion);
}

/**********************************************************************/
bool set_vdo_compressing(struct vdo *vdo, bool enable)
{
	thread_id_t packer_thread
		= vdo_get_packer_zone_thread(get_vdo_thread_config(vdo));
	perform_synchronous_vdo_action(vdo,
				       set_compression_callback,
				       packer_thread,
				       &enable);
	return enable;
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

/**********************************************************************/
static struct error_statistics __must_check
get_vdo_error_statistics(const struct vdo *vdo)
{
	/*
	 * The error counts can be incremented from arbitrary threads and so
	 * must be incremented atomically, but they are just statistics with no
	 * semantics that could rely on memory order, so unfenced reads are
	 * sufficient.
	 */
	const struct atomic_statistics *atoms = &vdo->stats;
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
static void copy_bio_stat(struct bio_stats *b,
			  const struct atomic_bio_stats *a)
{
	b->read = atomic64_read(&a->read);
	b->write = atomic64_read(&a->write);
	b->discard = atomic64_read(&a->discard);
	b->flush = atomic64_read(&a->flush);
	b->empty_flush = atomic64_read(&a->empty_flush);
	b->fua = atomic64_read(&a->fua);
}

/**********************************************************************/
static struct bio_stats subtract_bio_stats(struct bio_stats minuend,
					   struct bio_stats subtrahend)
{
	return (struct bio_stats) {
		.read = minuend.read - subtrahend.read,
		.write = minuend.write - subtrahend.write,
		.discard = minuend.discard - subtrahend.discard,
		.flush = minuend.flush - subtrahend.flush,
		.empty_flush = minuend.empty_flush - subtrahend.empty_flush,
		.fua = minuend.fua - subtrahend.fua,
	};
}


/**
 * Populate a vdo_statistics structure on the admin thread.
 *
 * @param vdo    The vdo
 * @param stats  The statistics structure to populate
 **/
static void get_vdo_statistics(const struct vdo *vdo,
			       struct vdo_statistics *stats)
{
	struct recovery_journal *journal = vdo->recovery_journal;
	enum vdo_state state = get_vdo_state(vdo);

	assert_on_admin_thread(vdo, __func__);

	// start with a clean slate
	memset(stats, 0, sizeof(struct vdo_statistics));

	// These are immutable properties of the vdo object, so it is safe to
	// query them from any thread.
	stats->version = STATISTICS_VERSION;
	stats->release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER;
	stats->logical_blocks = vdo->states.vdo.config.logical_blocks;
	// XXX config.physical_blocks is actually mutated during resize and is
	// in a packed structure, but resize runs on the admin thread so we're
	// usually OK.
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
	get_vdo_slab_depot_statistics(vdo->depot, stats);
	stats->journal = get_vdo_recovery_journal_statistics(journal);
	stats->packer = get_vdo_packer_statistics(vdo->packer);
	stats->block_map = get_vdo_block_map_statistics(vdo->block_map);
	stats->hash_lock = get_hash_lock_statistics(vdo);
	stats->errors = get_vdo_error_statistics(vdo);
	stats->in_recovery_mode = (state == VDO_RECOVERING);
	snprintf(stats->mode,
		 sizeof(stats->mode),
		 "%s",
		 describe_vdo_state(state));
	stats->version = STATISTICS_VERSION;
	stats->release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER;
	stats->instance = vdo->instance;

	stats->current_vios_in_progress =
		READ_ONCE(vdo->request_limiter.active);
	stats->max_vios = READ_ONCE(vdo->request_limiter.maximum);

	// get_vdo_dedupe_index_timeout_count() gives the number of timeouts,
	// and dedupe_context_busy gives the number of queries not made because
	// of earlier timeouts.
	stats->dedupe_advice_timeouts =
		(get_vdo_dedupe_index_timeout_count(vdo->dedupe_index) +
		 atomic64_read(&vdo->stats.dedupe_context_busy));
	stats->flush_out = atomic64_read(&vdo->stats.flush_out);
	stats->logical_block_size =
		vdo->device_config->logical_block_size;
	copy_bio_stat(&stats->bios_in, &vdo->stats.bios_in);
	copy_bio_stat(&stats->bios_in_partial, &vdo->stats.bios_in_partial);
	copy_bio_stat(&stats->bios_out, &vdo->stats.bios_out);
	copy_bio_stat(&stats->bios_meta, &vdo->stats.bios_meta);
	copy_bio_stat(&stats->bios_journal, &vdo->stats.bios_journal);
	copy_bio_stat(&stats->bios_page_cache, &vdo->stats.bios_page_cache);
	copy_bio_stat(&stats->bios_out_completed,
		      &vdo->stats.bios_out_completed);
	copy_bio_stat(&stats->bios_meta_completed,
		      &vdo->stats.bios_meta_completed);
	copy_bio_stat(&stats->bios_journal_completed,
		      &vdo->stats.bios_journal_completed);
	copy_bio_stat(&stats->bios_page_cache_completed,
		      &vdo->stats.bios_page_cache_completed);
	copy_bio_stat(&stats->bios_acknowledged, &vdo->stats.bios_acknowledged);
	copy_bio_stat(&stats->bios_acknowledged_partial,
		      &vdo->stats.bios_acknowledged_partial);
	stats->bios_in_progress =
		subtract_bio_stats(stats->bios_in, stats->bios_acknowledged);
	get_uds_memory_stats(&stats->memory_usage.bytes_used,
			     &stats->memory_usage.peak_bytes_used);
	get_vdo_dedupe_index_statistics(vdo->dedupe_index, &stats->index);
}

/**
 * Action to populate a vdo_statistics structure on the admin thread;
 * registered in fetch_vdo_statistics().
 *
 * @param completion  The completion
 **/
static void fetch_vdo_statistics_callback(struct vdo_completion *completion)
{
	get_vdo_statistics(completion->vdo, completion->parent);
	complete_vdo_completion(completion);
}

/***********************************************************************/
void fetch_vdo_statistics(struct vdo *vdo, struct vdo_statistics *stats)
{
	perform_synchronous_vdo_action(vdo,
				       fetch_vdo_statistics_callback,
				       vdo_get_admin_thread(get_vdo_thread_config(vdo)),
				       stats);
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
void assert_on_admin_thread(const struct vdo *vdo, const char *name)
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
	if ((advice->state == VDO_MAPPING_STATE_UNMAPPED) ||
	    (advice->pbn == VDO_ZERO_BLOCK)) {
		uds_log_debug("Invalid advice from deduplication server: pbn %llu, state %u. Giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn, advice->state,
			      (unsigned long long) lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return no_advice;
	}

	result = get_physical_zone(vdo, advice->pbn, &zone);
	if ((result != VDO_SUCCESS) || (zone == NULL)) {
		uds_log_debug("Invalid physical block number from deduplication server: %llu, giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn,
			      (unsigned long long) lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return no_advice;
	}

	return (struct zoned_pbn) {
		.pbn = advice->pbn,
		.state = advice->state,
		.zone = zone,
	};
}
