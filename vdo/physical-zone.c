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

#include "physical-zone.h"

#include "memoryAlloc.h"
#include "permassert.h"

#include "block-allocator.h"
#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "data-vio.h"
#include "flush.h"
#include "hash-lock.h"
#include "int-map.h"
#include "pbn-lock.h"
#include "pbn-lock-pool.h"
#include "slab-depot.h"
#include "vdo.h"

enum {
	/*
	 * Each user data_vio needs a PBN read lock and write lock, and each 
	 * packer output bin has an allocating_vio that needs a PBN write lock. 
	 */
	LOCK_POOL_CAPACITY = 2 * MAXIMUM_VDO_USER_VIOS + DEFAULT_PACKER_OUTPUT_BINS,
};

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
};

/**
 * Create a physical zone.
 *
 * @param [in]  vdo          The vdo to which the zone will belong
 * @param [in]  zone_number  The number of the zone to create
 * @param [out] zone_ptr     A pointer to hold the new physical_zone
 *
 * @return VDO_SUCCESS or an error code
 **/
int make_vdo_physical_zone(struct vdo *vdo,
			   zone_count_t zone_number,
			   struct physical_zone **zone_ptr)
{
	struct physical_zone *zone;
	int result = UDS_ALLOCATE(1, struct physical_zone, __func__, &zone);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0, &zone->pbn_operations);
	if (result != VDO_SUCCESS) {
		free_vdo_physical_zone(zone);
		return result;
	}

	result = make_vdo_pbn_lock_pool(LOCK_POOL_CAPACITY, &zone->lock_pool);
	if (result != VDO_SUCCESS) {
		free_vdo_physical_zone(zone);
		return result;
	}

	zone->zone_number = zone_number;
	zone->thread_id =
		vdo_get_physical_zone_thread(vdo->thread_config, zone_number);
	zone->allocator =
		vdo_get_block_allocator_for_zone(vdo->depot, zone_number);

	*zone_ptr = zone;
	return VDO_SUCCESS;
}

/**
 * Free a physical zone.
 *
 * @param zone  The zone to free
 **/
void free_vdo_physical_zone(struct physical_zone *zone)
{
	if (zone == NULL) {
		return;
	}

	free_vdo_pbn_lock_pool(UDS_FORGET(zone->lock_pool));
	free_int_map(UDS_FORGET(zone->pbn_operations));
	UDS_FREE(zone);
}

/**
 * Get the zone number of a physical zone.
 *
 * @param zone  The zone
 *
 * @return The number of the zone
 **/
zone_count_t get_vdo_physical_zone_number(const struct physical_zone *zone)
{
	return zone->zone_number;
}

/**
 * Get the ID of a physical zone's thread.
 *
 * @param zone  The zone
 *
 * @return The zone's thread ID
 **/
thread_id_t get_vdo_physical_zone_thread_id(const struct physical_zone *zone)
{
	return zone->thread_id;
}

/**
 * Get the block allocator from a physical zone.
 *
 * @param zone  The zone
 *
 * @return The zone's allocator
 **/
struct block_allocator *
get_vdo_physical_zone_block_allocator(const struct physical_zone *zone)
{
	return zone->allocator;
}

/**
 * Get the lock on a PBN if one exists.
 *
 * @param zone  The physical zone responsible for the PBN
 * @param pbn   The physical block number whose lock is desired
 *
 * @return The lock or NULL if the PBN is not locked
 **/
struct pbn_lock *get_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
						physical_block_number_t pbn)
{
	return ((zone == NULL) ? NULL : int_map_get(zone->pbn_operations, pbn));
}

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
int attempt_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
				       physical_block_number_t pbn,
				       enum pbn_lock_type type,
				       struct pbn_lock **lock_ptr)
{
	/*
	 * Borrow and prepare a lock from the pool so we don't have to do two 
	 * int_map accesses in the common case of no lock contention. 
	 */
	struct pbn_lock *lock, *new_lock;
	int result = borrow_vdo_pbn_lock_from_pool(zone->lock_pool, type,
						   &new_lock);
	if (result != VDO_SUCCESS) {
		ASSERT_LOG_ONLY(false,
				"must always be able to borrow a PBN lock");
		return result;
	}

	result = int_map_put(zone->pbn_operations, pbn, new_lock, false,
			     (void **) &lock);
	if (result != VDO_SUCCESS) {
		return_vdo_pbn_lock_to_pool(zone->lock_pool, new_lock);
		return result;
	}

	if (lock != NULL) {
		/* The lock is already held, so we don't need the borrowed one. */
		return_vdo_pbn_lock_to_pool(zone->lock_pool,
					    UDS_FORGET(new_lock));
		result = ASSERT(lock->holder_count > 0,
				"physical block %llu lock held",
				(unsigned long long) pbn);
		if (result != VDO_SUCCESS) {
			return result;
		}
		*lock_ptr = lock;
	} else {
		*lock_ptr = new_lock;
	}
	return VDO_SUCCESS;
}

/**
 * Release a physical block lock if it is held and return it to the lock pool.
 * It must be the last live reference, as if the memory were being freed (the
 * lock memory will re-initialized or zeroed).
 *
 * @param zone        The physical zone in which the lock was obtained
 * @param locked_pbn  The physical block number to unlock
 * @param lock        The lock being released
 **/
void release_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
					physical_block_number_t locked_pbn,
					struct pbn_lock *lock)
{
	struct pbn_lock *holder;

	if (lock == NULL) {
		return;
	}

	ASSERT_LOG_ONLY(lock->holder_count > 0,
			"should not be releasing a lock that is not held");

	lock->holder_count -= 1;
	if (lock->holder_count > 0) {
		/*
		 * The lock was shared and is still referenced, so don't 
		 * release it yet. 
		 */
		return;
	}

	holder = int_map_remove(zone->pbn_operations, locked_pbn);
	ASSERT_LOG_ONLY((lock == holder),
			"physical block lock mismatch for block %llu",
			(unsigned long long) locked_pbn);

	release_vdo_pbn_lock_provisional_reference(lock, locked_pbn,
						   zone->allocator);
	return_vdo_pbn_lock_to_pool(zone->lock_pool, lock);
}

/**
 * Dump information about a physical zone to the log for debugging.
 *
 * @param zone   The zone to dump
 **/
void dump_vdo_physical_zone(const struct physical_zone *zone)
{
	dump_vdo_block_allocator(zone->allocator);
}
