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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dataVIO.c#56 $
 */

#include "dataVIO.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "bio.h"
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
	"acknowledge_write",
	"acquire_vdo_hash_lock",
	"attempt_logical_block_lock",
	"lock_duplicate_pbn",
	"check_for_duplication",
	"compress_data_vio",
	"find_block_map_slot",
	"get_mapped_block/for_read",
	"get_mapped_block/for_dedupe",
	"get_mapped_block/for_write",
	"hash_data_vio",
	"journal_decrement_for_dedupe",
	"journal_decrement_for_write",
	"journal_increment_for_compression",
	"journal_increment_for_dedupe",
	"journal_increment_for_write",
	"journal_mapping_for_compression",
	"journal_mapping_for_dedupe",
	"journal_mapping_for_write",
	"journal_unmapping_for_dedupe",
	"journal_unmapping_for_write",
	"vdo_attempt_packing",
	"put_mapped_block/for_write",
	"put_mapped_block/for_dedupe",
	"read_data_vio",
	"update_dedupe_index",
	"verify_duplication",
	"write_data_vio",
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
				logical_block_number_t lbn)
{
	struct vdo *vdo = get_vdo_from_data_vio(data_vio);
	struct lbn_lock *lock = &data_vio->logical;

	lock->lbn = lbn;
	lock->locked = false;
	initialize_wait_queue(&lock->waiters);

	lock->zone = get_vdo_logical_zone(vdo->logical_zones,
					  vdo_compute_logical_zone(data_vio));
}

/**********************************************************************/
void prepare_data_vio(struct data_vio *data_vio,
		      logical_block_number_t lbn,
		      enum vio_operation operation,
		      bool is_trim,
		      vdo_action *callback)
{
	struct vio *vio = data_vio_as_vio(data_vio);

	// Clearing the tree lock must happen before initializing the LBN lock,
	// which also adds information to the tree lock.
	memset(&data_vio->tree_lock, 0, sizeof(data_vio->tree_lock));
	initialize_lbn_lock(data_vio, lbn);
	INIT_LIST_HEAD(&data_vio->hash_lock_entry);
	INIT_LIST_HEAD(&data_vio->write_entry);

	vio_reset_allocation(data_vio_as_allocating_vio(data_vio));

	data_vio->is_duplicate = false;

	memset(&data_vio->chunk_name, 0, sizeof(data_vio->chunk_name));
	memset(&data_vio->duplicate, 0, sizeof(data_vio->duplicate));

	vio->operation = operation;
	vio->callback = callback;

	data_vio->mapped.state = MAPPING_STATE_UNCOMPRESSED;
	data_vio->new_mapped.state =
		(is_trim ? MAPPING_STATE_UNMAPPED : MAPPING_STATE_UNCOMPRESSED);
	reset_vdo_completion(vio_as_completion(vio));
	set_data_vio_logical_callback(data_vio,
				      vdo_attempt_logical_block_lock);
}

/**********************************************************************/
void complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	if (completion->result != VDO_SUCCESS) {
		struct vio *vio = data_vio_as_vio(data_vio);
		char vio_operation[VDO_VIO_OPERATION_DESCRIPTION_MAX_LENGTH];
		get_vio_operation_description(vio, vio_operation);
		update_vio_error_stats(vio,
				       "Completing %s vio for LBN %llu with error after %s",
				       vio_operation,
				       data_vio->logical.lbn,
				       get_data_vio_operation_name(data_vio));
	}

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
	set_vdo_completion_result(completion, result);
	complete_data_vio(completion);
}

/**********************************************************************/
const char *get_data_vio_operation_name(struct data_vio *data_vio)
{
	STATIC_ASSERT(
		(MAX_ASYNC_OPERATION_NUMBER - MIN_ASYNC_OPERATION_NUMBER) ==
		COUNT_OF(ASYNC_OPERATION_NAMES));

	return ((data_vio->last_async_operation < MAX_ASYNC_OPERATION_NUMBER) ?
			ASYNC_OPERATION_NAMES[data_vio->last_async_operation] :
			"unknown async operation");
}

/**********************************************************************/
void receive_data_vio_dedupe_advice(struct data_vio *data_vio,
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
		vdo_validate_dedupe_advice(vdo, advice, data_vio->logical.lbn);
	set_data_vio_duplicate_location(data_vio, duplicate);
}

/**********************************************************************/
void set_data_vio_duplicate_location(struct data_vio *data_vio,
				     const struct zoned_pbn source)
{
	data_vio->is_duplicate = (source.pbn != VDO_ZERO_BLOCK);
	data_vio->duplicate = source;
}

/**********************************************************************/
void clear_data_vio_mapped_location(struct data_vio *data_vio)
{
	data_vio->mapped = (struct zoned_pbn){
		.state = MAPPING_STATE_UNMAPPED,
	};
}

/**********************************************************************/
int set_data_vio_mapped_location(struct data_vio *data_vio,
				 physical_block_number_t pbn,
				 enum block_mapping_state state)
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
	data_vio->logical.locked = true;

	if (is_write_data_vio(data_vio)) {
		launch_write_data_vio(data_vio);
	} else {
		launch_read_data_vio(data_vio);
	}
}

/**********************************************************************/
void vdo_attempt_logical_block_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct lbn_lock *lock = &data_vio->logical;
	struct vdo *vdo = get_vdo_from_data_vio(data_vio);
	struct data_vio *lock_holder;
	int result;

	assert_data_vio_in_logical_zone(data_vio);

	if (data_vio->logical.lbn >= vdo->states.vdo.config.logical_blocks) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	result = int_map_put(get_vdo_logical_zone_lbn_lock_map(lock->zone),
			     lock->lbn, data_vio, false,
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
	    READ_ONCE(lock_holder->allocation_succeeded)) {
		vdo_copy_data(lock_holder, data_vio);
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	data_vio->last_async_operation = ASYNC_OP_ATTEMPT_LOGICAL_BLOCK_LOCK;
	result = enqueue_data_vio(&lock_holder->logical.waiters,
				data_vio);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	// Prevent writes and read-modify-writes from blocking indefinitely on
	// lock holders in the packer.
	if (!is_read_data_vio(lock_holder) &&
	    cancel_vio_compression(lock_holder)) {
		data_vio->compression.lock_holder = lock_holder;
		launch_data_vio_packer_callback(data_vio,
						remove_lock_holder_from_vdo_packer);
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
	struct int_map *lock_map =
		get_vdo_logical_zone_lbn_lock_map(lock->zone);
	struct data_vio *lock_holder;

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
	lock_holder = int_map_remove(lock_map, lock->lbn);
	ASSERT_LOG_ONLY((data_vio == lock_holder),
			"logical block lock mismatch for block %llu",
			lock->lbn);
	lock->locked = false;
	return;
}

/**********************************************************************/
void vdo_release_logical_block_lock(struct data_vio *data_vio)
{
	struct data_vio *lock_holder, *next_lock_holder;
	struct lbn_lock *lock = &data_vio->logical;
	int result;

	assert_data_vio_in_logical_zone(data_vio);
	if (!has_waiters(&data_vio->logical.waiters)) {
		release_lock(data_vio);
		return;
	}

	ASSERT_LOG_ONLY(lock->locked, "lbn_lock with waiters is not locked");

	// Another data_vio is waiting for the lock, so just transfer it in a
	// single lock map operation
	next_lock_holder =
		waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));

	// Transfer the remaining lock waiters to the next lock holder.
	transfer_all_waiters(&lock->waiters,
			     &next_lock_holder->logical.waiters);

	result = int_map_put(get_vdo_logical_zone_lbn_lock_map(lock->zone),
			     lock->lbn, next_lock_holder, true,
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
		cancel_vio_compression(next_lock_holder);
	}

	// Avoid stack overflow on lock transfer.
	// XXX: this is only an issue in the 1 thread config.
	data_vio_as_completion(next_lock_holder)->requeue = true;
	launch_locked_request(next_lock_holder);
}

/**********************************************************************/
void free_data_vio(struct data_vio *data_vio)
{
	if (data_vio == NULL) {
		return;
	}

	vdo_free_bio(FORGET(data_vio_as_vio(data_vio)->bio));
	FREE(FORGET(data_vio->read_block.buffer));
	FREE(FORGET(data_vio->data_block));
	FREE(FORGET(data_vio->scratch_block));
	FREE(FORGET(data_vio));
}
