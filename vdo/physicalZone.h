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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/physicalZone.h#1 $
 */

#ifndef PHYSICAL_ZONE_H
#define PHYSICAL_ZONE_H

#include "pbnLock.h"
#include "types.h"

/**
 * Create a physical zone.
 *
 * @param [in]  vdo          The vdo to which the zone will belong
 * @param [in]  zone_number  The number of the zone to create
 * @param [out] zone_ptr     A pointer to hold the new physical_zone
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check make_physical_zone(struct vdo *vdo,
				    zone_count_t zone_number,
				    struct physical_zone **zone_ptr);

/**
 * Free a physical zone and null out the reference to it.
 *
 * @param zone_ptr  A pointer to the zone to free
 **/
void free_physical_zone(struct physical_zone **zone_ptr);

/**
 * Get the zone number of a physical zone.
 *
 * @param zone  The zone
 *
 * @return The number of the zone
 **/
zone_count_t __must_check
get_physical_zone_number(const struct physical_zone *zone);

/**
 * Get the ID of a physical zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
thread_id_t __must_check
get_physical_zone_thread_id(const struct physical_zone *zone);

/**
 * Get the block allocator from a physical zone.
 *
 * @param zone  The zone
 *
 * @return The zone's allocator
 **/
struct block_allocator * __must_check
get_block_allocator(const struct physical_zone *zone);

/**
 * Get the lock on a PBN if one exists.
 *
 * @param zone  The physical zone responsible for the PBN
 * @param pbn   The physical block number whose lock is desired
 *
 * @return The lock or NULL if the PBN is not locked
 **/
struct pbn_lock * __must_check
get_pbn_lock(struct physical_zone *zone, physical_block_number_t pbn);

/**
 * Attempt to lock a physical block in the zone responsible for it. If the PBN
 * is already locked, the existing lock will be returned. Otherwise, a new
 * lock instance will be borrowed from the pool, initialized, and returned.
 * The lock owner will be NULL for a new lock acquired by the caller, who is
 * responsible for setting that field promptly. The lock owner will be
 * non-NULL when there is already an existing lock on the PBN.
 *
 * @param [in]  zone      The physical zone responsible for the PBN
 * @param [in]  pbn       The physical block number to lock
 * @param [in]  type      The type with which to initialize a new lock
 * @param [out] lock_ptr  A pointer to receive the lock, existing or new
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check attempt_pbn_lock(struct physical_zone *zone,
				  physical_block_number_t pbn,
				  enum pbn_lock_type type,
				  struct pbn_lock **lock_ptr);

/**
 * Release a physical block lock if it is held, return it to the lock pool,
 * and null out the caller's reference to it. It must be the last live
 * reference, as if the memory were being freed (the lock memory will
 * re-initialized or zeroed).
 *
 * @param [in]     zone        The physical zone in which the lock was obtained
 * @param [in]     locked_pbn  The physical block number to unlock
 * @param [in,out] lock_ptr    The last reference to the lock being released
 **/
void release_pbn_lock(struct physical_zone *zone,
		      physical_block_number_t locked_pbn,
		      struct pbn_lock **lock_ptr);

/**
 * Dump information about a physical zone to the log for debugging.
 *
 * @param zone   The zone to dump
 **/
void dump_physical_zone(const struct physical_zone *zone);

#endif // PHYSICAL_ZONE_H
