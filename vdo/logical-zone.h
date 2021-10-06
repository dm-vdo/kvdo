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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/logical-zone.h#1 $
 */

#ifndef LOGICAL_ZONE_H
#define LOGICAL_ZONE_H

#include "admin-state.h"
#include "int-map.h"
#include "types.h"

/**
 * Get a logical zone by number.
 *
 * @param zones        A set of logical zones
 * @param zone_number  The number of the zone to get
 *
 * @return The requested zone
 **/
struct logical_zone * __must_check
get_vdo_logical_zone(struct logical_zones *zones, zone_count_t zone_number);

/**
 * Create a set of logical zones.
 *
 * @param [in]  vdo        The vdo to which the zones will belong
 * @param [out] zones_ptr  A pointer to hold the new zones
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
make_vdo_logical_zones(struct vdo *vdo, struct logical_zones **zones_ptr);

/**
 * Free a set of logical zones.
 *
 * @param zones The set of zones to free
 **/
void free_vdo_logical_zones(struct logical_zones *zones);

/**
 * Drain a set of logical zones.
 *
 * @param zones       The logical zones to suspend
 * @param operation   The type of drain to perform
 * @param completion  The object to notify when the zones are suspended
 **/
void drain_vdo_logical_zones(struct logical_zones *zones,
			     const struct admin_state_code *operation,
			     struct vdo_completion *completion);

/**
 * Resume a set of logical zones.
 *
 * @param zones   The logical zones to resume
 * @param parent  The object to notify when the zones have resumed
 **/
void resume_vdo_logical_zones(struct logical_zones *zones,
			      struct vdo_completion *parent);

/**
 * Get the ID of a logical zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
thread_id_t __must_check
get_vdo_logical_zone_thread_id(const struct logical_zone *zone);

/**
 * Get the portion of the block map for this zone.
 *
 * @param zone  The zone
 *
 * @return The block map zone
 **/
struct block_map_zone * __must_check
get_vdo_logical_zone_block_map(const struct logical_zone *zone);

/**
 * Get the logical lock map for this zone.
 *
 * @param zone  The zone
 *
 * @return The logical lock map for the zone
 **/
struct int_map * __must_check
get_vdo_logical_zone_lbn_lock_map(const struct logical_zone *zone);

/**
 * Get the next-highest-numbered logical zone, or <code>NULL</code> if the
 * zone is the highest-numbered zone in its vdo.
 *
 * @param zone  The logical zone to query
 *
 * @return The logical zone whose zone number is one greater than the given
 *         zone, or <code>NULL</code> if there is no such zone
 **/
struct logical_zone * __must_check
get_next_vdo_logical_zone(const struct logical_zone *zone);

/**
 * Increment the flush generation in a logical zone.
 *
 * @param zone                 The logical zone
 * @param expected_generation  The expected value of the flush generation
 *                             before the increment
 **/
void
increment_vdo_logical_zone_flush_generation(struct logical_zone *zone,
					    sequence_number_t expected_generation);

/**
 * Get the oldest flush generation which is locked by a logical zone.
 *
 * @param zone   The logical zone
 *
 * @return The oldest generation locked by the zone
 **/
sequence_number_t __must_check
get_vdo_logical_zone_oldest_locked_generation(const struct logical_zone *zone);

/**
 * Acquire the shared lock on a flush generation by a write data_vio.
 *
 * @param data_vio   The data_vio
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check acquire_vdo_flush_generation_lock(struct data_vio *data_vio);

/**
 * Release the shared lock on a flush generation held by a write data_vio. If
 * there are pending flushes, and this data_vio completes the oldest generation
 * active in this zone, an attempt will be made to finish any flushes which may
 * now be complete.
 *
 * @param data_vio  The data_vio whose lock is to be released
 **/
void release_vdo_flush_generation_lock(struct data_vio *data_vio);

/**
 * Get the selector for deciding which physical zone should be allocated from
 * next for activities in a logical zone.
 *
 * @param zone  The logical zone of the operation which needs an allocation
 *
 * @return The allocation selector for this zone
 **/
struct allocation_selector * __must_check
get_vdo_logical_zone_allocation_selector(struct logical_zone *zone);

/**
 * Dump information about a logical zone to the log for debugging, in a
 * thread-unsafe fashion.
 *
 * @param zone   The zone to dump
 **/
void dump_vdo_logical_zone(const struct logical_zone *zone);

#endif // LOGICAL_ZONE_H
