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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocatingVIO.c#27 $
 */

#include "allocatingVIO.h"

#include "logger.h"

#include "allocationSelector.h"
#include "blockAllocator.h"
#include "dataVIO.h"
#include "pbnLock.h"
#include "slabDepot.h"
#include "types.h"
#include "vdoInternal.h"
#include "vioWrite.h"

/**
 * Make a single attempt to acquire a write lock on a newly-allocated PBN.
 *
 * @param allocating_vio  The allocating_vio that wants a write lock for its
 *                        newly allocated block
 *
 * @return VDO_SUCCESS or an error code
 **/
static int attempt_pbn_write_lock(struct allocating_vio *allocating_vio)
{
	struct pbn_lock *lock;
	int result;

	assert_in_physical_zone(allocating_vio);

	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not acquire a lock while already referencing one");

	result = attempt_pbn_lock(allocating_vio->zone,
				  allocating_vio->allocation,
				  allocating_vio->write_lock_type,
				  &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock->holder_count > 0) {
		// This block is already locked, which should be impossible.
		return log_error_strerror(VDO_LOCK_ERROR,
					  "Newly allocated block %llu was spuriously locked (holder_count=%u)",
					  allocating_vio->allocation,
					  lock->holder_count);
	}

	// We've successfully acquired a new lock, so mark it as ours.
	lock->holder_count += 1;
	allocating_vio->allocation_lock = lock;
	assign_provisional_reference(lock);
	return VDO_SUCCESS;
}

/**
 * Attempt to allocate and lock a physical block. If successful, continue
 * along the write path.
 *
 * @param allocating_vio  The allocating_vio which needs an allocation
 *
 * @return VDO_SUCCESS or an error if a block could not be allocated
 **/
static int allocate_and_lock_block(struct allocating_vio *allocating_vio)
{
	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	struct block_allocator *allocator =
		get_block_allocator(allocating_vio->zone);
	int result = allocate_block(allocator, &allocating_vio->allocation);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = attempt_pbn_write_lock(allocating_vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// We got a block!
	vio->physical = allocating_vio->allocation;
	allocating_vio->allocation_callback(allocating_vio);
	return VDO_SUCCESS;
}

static void allocate_block_for_write(struct vdo_completion *completion);

/**
 * Retry allocating a block for write.
 *
 * @param waiter   The allocating_vio that was waiting to allocate
 * @param context  The context (unused)
 **/
static void
retry_allocate_block_for_write(struct waiter *waiter,
			       void *context __always_unused)
{
	struct allocating_vio *allocating_vio = waiter_as_allocating_vio(waiter);
	allocate_block_for_write(allocating_vio_as_completion(allocating_vio));
}

/**
 * Attempt to enqueue an allocating_vio to wait for a slab to be scrubbed in the
 * current allocation zone.
 *
 * @param allocating_vio  The struct allocating_vio which wants to allocate a
 *                        block
 *
 * @return VDO_SUCCESS if the allocating_vio was queued, VDO_NO_SPACE if there
 *         are no slabs to be scrubbed in the current zone, or some other
 *         error
 **/
static int wait_for_clean_slab(struct allocating_vio *allocating_vio)
{
	int result;
	struct block_allocator *allocator =
		get_block_allocator(allocating_vio->zone);
	struct waiter *waiter = allocating_vio_as_waiter(allocating_vio);

	waiter->callback = retry_allocate_block_for_write;

	result = enqueue_for_clean_slab(allocator, waiter);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// We've successfully enqueued, when we come back, pretend like we've
	// never tried this allocation before.
	allocating_vio->wait_for_clean_slab = false;
	allocating_vio->allocation_attempts = 0;
	return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block in an allocating_vio's current allocation zone.
 *
 * @param allocating_vio  The allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_block_in_zone(struct allocating_vio *allocating_vio)
{
	zone_count_t zone_number;
	int result;
	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);
	const struct thread_config *thread_config = get_thread_config(vdo);

	allocating_vio->allocation_attempts++;
	result = allocate_and_lock_block(allocating_vio);
	if (result != VDO_NO_SPACE) {
		return result;
	}

	if (allocating_vio->wait_for_clean_slab) {
		result = wait_for_clean_slab(allocating_vio);
		if (result != VDO_NO_SPACE) {
			return result;
		}
	}

	if (allocating_vio->allocation_attempts >=
	    thread_config->physical_zone_count) {
		if (allocating_vio->wait_for_clean_slab) {
			// There were no free blocks in any zone, and no zone
			// had slabs to scrub.
			allocating_vio->allocation_callback(allocating_vio);
			return VDO_SUCCESS;
		}

		allocating_vio->wait_for_clean_slab = true;
		allocating_vio->allocation_attempts = 0;
	}

	// Try the next zone
	zone_number = get_physical_zone_number(allocating_vio->zone) + 1;
	if (zone_number == thread_config->physical_zone_count) {
		zone_number = 0;
	}
	allocating_vio->zone = vdo->physical_zones[zone_number];
	launch_physical_zone_callback(allocating_vio,
				      allocate_block_for_write);
	return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block. This callback is registered in
 * allocate_data_block() and allocate_block_in_zone().
 *
 * @param completion  The allocating_vio needing an allocation
 **/
static void allocate_block_for_write(struct vdo_completion *completion)
{
	int result;
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);
	assert_in_physical_zone(allocating_vio);
	result = allocate_block_in_zone(allocating_vio);
	if (result != VDO_SUCCESS) {
		set_completion_result(completion, result);
		allocating_vio->allocation_callback(allocating_vio);
	}
}

/**********************************************************************/
void allocate_data_block(struct allocating_vio *allocating_vio,
			 struct allocation_selector *selector,
			 enum pbn_lock_type write_lock_type,
			 allocation_callback *callback)
{
	struct vio *vio = allocating_vio_as_vio(allocating_vio);

	allocating_vio->write_lock_type = write_lock_type;
	allocating_vio->allocation_callback = callback;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->allocation = ZERO_BLOCK;

	allocating_vio->zone =
		vio->vdo->physical_zones[get_next_allocation_zone(selector)];

	launch_physical_zone_callback(allocating_vio,
				      allocate_block_for_write);
}

/**********************************************************************/
void release_allocation_lock(struct allocating_vio *allocating_vio)
{
	physical_block_number_t locked_pbn;

	assert_in_physical_zone(allocating_vio);
	locked_pbn = allocating_vio->allocation;
	if (has_provisional_reference(allocating_vio->allocation_lock)) {
		allocating_vio->allocation = ZERO_BLOCK;
	}

	release_pbn_lock(allocating_vio->zone,
			 locked_pbn,
			 &allocating_vio->allocation_lock);
}

/**********************************************************************/
void reset_allocation(struct allocating_vio *allocating_vio)
{
	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not reset allocation while holding a PBN lock");

	allocating_vio_as_vio(allocating_vio)->physical = ZERO_BLOCK;
	allocating_vio->zone = NULL;
	allocating_vio->allocation = ZERO_BLOCK;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->wait_for_clean_slab = false;
}
