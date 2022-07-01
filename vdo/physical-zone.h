/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PHYSICAL_ZONE_H
#define PHYSICAL_ZONE_H

#include "kernel-types.h"
#include "pbn-lock.h"
#include "types.h"

struct physical_zone {
	/** Which physical zone this is */
	zone_count_t zone_number;
	/** The thread ID for this zone */
	thread_id_t thread_id;
	/** In progress operations keyed by PBN */
	struct int_map *pbn_operations;
	/** Pool of unused pbn_lock instances */
	struct pbn_lock_pool *lock_pool;
	/** The block allocator for this zone */
	struct block_allocator *allocator;
	/** The next zone from which to attempt an allocation */
	struct physical_zone *next;
};

struct physical_zones {
	/** The number of zones */
	zone_count_t zone_count;
	/** The physical zones themselves */
	struct physical_zone zones[];
};

int __must_check
vdo_make_physical_zones(struct vdo *vdo, struct physical_zones **zones_ptr);

void vdo_free_physical_zones(struct physical_zones *zones);

struct pbn_lock * __must_check
vdo_get_physical_zone_pbn_lock(struct physical_zone *zone,
			       physical_block_number_t pbn);

int __must_check
vdo_attempt_physical_zone_pbn_lock(struct physical_zone *zone,
				   physical_block_number_t pbn,
				   enum pbn_lock_type type,
				   struct pbn_lock **lock_ptr);

bool __must_check vdo_allocate_block_in_zone(struct data_vio *data_vio);

void vdo_release_physical_zone_pbn_lock(struct physical_zone *zone,
					physical_block_number_t locked_pbn,
					struct pbn_lock *lock);

void vdo_dump_physical_zone(const struct physical_zone *zone);


#endif /* PHYSICAL_ZONE_H */
