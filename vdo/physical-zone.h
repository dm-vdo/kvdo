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

int __must_check vdo_initialize_physical_zone(struct vdo *vdo,
					      zone_count_t zone_number,
					      struct physical_zone *zone);

void vdo_destroy_physical_zone(struct physical_zone *zone);

struct pbn_lock * __must_check
vdo_get_physical_zone_pbn_lock(struct physical_zone *zone,
			       physical_block_number_t pbn);

int __must_check
vdo_attempt_physical_zone_pbn_lock(struct physical_zone *zone,
				   physical_block_number_t pbn,
				   enum pbn_lock_type type,
				   struct pbn_lock **lock_ptr);

int __must_check
vdo_allocate_and_lock_block(struct allocating_vio *allocating_vio);

void vdo_release_physical_zone_pbn_lock(struct physical_zone *zone,
					physical_block_number_t locked_pbn,
					struct pbn_lock *lock);

void vdo_dump_physical_zone(const struct physical_zone *zone);

#endif /* PHYSICAL_ZONE_H */
