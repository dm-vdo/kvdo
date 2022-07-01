/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef HASH_ZONE_H
#define HASH_ZONE_H

#include <linux/list.h>

#include "uds.h"

#include "kernel-types.h"
#include "statistics.h"
#include "types.h"

struct hash_zone {
	/* Which hash zone this is */
	zone_count_t zone_number;

	/* The thread ID for this zone */
	thread_id_t thread_id;

	/* Mapping from chunk_name fields to hash_locks */
	struct pointer_map *hash_lock_map;

	/* List containing all unused hash_locks */
	struct list_head lock_pool;

	/*
	 * Statistics shared by all hash locks in this zone. Only modified on
	 * the hash zone thread, but queried by other threads.
	 */
	struct hash_lock_statistics statistics;

	/* Array of all hash_locks */
	struct hash_lock *lock_array;
};

struct hash_zones {
	/* The number of zones */
	zone_count_t zone_count;
	/* The hash zones themselves */
	struct hash_zone zones[];
};

int __must_check
vdo_make_hash_zones(struct vdo *vdo, struct hash_zones **zones_ptr);

void vdo_free_hash_zones(struct hash_zones *zones);

thread_id_t __must_check
vdo_get_hash_zone_thread_id(const struct hash_zone *zone);

struct hash_lock_statistics __must_check
vdo_get_hash_zone_statistics(const struct hash_zone *zone);

int __must_check
vdo_acquire_lock_from_hash_zone(struct hash_zone *zone,
				const struct uds_chunk_name *hash,
				struct hash_lock *replace_lock,
				struct hash_lock **lock_ptr);

void vdo_return_lock_to_hash_zone(struct hash_zone *zone,
				  struct hash_lock *lock);

void vdo_bump_hash_zone_valid_advice_count(struct hash_zone *zone);

void vdo_bump_hash_zone_stale_advice_count(struct hash_zone *zone);

void vdo_bump_hash_zone_data_match_count(struct hash_zone *zone);

void vdo_bump_hash_zone_collision_count(struct hash_zone *zone);

void vdo_dump_hash_zone(const struct hash_zone *zone);

#endif /* HASH_ZONE_H */
