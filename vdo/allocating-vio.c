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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocating-vio.c#1 $
 */

#include "allocatingVIO.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "allocationSelector.h"
#include "blockAllocator.h"
#include "dataVIO.h"
#include "kernelTypes.h"
#include "pbnLock.h"
#include "slabDepot.h"
#include "types.h"
#include "vdo.h"
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

	assert_vio_in_physical_zone(allocating_vio);

	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not acquire a lock while already referencing one");

	result = attempt_vdo_physical_zone_pbn_lock(allocating_vio->zone,
						    allocating_vio->allocation,
						    allocating_vio->write_lock_type,
						    &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock->holder_count > 0) {
		// This block is already locked, which should be impossible.
		return uds_log_error_strerror(VDO_LOCK_ERROR,
					      "Newly allocated block %llu was spuriously locked (holder_count=%u)",
					      (unsigned long long) allocating_vio->allocation,
					      lock->holder_count);
	}

	// We've successfully acquired a new lock, so mark it as ours.
	lock->holder_count += 1;
	allocating_vio->allocation_lock = lock;
	assign_vdo_pbn_lock_provisional_reference(lock);
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
	struct block_allocator *allocator =
		get_vdo_physical_zone_block_allocator(allocating_vio->zone);
	int result = allocate_vdo_block(allocator, &allocating_vio->allocation);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = attempt_pbn_write_lock(allocating_vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// We got a block!
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
		get_vdo_physical_zone_block_allocator(allocating_vio->zone);
	struct waiter *waiter = allocating_vio_as_waiter(allocating_vio);

	waiter->callback = retry_allocate_block_for_write;

	result = enqueue_for_clean_vdo_slab(allocator, waiter);
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
	    vdo->thread_config->physical_zone_count) {
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
	zone_number = get_vdo_physical_zone_number(allocating_vio->zone) + 1;
	if (zone_number == vdo->thread_config->physical_zone_count) {
		zone_number = 0;
	}
	allocating_vio->zone = vdo->physical_zones[zone_number];
	vio_launch_physical_zone_callback(allocating_vio,
					  allocate_block_for_write);
	return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block. This callback is registered in
 * vio_allocate_data_block() and allocate_block_in_zone().
 *
 * @param completion  The allocating_vio needing an allocation
 **/
static void allocate_block_for_write(struct vdo_completion *completion)
{
	int result;
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);

	assert_vio_in_physical_zone(allocating_vio);
	result = allocate_block_in_zone(allocating_vio);
	if (result != VDO_SUCCESS) {
		set_vdo_completion_result(completion, result);
		allocating_vio->allocation_callback(allocating_vio);
	}
}

/**********************************************************************/
void vio_allocate_data_block(struct allocating_vio *allocating_vio,
			     struct allocation_selector *selector,
			     enum pbn_lock_type write_lock_type,
			     allocation_callback *callback)
{
	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);

	allocating_vio->write_lock_type = write_lock_type;
	allocating_vio->allocation_callback = callback;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->allocation = VDO_ZERO_BLOCK;

	allocating_vio->zone =
		vdo->physical_zones[get_next_vdo_allocation_zone(selector)];

	vio_launch_physical_zone_callback(allocating_vio,
					  allocate_block_for_write);
}

/**********************************************************************/
void vio_release_allocation_lock(struct allocating_vio *allocating_vio)
{
	physical_block_number_t locked_pbn;

	assert_vio_in_physical_zone(allocating_vio);
	locked_pbn = allocating_vio->allocation;
	if (vdo_pbn_lock_has_provisional_reference(allocating_vio->allocation_lock)) {
		allocating_vio->allocation = VDO_ZERO_BLOCK;
	}

	release_vdo_physical_zone_pbn_lock(allocating_vio->zone,
					   locked_pbn,
					   UDS_FORGET(allocating_vio->allocation_lock));
}

/**********************************************************************/
void vio_reset_allocation(struct allocating_vio *allocating_vio)
{
	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not reset allocation while holding a PBN lock");

	allocating_vio->zone = NULL;
	allocating_vio->allocation = VDO_ZERO_BLOCK;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->wait_for_clean_slab = false;
}

/**********************************************************************/
int create_compressed_write_vio(struct vdo *vdo,
				void *parent,
				char *data,
				struct allocating_vio **allocating_vio_ptr)
{
	struct bio *bio;
	struct allocating_vio *allocating_vio;
	struct vio *vio;

	// Compressed write vios should use direct allocation and not use the
	// buffer pool, which is reserved for submissions from the linux block
	// layer.
	int result = UDS_ALLOCATE(1, struct allocating_vio, __func__,
				  &allocating_vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("compressed write vio allocation failure %d",
			      result);
		return result;
	}

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(allocating_vio);
		return result;
	}

	vio = allocating_vio_as_vio(allocating_vio);
	initialize_vio(vio,
		       bio,
		       VIO_TYPE_COMPRESSED_BLOCK,
		       VIO_PRIORITY_COMPRESSED_DATA,
		       parent,
		       vdo,
		       data);
	*allocating_vio_ptr = allocating_vio;
	return VDO_SUCCESS;
}
