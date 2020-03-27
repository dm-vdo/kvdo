/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dataVIO.c#27 $
 */

#include "dataVIO.h"

#include "logger.h"

#include "atomic.h"
#include "blockMap.h"
#include "compressionState.h"
#include "extent.h"
#include "logicalZone.h"
#include "threadConfig.h"
#include "vdoInternal.h"
#include "vioRead.h"
#include "vioWrite.h"

static const char *ASYNC_OPERATION_NAMES[] = {
	"launch",
	"acknowledgeWrite",
	"acquire_hash_lock",
	"acquireLogicalBlockLock",
	"acquirePBNReadLock",
	"checkForDedupeForRollover",
	"checkForDeduplication",
	"compress_data",
	"continueVIOAsync",
	"findBlockMapSlot",
	"getMappedBlock",
	"getMappedBlockForDedupe",
	"getMappedBlockForWrite",
	"hashData",
	"journalDecrementForDedupe",
	"journalDecrementForWrite",
	"journalIncrementForCompression",
	"journalIncrementForDedupe",
	"journalIncrementForWrite",
	"journalMappingForCompression",
	"journalMappingForDedupe",
	"journalMappingForWrite",
	"journalUnmappingForDedupe",
	"journalUnmappingForWrite",
	"attempt_packing",
	"putMappedBlock",
	"putMappedBlockForDedupe",
	"readData",
	"updateIndex",
	"verifyDeduplication",
	"writeData",
};

/**
 * Initialize the LBN lock of a data_vio. In addition to recording the LBN on
 * which the data_vio will operate, it will also find the logical zone
 * associated with the LBN.
 *
 * @param data_vio  The data_vio to initialize
 * @param lbn       The lbn on which the data_vio will operate
 **/
static void initialize_lbn_lock(struct data_vio *data_vio,
				LogicalBlockNumber lbn)
{
	struct lbn_lock *lock = &data_vio->logical;
	lock->lbn = lbn;
	lock->locked = false;
	initialize_wait_queue(&lock->waiters);

	struct vdo *vdo = get_vdo_from_data_vio(data_vio);
	lock->zone = get_logical_zone(vdo->logical_zones,
				      compute_logical_zone(data_vio));
}

/**********************************************************************/
void prepare_data_vio(struct data_vio *data_vio,
		      LogicalBlockNumber lbn,
		      VIOOperation operation,
		      bool is_trim,
		      vdo_action *callback)
{
	// Clearing the tree lock must happen before initializing the LBN lock,
	// which also adds information to the tree lock.
	memset(&data_vio->treeLock, 0, sizeof(data_vio->treeLock));
	initialize_lbn_lock(data_vio, lbn);
	initializeRing(&data_vio->hashLockNode);
	initializeRing(&data_vio->writeNode);

	reset_allocation(data_vio_as_allocating_vio(data_vio));

	data_vio->isDuplicate = false;

	memset(&data_vio->chunkName, 0, sizeof(data_vio->chunkName));
	memset(&data_vio->duplicate, 0, sizeof(data_vio->duplicate));

	struct vio *vio = data_vio_as_vio(data_vio);
	vio->operation = operation;
	vio->callback = callback;
	data_vio->pageCompletion.completion.enqueueable =
		vio_as_completion(vio)->enqueueable;

	data_vio->mapped.state = MAPPING_STATE_UNCOMPRESSED;
	data_vio->newMapped.state =
		(is_trim ? MAPPING_STATE_UNMAPPED : MAPPING_STATE_UNCOMPRESSED);
	reset_completion(vio_as_completion(vio));
	set_logical_callback(data_vio,
			     attempt_logical_block_lock,
			     THIS_LOCATION("$F;cb=acquireLogicalBlockLock"));
}

/**********************************************************************/
void complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	if (completion->result != VDO_SUCCESS) {
		struct vio *vio = data_vio_as_vio(data_vio);
		update_vio_error_stats(vio,
				       "Completing %s vio for LBN %llu with error after %s",
				       get_vio_read_write_flavor(vio),
				       data_vio->logical.lbn,
				       get_operation_name(data_vio));
	}

	data_vio_add_trace_record(data_vio, THIS_LOCATION("$F($io)"));
	if (is_read_data_vio(data_vio)) {
		cleanup_read_data_vio(data_vio);
	} else {
		cleanup_write_data_vio(data_vio);
	}
}

/**********************************************************************/
void finish_data_vio(struct data_vio *data_vio, int result)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);
	set_completion_result(completion, result);
	complete_data_vio(completion);
}

/**********************************************************************/
const char *get_operation_name(struct data_vio *data_vio)
{
	STATIC_ASSERT(
		(MAX_ASYNC_OPERATION_NUMBER - MIN_ASYNC_OPERATION_NUMBER) ==
		COUNT_OF(ASYNC_OPERATION_NAMES));

	return ((data_vio->lastAsyncOperation < MAX_ASYNC_OPERATION_NUMBER) ?
			ASYNC_OPERATION_NAMES[data_vio->lastAsyncOperation] :
			"unknown async operation");
}

/**********************************************************************/
void receive_dedupe_advice(struct data_vio *data_vio,
			   const struct data_location *advice)
{
	/*
	 * NOTE: this is called on non-base-code threads. Be very careful to
	 * not do anything here that needs a base code thread-local variable,
	 * such as trying to get the current thread ID, or that does a lot of
	 * work.
	 */

	struct vdo *vdo = get_vdo_from_data_vio(data_vio);
	struct zoned_pbn duplicate =
		validate_dedupe_advice(vdo, advice, data_vio->logical.lbn);
	set_duplicate_location(data_vio, duplicate);
}

/**********************************************************************/
void set_duplicate_location(struct data_vio *data_vio,
			    const struct zoned_pbn source)
{
	data_vio->isDuplicate = (source.pbn != ZERO_BLOCK);
	data_vio->duplicate = source;
}

/**********************************************************************/
void clear_mapped_location(struct data_vio *data_vio)
{
	data_vio->mapped = (struct zoned_pbn){
		.state = MAPPING_STATE_UNMAPPED,
	};
}

/**********************************************************************/
int set_mapped_location(struct data_vio *data_vio,
			PhysicalBlockNumber pbn,
			BlockMappingState state)
{
	struct physical_zone *zone;
	int result = get_physical_zone(get_vdo_from_data_vio(data_vio), pbn,
				       &zone);
	if (result != VDO_SUCCESS) {
		return result;
	}

	data_vio->mapped = (struct zoned_pbn){
		.pbn = pbn,
		.state = state,
		.zone = zone,
	};
	return VDO_SUCCESS;
}

/**
 * Launch a request which has acquired an LBN lock.
 *
 * @param data_vio  The data_vio which has just acquired a lock
 **/
static void launch_locked_request(struct data_vio *data_vio)
{
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	data_vio->logical.locked = true;

	if (is_write_data_vio(data_vio)) {
		launch_write_data_vio(data_vio);
	} else {
		launch_read_data_vio(data_vio);
	}
}

/**********************************************************************/
void attempt_logical_block_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_logical_zone(data_vio);

	if (data_vio->logical.lbn >=
	    get_vdo_from_data_vio(data_vio)->config.logical_blocks) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	struct data_vio *lock_holder;
	struct lbn_lock *lock = &data_vio->logical;
	int result = int_map_put(get_lbn_lock_map(lock->zone),
				 lock->lbn,
				 data_vio,
				 false,
				 (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	if (lock_holder == NULL) {
		// We got the lock
		launch_locked_request(data_vio);
		return;
	}

	result = ASSERT(lock_holder->logical.locked,
			"logical block lock held");
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	/*
	 * If the new request is a pure read request (not read-modify-write)
	 * and the lock_holder is writing and has received an allocation
	 * (VDO-2683), service the read request immediately by copying data
	 * from the lock_holder to avoid having to flush the write out of the
	 * packer just to prevent the read from waiting indefinitely. If the
	 * lock_holder does not yet have an allocation, prevent it from
	 * blocking in the packer and wait on it.
	 */
	if (is_read_data_vio(data_vio) &&
	    atomicLoadBool(&lock_holder->has_allocation)) {
		copyData(lock_holder, data_vio);
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	data_vio->lastAsyncOperation = ACQUIRE_LOGICAL_BLOCK_LOCK;
	result = enqueue_data_vio(&lock_holder->logical.waiters,
				data_vio,
				THIS_LOCATION("$F;cb=logicalBlockLock"));
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	// Prevent writes and read-modify-writes from blocking indefinitely on
	// lock holders in the packer.
	if (!is_read_data_vio(lock_holder) &&
	    cancel_compression(lock_holder)) {
		data_vio->compression.lockHolder = lock_holder;
		launch_packer_callback(
			data_vio,
			remove_lock_holder_from_packer,
			THIS_LOCATION("$F;cb=remove_lock_holder_from_packer"));
	}
}

/**
 * Release an uncontended LBN lock.
 *
 * @param data_vio  The data_vio holding the lock
 **/
static void release_lock(struct data_vio *data_vio)
{
	struct lbn_lock *lock = &data_vio->logical;
	struct int_map *lock_map = get_lbn_lock_map(lock->zone);
	if (!lock->locked) {
		// The lock is not locked, so it had better not be registered
		// in the lock map.
		struct data_vio *lock_holder = int_map_get(lock_map, lock->lbn);
		ASSERT_LOG_ONLY((data_vio != lock_holder),
				"no logical block lock held for block %llu",
				lock->lbn);
		return;
	}

	// Remove the lock from the logical block lock map, releasing the lock.
	struct data_vio *lock_holder = int_map_remove(lock_map, lock->lbn);
	ASSERT_LOG_ONLY((data_vio == lock_holder),
			"logical block lock mismatch for block %llu",
			lock->lbn);
	lock->locked = false;
	return;
}

/**********************************************************************/
void release_logical_block_lock(struct data_vio *data_vio)
{
	assert_in_logical_zone(data_vio);
	if (!has_waiters(&data_vio->logical.waiters)) {
		release_lock(data_vio);
		return;
	}

	struct lbn_lock *lock = &data_vio->logical;
	ASSERT_LOG_ONLY(lock->locked, "lbn_lock with waiters is not locked");

	// Another data_vio is waiting for the lock, so just transfer it in a
	// single lock map operation
	struct data_vio *next_lock_holder =
		waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));

	// Transfer the remaining lock waiters to the next lock holder.
	transfer_all_waiters(&lock->waiters,
			     &next_lock_holder->logical.waiters);

	struct data_vio *lock_holder;
	int result = int_map_put(get_lbn_lock_map(lock->zone),
				 lock->lbn,
				 next_lock_holder,
				 true,
				 (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		finish_data_vio(next_lock_holder, result);
		return;
	}

	ASSERT_LOG_ONLY((lock_holder == data_vio),
			"logical block lock mismatch for block %llu",
			lock->lbn);
	lock->locked = false;

	/*
	 * If there are still waiters, other data_vios must be trying to get
	 * the lock we just transferred. We must ensure that the new lock
	 * holder doesn't block in the packer.
	 */
	if (has_waiters(&next_lock_holder->logical.waiters)) {
		cancel_compression(next_lock_holder);
	}

	// Avoid stack overflow on lock transfer.
	// XXX: this is only an issue in the 1 thread config.
	data_vio_as_completion(next_lock_holder)->requeue = true;
	launch_locked_request(next_lock_holder);
}
