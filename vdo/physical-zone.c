// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "physical-zone.h"

#include "memory-alloc.h"
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
#include "slab-scrubber.h"
#include "vdo.h"

enum {
	/* Each user data_vio needs a PBN read lock and write lock. */
	LOCK_POOL_CAPACITY = 2 * MAXIMUM_VDO_USER_VIOS,
};

/**
 * initialize_zone() - Initialize a physical zone.
 * @vdo: The vdo to which the zone will belong.
 * @zones: The physical_zones to which the zone being initialized belongs
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int initialize_zone(struct vdo *vdo,
			   struct physical_zones *zones)
{
	int result;
	zone_count_t zone_number = zones->zone_count;
	struct physical_zone *zone = &zones->zones[zone_number];

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0, &zone->pbn_operations);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_pbn_lock_pool(LOCK_POOL_CAPACITY, &zone->lock_pool);
	if (result != VDO_SUCCESS) {
		free_int_map(zone->pbn_operations);
		return result;
	}

	zone->zone_number = zone_number;
	zone->thread_id = vdo->thread_config->physical_threads[zone_number];
	zone->allocator = vdo->depot->allocators[zone_number];
	zone->next = &zones->zones[(zone_number + 1) %
				   vdo->thread_config->physical_zone_count];
	result = vdo_make_default_thread(vdo, zone->thread_id);
	if (result != VDO_SUCCESS) {
		vdo_free_pbn_lock_pool(UDS_FORGET(zone->lock_pool));
		free_int_map(zone->pbn_operations);
		return result;
	}
	return result;
}

/**
 * vdo_make_physical_zones() - Make the physical zones for a vdo.
 * @vdo: The vdo being constructed
 * @zones_ptr: A pointer to hold the zones
 *
 * Return: VDO_SUCCESS or an error code.
 **/
int vdo_make_physical_zones(struct vdo *vdo, struct physical_zones **zones_ptr)
{
	struct physical_zones *zones;
	int result;
	zone_count_t zone_count = vdo->thread_config->physical_zone_count;

	if (zone_count == 0) {
		return VDO_SUCCESS;
	}

	result = UDS_ALLOCATE_EXTENDED(struct physical_zones,
				       zone_count,
				       struct physical_zone,
				       __func__,
				       &zones);
	if (result != VDO_SUCCESS) {
		return result;
	}

	for (zones->zone_count = 0;
	     zones->zone_count < zone_count;
	     zones->zone_count++) {
		result = initialize_zone(vdo, zones);
		if (result != VDO_SUCCESS) {
			vdo_free_physical_zones(zones);
			return result;
		}
	}

	*zones_ptr = zones;
	return VDO_SUCCESS;
}

/**
 * vdo_free_physical_zones() - Destroy the physical zones.
 * @zones: The zones to free.
 */
void vdo_free_physical_zones(struct physical_zones *zones)
{
	zone_count_t index;

	if (zones == NULL) {
		return;
	}

	for (index = 0; index < zones->zone_count; index++) {
		struct physical_zone *zone = &zones->zones[index];

		vdo_free_pbn_lock_pool(UDS_FORGET(zone->lock_pool));
		free_int_map(UDS_FORGET(zone->pbn_operations));
	}

	UDS_FREE(zones);
}

/**
 * vdo_get_physical_zone_pbn_lock() - Get the lock on a PBN if one exists.
 * @zone: The physical zone responsible for the PBN.
 * @pbn: The physical block number whose lock is desired.
 *
 * Return: The lock or NULL if the PBN is not locked.
 */
struct pbn_lock *vdo_get_physical_zone_pbn_lock(struct physical_zone *zone,
						physical_block_number_t pbn)
{
	return ((zone == NULL) ? NULL : int_map_get(zone->pbn_operations, pbn));
}

/**
 * vdo_attempt_physical_zone_pbn_lock() - Attempt to lock a physical block in
 *                                        the zone responsible for it.
 * @zone: The physical zone responsible for the PBN.
 * @pbn: The physical block number to lock.
 * @type: The type with which to initialize a new lock.
 * @lock_ptr:  A pointer to receive the lock, existing or new.
 *
 * If the PBN is already locked, the existing lock will be returned.
 * Otherwise, a new lock instance will be borrowed from the pool, initialized,
 * and returned. The lock owner will be NULL for a new lock acquired by the
 * caller, who is responsible for setting that field promptly. The lock owner
 * will be non-NULL when there is already an existing lock on the PBN.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_attempt_physical_zone_pbn_lock(struct physical_zone *zone,
				       physical_block_number_t pbn,
				       enum pbn_lock_type type,
				       struct pbn_lock **lock_ptr)
{
	/*
	 * Borrow and prepare a lock from the pool so we don't have to do two
	 * int_map accesses in the common case of no lock contention.
	 */
	struct pbn_lock *lock, *new_lock;
	int result = vdo_borrow_pbn_lock_from_pool(zone->lock_pool, type,
						   &new_lock);
	if (result != VDO_SUCCESS) {
		ASSERT_LOG_ONLY(false,
				"must always be able to borrow a PBN lock");
		return result;
	}

	result = int_map_put(zone->pbn_operations, pbn, new_lock, false,
			     (void **) &lock);
	if (result != VDO_SUCCESS) {
		vdo_return_pbn_lock_to_pool(zone->lock_pool, new_lock);
		return result;
	}

	if (lock != NULL) {
		/* The lock is already held, so we don't need the borrowed one. */
		vdo_return_pbn_lock_to_pool(zone->lock_pool,
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
 * allocate_and_lock_block() - Attempt to allocate a block from this zone.
 * @allocation: The struct allocation of the data_vio attempting to allocate.
 *
 * If a block is allocated, the recipient will also hold a lock on it.
 *
 * Return: VDO_SUCESSS if a block was allocated, or an error code.
 */
static int allocate_and_lock_block(struct allocation *allocation)
{
	int result;
	struct pbn_lock *lock;

	ASSERT_LOG_ONLY(allocation->lock == NULL,
			"must not allocate a block while already holding a lock on one");

	result = vdo_allocate_block(allocation->zone->allocator,
				    &allocation->pbn);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_attempt_physical_zone_pbn_lock(allocation->zone,
						    allocation->pbn,
						    allocation->write_lock_type,
						    &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock->holder_count > 0) {
		/* This block is already locked, which should be impossible. */
		return uds_log_error_strerror(VDO_LOCK_ERROR,
					      "Newly allocated block %llu was spuriously locked (holder_count=%u)",
					      (unsigned long long) allocation->pbn,
					      lock->holder_count);
	}

	/* We've successfully acquired a new lock, so mark it as ours. */
	lock->holder_count += 1;
	allocation->lock = lock;
	vdo_assign_pbn_lock_provisional_reference(lock);
	return VDO_SUCCESS;
}

/**
 * retry_allocation() - Retry allocating a block now that we're done waiting
 *                      for scrubbing.
 * @waiter: The allocating_vio that was waiting to allocate.
 * @context: The context (unused).
 */
static void retry_allocation(struct waiter *waiter,
			     void *context __always_unused)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);

	/* Now that some slab has scrubbed, restart the allocation process. */
	data_vio->allocation.wait_for_clean_slab = false;
	data_vio->allocation.first_allocation_zone =
		data_vio->allocation.zone->zone_number;
	continue_data_vio(data_vio, VDO_SUCCESS);
}

/**
 * continue_allocating() - Continue searching for an allocation by enqueuing
 *                         to wait for scrubbing or switching to the next
 *                         zone.
 * @data_vio: The data_vio attempting to get an allocation.
 *
 * This method should only be called from the error handler set in
 * data_vio_allocate_data_block.
 *
 * Return: true if the allocation process has continued in another zone.
 */
static bool continue_allocating(struct data_vio *data_vio)
{
	struct allocation *allocation = &data_vio->allocation;
	struct physical_zone *zone = allocation->zone;
	struct vdo_completion *completion = data_vio_as_completion(data_vio);
	int result = VDO_SUCCESS;
	bool was_waiting = allocation->wait_for_clean_slab;
	bool tried_all =
		(allocation->first_allocation_zone == zone->next->zone_number);

	vdo_reset_completion(completion);

	if (tried_all && !was_waiting) {
		/*
		 * We've already looked in all the zones, and found nothing.
		 * So go through the zones again, and wait for each to scrub
		 * before trying to allocate.
		 */
		allocation->wait_for_clean_slab = true;
		allocation->first_allocation_zone = zone->zone_number;
	}

	if (allocation->wait_for_clean_slab) {
		struct waiter *waiter = data_vio_as_waiter(data_vio);
		struct slab_scrubber *scrubber
			= zone->allocator->slab_scrubber;

		waiter->callback = retry_allocation;
		result = vdo_enqueue_clean_slab_waiter(scrubber, waiter);

		if (result == VDO_SUCCESS) {
			/* We've enqueued to wait for a slab to be scrubbed. */
			return true;
		}

		if ((result != VDO_NO_SPACE) || (was_waiting && tried_all)) {
			vdo_set_completion_result(completion, result);
			return false;
		}
	}

	allocation->zone = zone->next;
	completion->callback_thread_id = allocation->zone->thread_id;
	vdo_continue_completion(completion, VDO_SUCCESS);
	return true;
}

/**
 * vdo_allocate_block_in_zone() - Attempt to allocate a block in the current
 *                                physical zone, and if that fails try the
 *                                next if possible.
 * @data_vio: The data_vio needing an allocation.
 *
 * Return: true if a block was allocated, if not the data_vio will have been
 *         dispatched so the caller must not touch it.
 */
bool vdo_allocate_block_in_zone(struct data_vio *data_vio)
{
	int result = allocate_and_lock_block(&data_vio->allocation);

	if (result == VDO_SUCCESS) {
		return true;
	}

	if ((result != VDO_NO_SPACE) || !continue_allocating(data_vio)) {
		continue_data_vio(data_vio, result);
	}

	return false;
}

/**
 * vdo_release_physical_zone_pbn_lock() - Release a physical block lock if it
 *                                        is held and return it to the lock
 *                                        pool.
 * @zone: The physical zone in which the lock was obtained.
 * @locked_pbn: The physical block number to unlock.
 * @lock: The lock being released.
 *
 * It must be the last live reference, as if the memory were being freed (the
 * lock memory will re-initialized or zeroed).
 */
void vdo_release_physical_zone_pbn_lock(struct physical_zone *zone,
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

	vdo_release_pbn_lock_provisional_reference(lock, locked_pbn,
						   zone->allocator);
	vdo_return_pbn_lock_to_pool(zone->lock_pool, lock);
}

/**
 * vdo_dump_physical_zone() - Dump information about a physical zone to the
 *                            log for debugging.
 * @zone: The zone to dump.
 */
void vdo_dump_physical_zone(const struct physical_zone *zone)
{
	vdo_dump_block_allocator(zone->allocator);
}
