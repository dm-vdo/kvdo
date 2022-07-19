// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "hash-zone.h"

#include <linux/list.h>

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "data-vio.h"
#include "hash-lock.h"
#include "kernel-types.h"
#include "pointer-map.h"
#include "statistics.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

enum {
	LOCK_POOL_CAPACITY = MAXIMUM_VDO_USER_VIOS,
};

/**
 * compare_keys() - Implements pointer_key_comparator.
 */
static bool compare_keys(const void *this_key, const void *that_key)
{
	/* Null keys are not supported. */
	return (memcmp(this_key, that_key, sizeof(struct uds_chunk_name)) == 0);
}

/**
 * hash_key() - Implements pointer_key_comparator.
 */
static uint32_t hash_key(const void *key)
{
	const struct uds_chunk_name *name = key;
	/*
	 * Use a fragment of the chunk name as a hash code. It must not overlap
	 * with fragments used elsewhere to ensure uniform distributions.
	 */
	/* XXX pick an offset in the chunk name that isn't used elsewhere */
	return get_unaligned_le32(&name->name[4]);
}

/**
 * vdo_make_hash_zones() - Create the hash zones.
 *
 * @vdo: The vdo to which the zone will belong.
 * @zones_ptr: A pointer to hold the zones.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_make_hash_zones(struct vdo *vdo, struct hash_zones **zones_ptr)
{
	int result;
	vio_count_t i;
	struct hash_zones *zones;
	zone_count_t zone_count = vdo->thread_config->hash_zone_count;

	if (zone_count == 0) {
		return VDO_SUCCESS;
	}

	result = UDS_ALLOCATE_EXTENDED(struct hash_zones,
				       zone_count,
				       struct hash_zone,
				       __func__,
				       &zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (zones->zone_count = 0;
	     zones->zone_count < zone_count;
	     zones->zone_count++) {
		struct hash_zone *zone = &zones->zones[zones->zone_count];

		result = make_pointer_map(VDO_LOCK_MAP_CAPACITY,
					  0,
					  compare_keys,
					  hash_key,
					  &zone->hash_lock_map);
		if (result != VDO_SUCCESS) {
			vdo_free_hash_zones(zones);
			return result;
		}

		zone->zone_number = zones->zone_count;
		zone->thread_id = vdo_get_hash_zone_thread(vdo->thread_config,
							   zone->zone_number);
		INIT_LIST_HEAD(&zone->lock_pool);

		result = UDS_ALLOCATE(LOCK_POOL_CAPACITY,
				      struct hash_lock,
				      "hash_lock array",
				      &zone->lock_array);
		if (result != VDO_SUCCESS) {
			zones->zone_count++;
			vdo_free_hash_zones(zones);
			return result;
		}

		for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
			struct hash_lock *lock = &zone->lock_array[i];

			vdo_initialize_hash_lock(lock);
			list_add_tail(&lock->pool_node, &zone->lock_pool);
		}

		result = vdo_make_default_thread(vdo, zone->thread_id);
	}

	*zones_ptr = zones;
	return VDO_SUCCESS;
}

/**
 * vdo_free_hash_zones() - Free the hash zones.
 * @zones: The zone to free.
 */
void vdo_free_hash_zones(struct hash_zones *zones)
{
	zone_count_t i;

	if (zones == NULL) {
		return;
	}

	for (i = 0; i < zones->zone_count; i++) {
		struct hash_zone *zone = &zones->zones[i];

		free_pointer_map(UDS_FORGET(zone->hash_lock_map));
		UDS_FREE(UDS_FORGET(zone->lock_array));
	}

	UDS_FREE(zones);
}

/**
 * vdo_get_hash_zone_thread_id() - Get the ID of a hash zone's thread.
 * @zone: The zone.
 *
 * Return: The zone's thread ID.
 */
thread_id_t vdo_get_hash_zone_thread_id(const struct hash_zone *zone)
{
	return zone->thread_id;
}

/**
 * vdo_get_hash_zone_statistics() - Get the statistics for this hash zone.
 * @zone: The hash zone to query.
 *
 * Return: A copy of the current statistics for the hash zone.
 */
struct hash_lock_statistics
vdo_get_hash_zone_statistics(const struct hash_zone *zone)
{
	const struct hash_lock_statistics *stats = &zone->statistics;

	return (struct hash_lock_statistics) {
		.dedupe_advice_valid =
			READ_ONCE(stats->dedupe_advice_valid),
		.dedupe_advice_stale =
			READ_ONCE(stats->dedupe_advice_stale),
		.concurrent_data_matches =
			READ_ONCE(stats->concurrent_data_matches),
		.concurrent_hash_collisions =
			READ_ONCE(stats->concurrent_hash_collisions),
	};
}

/**
 * return_hash_lock_to_pool() - Return a hash lock to the zone's pool.
 * @zone: The zone from which the lock was borrowed.
 * @lock: The lock that is no longer in use.
 */
static void return_hash_lock_to_pool(struct hash_zone *zone,
				     struct hash_lock *lock)
{
	memset(lock, 0, sizeof(*lock));
	vdo_initialize_hash_lock(lock);
	list_add_tail(&lock->pool_node, &zone->lock_pool);
}

/**
 * vdo_acquire_lock_from_hash_zone() - Get the lock for a chunk name.
 * @zone: The zone responsible for the hash.
 * @hash: The hash to lock.
 * @replace_lock:  If non-NULL, the lock already registered for the
 *                 hash which should be replaced by the new lock.
 * @lock_ptr: A pointer to receive the hash lock.
 *
 * Gets the lock for the hash (chunk name) of the data in a data_vio, or if
 * one does not exist (or if we are explicitly rolling over), initialize a new
 * lock for the hash and register it in the zone. This must only be called in
 * the correct thread for the zone.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_acquire_lock_from_hash_zone(struct hash_zone *zone,
				    const struct uds_chunk_name *hash,
				    struct hash_lock *replace_lock,
				    struct hash_lock **lock_ptr)
{
	struct hash_lock *lock, *new_lock;

	/*
	 * Borrow and prepare a lock from the pool so we don't have to do two
	 * pointer_map accesses in the common case of no lock contention.
	 */
	int result = ASSERT(!list_empty(&zone->lock_pool),
			    "never need to wait for a free hash lock");
	if (result != VDO_SUCCESS) {
		return result;
	}
	new_lock = list_entry(zone->lock_pool.prev, struct hash_lock,
			      pool_node);
	list_del_init(&new_lock->pool_node);

	/*
	 * Fill in the hash of the new lock so we can map it, since we have to
	 * use the hash as the map key.
	 */
	new_lock->hash = *hash;

	result = pointer_map_put(zone->hash_lock_map, &new_lock->hash, new_lock,
				 (replace_lock != NULL), (void **) &lock);
	if (result != VDO_SUCCESS) {
		return_hash_lock_to_pool(zone, UDS_FORGET(new_lock));
		return result;
	}

	if (replace_lock != NULL) {
		/*
		 * XXX on mismatch put the old lock back and return a severe
		 * error
		 */
		ASSERT_LOG_ONLY(lock == replace_lock,
				"old lock must have been in the lock map");
		/* XXX check earlier and bail out? */
		ASSERT_LOG_ONLY(replace_lock->registered,
				"old lock must have been marked registered");
		replace_lock->registered = false;
	}

	if (lock == replace_lock) {
		lock = new_lock;
		lock->registered = true;
	} else {
		/*
		 * There's already a lock for the hash, so we don't need the
		 * borrowed lock.
		 */
		return_hash_lock_to_pool(zone, UDS_FORGET(new_lock));
	}

	*lock_ptr = lock;
	return VDO_SUCCESS;
}

/**
 * vdo_return_lock_to_hash_zone() - Return a hash lock.
 * @zone: The zone from which the lock was borrowed.
 * @lock: The lock that is no longer in use.
 *
 * Returns a hash lock to the zone it was borrowed from, remove it from the
 * zone's lock map, and return it to the pool. This must only be called when
 * the lock has been completely released, and only in the correct thread for
 * the zone.
 */
void vdo_return_lock_to_hash_zone(struct hash_zone *zone,
				  struct hash_lock *lock)
{
	if (lock->registered) {
		struct hash_lock *removed =
			pointer_map_remove(zone->hash_lock_map, &lock->hash);
		ASSERT_LOG_ONLY(lock == removed,
				"hash lock being released must have been mapped");
	} else {
		ASSERT_LOG_ONLY(lock != pointer_map_get(zone->hash_lock_map,
							&lock->hash),
				"unregistered hash lock must not be in the lock map");
	}

	ASSERT_LOG_ONLY(!has_waiters(&lock->waiters),
			"hash lock returned to zone must have no waiters");
	ASSERT_LOG_ONLY((lock->duplicate_lock == NULL),
			"hash lock returned to zone must not reference a PBN lock");
	ASSERT_LOG_ONLY((lock->state == VDO_HASH_LOCK_DESTROYING),
			"returned hash lock must not be in use with state %s",
			vdo_get_hash_lock_state_name(lock->state));
	ASSERT_LOG_ONLY(list_empty(&lock->pool_node),
			"hash lock returned to zone must not be in a pool ring");
	ASSERT_LOG_ONLY(list_empty(&lock->duplicate_ring),
			"hash lock returned to zone must not reference DataVIOs");

	return_hash_lock_to_pool(zone, lock);
}

/**
 * dump_hash_lock() - Dump a compact description of hash_lock to the log if
 *                    the lock is not on the free list.
 * @lock: The hash lock to dump.
 */
static void dump_hash_lock(const struct hash_lock *lock)
{
	const char *state;

	if (!list_empty(&lock->pool_node)) {
		/* This lock is on the free list. */
		return;
	}

	/*
	 * Necessarily cryptic since we can log a lot of these. First three
	 * chars of state is unambiguous. 'U' indicates a lock not registered in
	 * the map.
	 */
	state = vdo_get_hash_lock_state_name(lock->state);
	uds_log_info("  hl %px: %3.3s %c%llu/%u rc=%u wc=%zu agt=%px",
		     (const void *) lock, state, (lock->registered ? 'D' : 'U'),
		     (unsigned long long) lock->duplicate.pbn,
		     lock->duplicate.state, lock->reference_count,
		     count_waiters(&lock->waiters), (void *) lock->agent);
}

/**
 * increment_stat() - Increment a statistic counter in a non-atomic yet
 *                    thread-safe manner.
 * @stat: The statistic field to increment.
 */
static void increment_stat(uint64_t *stat)
{
	/*
	 * Must only be mutated on the hash zone thread. Prevents any compiler
	 * shenanigans from affecting other threads reading stats.
	 */
	WRITE_ONCE(*stat, *stat + 1);
}

/**
 * vdo_bump_hash_zone_valid_advice_count() - Increment the valid advice count
 *                                           in the hash zone statistics.
 * @zone: The hash zone of the lock that received valid advice.
 *
 * Context: Must only be called from the hash zone thread.
 */
void vdo_bump_hash_zone_valid_advice_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.dedupe_advice_valid);
}

/**
 * vdo_bump_hash_zone_stale_advice_count() - Increment the stale advice count
 *                                           in the hash zone statistics.
 * @zone: The hash zone of the lock that received stale advice.
 *
 * Context: Must only be called from the hash zone thread.
 */
void vdo_bump_hash_zone_stale_advice_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.dedupe_advice_stale);
}

/**
 * vdo_bump_hash_zone_data_match_count() - Increment the concurrent dedupe
 *                                         count in the hash zone statistics.
 * @zone: The hash zone of the lock that matched a new data_vio.
 *
 * Context: Must only be called from the hash zone thread.
 */
void vdo_bump_hash_zone_data_match_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.concurrent_data_matches);
}

/**
 * vdo_bump_hash_zone_collision_count() - Increment the concurrent hash
 *                                        collision count in the hash zone
 *                                        statistics.
 * @zone: The hash zone of the lock that rejected a colliding data_vio.
 *
 * Context: Must only be called from the hash zone thread.
 */
void vdo_bump_hash_zone_collision_count(struct hash_zone *zone)
{
	increment_stat(&zone->statistics.concurrent_hash_collisions);
}

/**
 * vdo_dump_hash_zone() - Dump information about a hash zone to the log for
 *                        debugging.
 * @zone: The zone to dump.
 */
void vdo_dump_hash_zone(const struct hash_zone *zone)
{
	vio_count_t i;

	if (zone->hash_lock_map == NULL) {
		uds_log_info("struct hash_zone %u: NULL map", zone->zone_number);
		return;
	}

	uds_log_info("struct hash_zone %u: mapSize=%zu", zone->zone_number,
		     pointer_map_size(zone->hash_lock_map));
	for (i = 0; i < LOCK_POOL_CAPACITY; i++) {
		dump_hash_lock(&zone->lock_array[i]);
	}
}
