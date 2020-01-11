/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocatingVIO.c#11 $
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
	assertInPhysicalZone(allocating_vio);

	ASSERT_LOG_ONLY(allocating_vio->allocation_lock == NULL,
			"must not acquire a lock while already referencing one");

	struct pbn_lock *lock;
	int result = attemptPBNLock(allocating_vio->zone,
				    allocating_vio->allocation,
				    allocating_vio->write_lock_type,
				    &lock);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock->holderCount > 0) {
		// This block is already locked, which should be impossible.
		return logErrorWithStringError(VDO_LOCK_ERROR,
					       "Newly allocated block %llu was spuriously locked (holderCount=%u)",
					       allocating_vio->allocation,
					       lock->holderCount);
	}

	// We've successfully acquired a new lock, so mark it as ours.
	lock->holderCount += 1;
	allocating_vio->allocation_lock = lock;
	assignProvisionalReference(lock);
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
		getBlockAllocator(allocating_vio->zone);
	int result = allocateBlock(allocator, &allocating_vio->allocation);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = attempt_pbn_write_lock(allocating_vio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// We got a block!
	struct vio *vio = allocating_vio_as_vio(allocating_vio);
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
static void retryAllocateBlockForWrite(struct waiter *waiter,
				       void *context __attribute__((unused)))
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
	struct waiter *waiter = allocating_vio_as_waiter(allocating_vio);
	waiter->callback = retryAllocateBlockForWrite;

	struct block_allocator *allocator =
		getBlockAllocator(allocating_vio->zone);
	int result = enqueueForCleanSlab(allocator, waiter);
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
static int allocateBlockInZone(struct allocating_vio *allocating_vio)
{
	allocating_vio->allocation_attempts++;
	int result = allocate_and_lock_block(allocating_vio);
	if (result != VDO_NO_SPACE) {
		return result;
	}

	if (allocating_vio->wait_for_clean_slab) {
		result = wait_for_clean_slab(allocating_vio);
		if (result != VDO_NO_SPACE) {
			return result;
		}
	}

	struct vdo *vdo = get_vdo_from_allocating_vio(allocating_vio);
	const ThreadConfig *threadConfig = getThreadConfig(vdo);
	if (allocating_vio->allocation_attempts >=
	    threadConfig->physicalZoneCount) {
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
	ZoneCount zoneNumber = getPhysicalZoneNumber(allocating_vio->zone) + 1;
	if (zoneNumber == threadConfig->physicalZoneCount) {
		zoneNumber = 0;
	}
	allocating_vio->zone = vdo->physicalZones[zoneNumber];
	launch_physical_zone_callback(allocating_vio,
				      allocate_block_for_write,
				      THIS_LOCATION("$F;cb=allocBlockInZone"));
	return VDO_SUCCESS;
}

/**
 * Attempt to allocate a block. This callback is registered in
 * allocate_data_block() and allocateBlockInZone().
 *
 * @param completion  The allocating_vio needing an allocation
 **/
static void allocate_block_for_write(struct vdo_completion *completion)
{
	struct allocating_vio *allocating_vio = as_allocating_vio(completion);
	assertInPhysicalZone(allocating_vio);
	allocating_vio_add_trace_record(allocating_vio, THIS_LOCATION(NULL));
	int result = allocateBlockInZone(allocating_vio);
	if (result != VDO_SUCCESS) {
		setCompletionResult(completion, result);
		allocating_vio->allocation_callback(allocating_vio);
	}
}

/**********************************************************************/
void allocate_data_block(struct allocating_vio *allocating_vio,
			 struct allocation_selector *selector,
			 PBNLockType write_lock_type,
			 allocation_callback *callback)
{
	allocating_vio->write_lock_type = write_lock_type;
	allocating_vio->allocation_callback = callback;
	allocating_vio->allocation_attempts = 0;
	allocating_vio->allocation = ZERO_BLOCK;

	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	allocating_vio->zone =
		vio->vdo->physicalZones[get_next_allocation_zone(selector)];

	launch_physical_zone_callback(allocating_vio,
				      allocate_block_for_write,
				      THIS_LOCATION("$F;cb=allocDataBlock"));
}

/**********************************************************************/
void release_allocation_lock(struct allocating_vio *allocating_vio)
{
	assertInPhysicalZone(allocating_vio);
	PhysicalBlockNumber lockedPBN = allocating_vio->allocation;
	if (hasProvisionalReference(allocating_vio->allocation_lock)) {
		allocating_vio->allocation = ZERO_BLOCK;
	}

	releasePBNLock(allocating_vio->zone,
		       lockedPBN,
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
