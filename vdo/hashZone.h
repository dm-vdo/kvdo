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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashZone.h#11 $
 */

#ifndef HASH_ZONE_H
#define HASH_ZONE_H

#include "uds.h"

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
int __must_check make_vdo_hash_zone(struct vdo *vdo,
				    zone_count_t zone_number,
				    struct hash_zone **zone_ptr);

/**
 * Free a hash zone and null out the reference to it.
 *
 * @param zone_ptr  A pointer to the zone to free
 **/
void free_vdo_hash_zone(struct hash_zone **zone_ptr);

/**
 * Get the zone number of a hash zone.
 *
 * @param zone  The zone
 *
 * @return The number of the zone
 **/
zone_count_t __must_check
get_vdo_hash_zone_number(const struct hash_zone *zone);

/**
 * Get the ID of a hash zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
thread_id_t __must_check
get_vdo_hash_zone_thread_id(const struct hash_zone *zone);

/**
 * Get the statistics for this hash zone.
 *
 * @param zone  The hash zone to query
 *
 * @return A copy of the current statistics for the hash zone
 **/
struct hash_lock_statistics __must_check
get_vdo_hash_zone_statistics(const struct hash_zone *zone);

/**
 * Get the lock for the hash (chunk name) of the data in a data_vio, or if one
 * does not exist (or if we are explicitly rolling over), initialize a new
 * lock for the hash and register it in the zone. This must only be called in
 * the correct thread for the zone.
 *
 * @param [in]  zone          The zone responsible for the hash
 * @param [in]  hash          The hash to lock
 * @param [in]  replace_lock  If non-NULL, the lock already registered for the
 *                            hash which should be replaced by the new lock
 * @param [out] lock_ptr      A pointer to receive the hash lock
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
acquire_lock_from_vdo_hash_zone(struct hash_zone *zone,
				const struct uds_chunk_name *hash,
				struct hash_lock *replace_lock,
				struct hash_lock **lock_ptr);

/**
 * Return a hash lock to the zone it was borrowed from, remove it from the
 * zone's lock map, returning it to the pool, and nulling out the reference to
 * it. This must only be called when the lock has been completely released,
 * and only in the correct thread for the zone.
 *
 * @param [in]     zone      The zone from which the lock was borrowed
 * @param [in,out] lock_ptr  The lock that is no longer in use
 **/
void return_lock_to_vdo_hash_zone(struct hash_zone *zone,
				  struct hash_lock **lock_ptr);

/**
 * Increment the valid advice count in the hash zone statistics.
 * Must only be called from the hash zone thread.
 *
 * @param zone  The hash zone of the lock that received valid advice
 **/
void bump_vdo_hash_zone_valid_advice_count(struct hash_zone *zone);

/**
 * Increment the stale advice count in the hash zone statistics.
 * Must only be called from the hash zone thread.
 *
 * @param zone  The hash zone of the lock that received stale advice
 **/
void bump_vdo_hash_zone_stale_advice_count(struct hash_zone *zone);

/**
 * Increment the concurrent dedupe count in the hash zone statistics.
 * Must only be called from the hash zone thread.
 *
 * @param zone  The hash zone of the lock that matched a new data_vio
 **/
void bump_vdo_hash_zone_data_match_count(struct hash_zone *zone);

/**
 * Increment the concurrent hash collision count in the hash zone statistics.
 * Must only be called from the hash zone thread.
 *
 * @param zone  The hash zone of the lock that rejected a colliding data_vio
 **/
void bump_vdo_hash_zone_collision_count(struct hash_zone *zone);

/**
 * Dump information about a hash zone to the log for debugging.
 *
 * @param zone   The zone to dump
 **/
void dump_vdo_hash_zone(const struct hash_zone *zone);

#endif // HASH_ZONE_H
