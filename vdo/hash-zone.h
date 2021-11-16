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

#ifndef HASH_ZONE_H
#define HASH_ZONE_H

#include "uds.h"

#include "kernel-types.h"
#include "statistics.h"
#include "types.h"

/**
 * Create a hash zone.
 *
 * @param [in]  vdo          The vdo to which the zone will belong
 * @param [in]  zone_number  The number of the zone to create
 * @param [out] zone_ptr     A pointer to hold the new hash_zone
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check vdo_make_hash_zone(struct vdo *vdo,
				    zone_count_t zone_number,
				    struct hash_zone **zone_ptr);

void vdo_free_hash_zone(struct hash_zone *zone);

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
