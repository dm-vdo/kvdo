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

#ifndef LOGICAL_ZONE_H
#define LOGICAL_ZONE_H

#include "admin-state.h"
#include "int-map.h"
#include "types.h"

struct logical_zone * __must_check
get_vdo_logical_zone(struct logical_zones *zones, zone_count_t zone_number);

int __must_check
make_vdo_logical_zones(struct vdo *vdo, struct logical_zones **zones_ptr);

void free_vdo_logical_zones(struct logical_zones *zones);

void drain_vdo_logical_zones(struct logical_zones *zones,
			     const struct admin_state_code *operation,
			     struct vdo_completion *completion);

void resume_vdo_logical_zones(struct logical_zones *zones,
			      struct vdo_completion *parent);

thread_id_t __must_check
get_vdo_logical_zone_thread_id(const struct logical_zone *zone);

struct block_map_zone * __must_check
get_vdo_logical_zone_block_map(const struct logical_zone *zone);

struct int_map * __must_check
get_vdo_logical_zone_lbn_lock_map(const struct logical_zone *zone);

struct logical_zone * __must_check
get_next_vdo_logical_zone(const struct logical_zone *zone);

void
increment_vdo_logical_zone_flush_generation(struct logical_zone *zone,
					    sequence_number_t expected_generation);

sequence_number_t __must_check
get_vdo_logical_zone_oldest_locked_generation(const struct logical_zone *zone);

int __must_check acquire_vdo_flush_generation_lock(struct data_vio *data_vio);

void release_vdo_flush_generation_lock(struct data_vio *data_vio);

struct allocation_selector * __must_check
get_vdo_logical_zone_allocation_selector(struct logical_zone *zone);

void dump_vdo_logical_zone(const struct logical_zone *zone);

#endif /* LOGICAL_ZONE_H */
