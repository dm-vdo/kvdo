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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/physicalZone.c#33 $
 */

#include "physicalZone.h"

#include "memoryAlloc.h"
#include "permassert.h"

#include "blockAllocator.h"
#include "blockMap.h"
#include "completion.h"
#include "constants.h"
#include "dataVIO.h"
#include "flush.h"
#include "hashLock.h"
#include "intMap.h"
#include "pbnLock.h"
#include "pbnLockPool.h"
#include "slabDepot.h"
#include "vdoInternal.h"

enum {
	// Each user data_vio needs a PBN read lock and write lock, and each
	// packer output bin has an allocating_vio that needs a PBN write lock.
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

/**********************************************************************/
int make_vdo_physical_zone(struct vdo *vdo,
			   zone_count_t zone_number,
			   struct physical_zone **zone_ptr)
{
	struct physical_zone *zone;
	int result = ALLOCATE(1, struct physical_zone, __func__, &zone);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0, &zone->pbn_operations);
	if (result != VDO_SUCCESS) {
		free_vdo_physical_zone(&zone);
		return result;
	}

	result = make_vdo_pbn_lock_pool(LOCK_POOL_CAPACITY, &zone->lock_pool);
	if (result != VDO_SUCCESS) {
		free_vdo_physical_zone(&zone);
		return result;
	}

	zone->zone_number = zone_number;
	zone->thread_id =
		vdo_get_physical_zone_thread(get_vdo_thread_config(vdo),
					     zone_number);
	zone->allocator =
		vdo_get_block_allocator_for_zone(vdo->depot, zone_number);

	*zone_ptr = zone;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_physical_zone(struct physical_zone **zone_ptr)
{
	struct physical_zone *zone;

	if (*zone_ptr == NULL) {
		return;
	}
	zone = *zone_ptr;
	free_vdo_pbn_lock_pool(&zone->lock_pool);
	free_int_map(FORGET(zone->pbn_operations));
	FREE(zone);
	*zone_ptr = NULL;
}

/**********************************************************************/
zone_count_t get_vdo_physical_zone_number(const struct physical_zone *zone)
{
	return zone->zone_number;
}

/**********************************************************************/
thread_id_t get_vdo_physical_zone_thread_id(const struct physical_zone *zone)
{
	return zone->thread_id;
}

/**********************************************************************/
struct block_allocator *
get_vdo_physical_zone_block_allocator(const struct physical_zone *zone)
{
	return zone->allocator;
}

/**********************************************************************/
struct pbn_lock *get_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
						physical_block_number_t pbn)
{
	return ((zone == NULL) ? NULL : int_map_get(zone->pbn_operations, pbn));
}

/**********************************************************************/
int attempt_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
				       physical_block_number_t pbn,
				       enum pbn_lock_type type,
				       struct pbn_lock **lock_ptr)
{
	// Borrow and prepare a lock from the pool so we don't have to do two
	// int_map accesses in the common case of no lock contention.
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
		return_vdo_pbn_lock_to_pool(zone->lock_pool, &new_lock);
		return result;
	}

	if (lock != NULL) {
		// The lock is already held, so we don't need the borrowed lock.
		return_vdo_pbn_lock_to_pool(zone->lock_pool, &new_lock);

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

/**********************************************************************/
void release_vdo_physical_zone_pbn_lock(struct physical_zone *zone,
					physical_block_number_t locked_pbn,
					struct pbn_lock **lock_ptr)
{
	struct pbn_lock *holder, *lock = *lock_ptr;
	if (lock == NULL) {
		return;
	}
	*lock_ptr = NULL;

	ASSERT_LOG_ONLY(lock->holder_count > 0,
			"should not be releasing a lock that is not held");

	lock->holder_count -= 1;
	if (lock->holder_count > 0) {
		// The lock was shared and is still referenced, so don't
		// release it yet.
		return;
	}

	holder = int_map_remove(zone->pbn_operations, locked_pbn);
	ASSERT_LOG_ONLY((lock == holder),
			"physical block lock mismatch for block %llu",
			(unsigned long long) locked_pbn);

	release_vdo_pbn_lock_provisional_reference(lock, locked_pbn,
						   zone->allocator);

	return_vdo_pbn_lock_to_pool(zone->lock_pool, &lock);
}

/**********************************************************************/
void dump_vdo_physical_zone(const struct physical_zone *zone)
{
	dump_vdo_block_allocator(zone->allocator);
}
