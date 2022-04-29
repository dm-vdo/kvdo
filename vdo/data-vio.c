// SPDX-License-Identifier: GPL-2.0-only
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

#include "data-vio.h"

#include <linux/lz4.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "allocation-selector.h"
#include "bio.h"
#include "block-allocator.h"
#include "block-map.h"
#include "compressed-block.h"
#include "compression-state.h"
#include "dump.h"
#include "int-map.h"
#include "logical-zone.h"
#include "packer.h"
#include "status-codes.h"
#include "vdo.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vio-read.h"
#include "vio-write.h"

static const char *ASYNC_OPERATION_NAMES[] = {
	"launch",
	"acknowledge_write",
	"acquire_hash_lock",
	"attempt_logical_block_lock",
	"lock_duplicate_pbn",
	"check_for_duplication",
	"cleanup",
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

void destroy_data_vio(struct data_vio *data_vio)
{
	if (data_vio == NULL) {
		return;
	}

	vdo_free_bio(UDS_FORGET(data_vio_as_vio(data_vio)->bio));
	UDS_FREE(UDS_FORGET(data_vio->compression.block));
	UDS_FREE(UDS_FORGET(data_vio->data_block));
	UDS_FREE(UDS_FORGET(data_vio->scratch_block));
}

/**
 * Allocate the components of a data_vio.
 *
 * @param data_vio  The data_vio being constructed
 *
 * @return VDO_SUCCESS or an error
 **/
static int __must_check allocate_data_vio_components(struct data_vio *data_vio)
{
	struct vio *vio;
	int result;

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "vio data",
				     &data_vio->data_block);
	if (result != VDO_SUCCESS) {
		return uds_log_error_strerror(result,
					      "data_vio data allocation failure");
	}

	vio = data_vio_as_vio(data_vio);
	result = vdo_create_bio(&vio->bio);
	if (result != VDO_SUCCESS) {
		return uds_log_error_strerror(result,
					      "data_vio data bio allocation failure");
	}

	result = uds_allocate_memory(VDO_BLOCK_SIZE,
				     0,
				     "compressed block",
				     &data_vio->compression.block);
	if (result != VDO_SUCCESS) {
		return uds_log_error_strerror(result,
					      "data_vio compressed block allocation failure");
	}

	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "vio scratch",
				     &data_vio->scratch_block);
	if (result != VDO_SUCCESS) {
		return uds_log_error_strerror(result,
					      "data_vio scratch allocation failure");
	}

	return VDO_SUCCESS;
}

int initialize_data_vio(struct data_vio *data_vio)
{
	int result = allocate_data_vio_components(data_vio);

	if (result != VDO_SUCCESS) {
		destroy_data_vio(data_vio);
	}

	return result;
}

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
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	zone_count_t zone_number;
	struct lbn_lock *lock = &data_vio->logical;

	lock->lbn = lbn;
	lock->locked = false;
	initialize_wait_queue(&lock->waiters);
	zone_number = vdo_compute_logical_zone(data_vio);
	lock->zone = &vdo->logical_zones->zones[zone_number];
}

void attempt_logical_block_lock(struct vdo_completion *completion);

/**
 * (Re)initialize a data_vio to have a new logical block number, keeping the
 * same parent and other state and send it on its way.
 *
 * @param data_vio   The data_vio to initialize
 * @param lbn        The logical block number of the data_vio
 * @param operation  The operation this data_vio will perform
 **/
void launch_data_vio(struct data_vio *data_vio,
		     logical_block_number_t lbn,
		     enum vio_operation operation)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct vdo_completion *completion = vio_as_completion(vio);

	/*
	 * Clearing the tree lock must happen before initializing the LBN lock,
	 * which also adds information to the tree lock.
	 */
	memset(&data_vio->tree_lock, 0, sizeof(data_vio->tree_lock));
	initialize_lbn_lock(data_vio, lbn);
	INIT_LIST_HEAD(&data_vio->hash_lock_entry);
	INIT_LIST_HEAD(&data_vio->write_entry);

	memset(&data_vio->allocation, 0, sizeof(data_vio->allocation));

	data_vio->is_duplicate = false;

	memset(&data_vio->chunk_name, 0, sizeof(data_vio->chunk_name));
	memset(&data_vio->duplicate, 0, sizeof(data_vio->duplicate));

	vio->operation = operation;
	data_vio->mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
	if (data_vio->is_partial || (data_vio->remaining_discard == 0)) {
		/* This is either a write or a partial block discard */
		data_vio->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
	} else {
		/* This is a full block discard */
		data_vio->new_mapped.state = VDO_MAPPING_STATE_UNMAPPED;
	}

	vdo_reset_completion(completion);
	set_data_vio_logical_callback(data_vio, attempt_logical_block_lock);
	vdo_invoke_completion_callback_with_priority(completion,
						     VDO_REQ_Q_MAP_BIO_PRIORITY);
}

/**
 * Complete the processing of a data_vio.
 *
 * @param completion The completion of the vio to complete
 **/
void complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	completion->error_handler = NULL;
	if (completion->result != VDO_SUCCESS) {
		struct vio *vio = data_vio_as_vio(data_vio);
		char vio_operation[VDO_VIO_OPERATION_DESCRIPTION_MAX_LENGTH];

		get_vio_operation_description(vio, vio_operation);
		update_vio_error_stats(vio,
				       "Completing %s vio for LBN %llu with error after %s",
				       vio_operation,
				       (unsigned long long) data_vio->logical.lbn,
				       get_data_vio_operation_name(data_vio));
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_CLEANUP;
	if (is_read_data_vio(data_vio)) {
		cleanup_read_data_vio(data_vio);
	} else {
		cleanup_write_data_vio(data_vio);
	}
}

/**
 * Finish processing a data_vio, possibly due to an error. This function will
 * set any error, and then initiate data_vio clean up.
 *
 * @param data_vio  The data_vio to abort
 * @param result    The result of processing the data_vio
 **/
void finish_data_vio(struct data_vio *data_vio, int result)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);

	vdo_set_completion_result(completion, result);
	complete_data_vio(completion);
}

/**
 * Get the name of the last asynchronous operation performed on a data_vio.
 *
 * @param data_vio  The data_vio in question
 *
 * @return The name of the last operation performed on the data_vio
 **/
const char *get_data_vio_operation_name(struct data_vio *data_vio)
{
	STATIC_ASSERT((MAX_VIO_ASYNC_OPERATION_NUMBER -
		       MIN_VIO_ASYNC_OPERATION_NUMBER) ==
		      ARRAY_SIZE(ASYNC_OPERATION_NAMES));

	return ((data_vio->last_async_operation <
		 MAX_VIO_ASYNC_OPERATION_NUMBER) ?
			ASYNC_OPERATION_NAMES[data_vio->last_async_operation] :
			"unknown async operation");
}

/**
 * Check whether the advice received from UDS is a valid data location,
 * and if it is, accept it as the location of a potential duplicate of the
 * data_vio.
 *
 * @param data_vio The data_vio that queried UDS
 * @param advice   A potential location of the data, or NULL for no advice
 **/
void receive_data_vio_dedupe_advice(struct data_vio *data_vio,
				    const struct data_location *advice)
{
	/*
	 * NOTE: this is called on non-base-code threads. Be very careful to
	 * not do anything here that needs a base code thread-local variable,
	 * such as trying to get the current thread ID, or that does a lot of
	 * work.
	 */

	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct zoned_pbn duplicate =
		vdo_validate_dedupe_advice(vdo, advice, data_vio->logical.lbn);
	set_data_vio_duplicate_location(data_vio, duplicate);
}

/**
 * Set the location of the duplicate block for a data_vio, updating the
 * is_duplicate and duplicate fields from a zoned_pbn.
 *
 * @param data_vio The data_vio to modify
 * @param source   The location of the duplicate
 **/
void set_data_vio_duplicate_location(struct data_vio *data_vio,
				     const struct zoned_pbn source)
{
	data_vio->is_duplicate = (source.pbn != VDO_ZERO_BLOCK);
	data_vio->duplicate = source;
}

/**
 * Clear a data_vio's mapped block location, setting it to be unmapped. This
 * indicates the block map entry for the logical block is either unmapped or
 * corrupted.
 *
 * @param data_vio The data_vio whose mapped block location is to be reset
 **/
void clear_data_vio_mapped_location(struct data_vio *data_vio)
{
	data_vio->mapped = (struct zoned_pbn){
		.state = VDO_MAPPING_STATE_UNMAPPED,
	};
}

/**
 * Set a data_vio's mapped field to the physical location recorded in the block
 * map for the logical block in the vio.
 *
 * @param data_vio The data_vio whose field is to be set
 * @param pbn      The physical block number to set
 * @param state    The mapping state to set
 *
 * @return VDO_SUCCESS or an error code if the mapping is unusable
 **/
int set_data_vio_mapped_location(struct data_vio *data_vio,
				 physical_block_number_t pbn,
				 enum block_mapping_state state)
{
	struct physical_zone *zone;
	int result = vdo_get_physical_zone(vdo_from_data_vio(data_vio),
					   pbn, &zone);
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

/**
 * Attempt to acquire the lock on a logical block. This is the start of the
 * path for all external requests. It is registered in launch_data_vio().
 *
 * @param completion  The data_vio for an external data request as a completion
 **/
void attempt_logical_block_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct lbn_lock *lock = &data_vio->logical;
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct data_vio *lock_holder;
	int result;

	assert_data_vio_in_logical_zone(data_vio);

	if (data_vio->logical.lbn >= vdo->states.vdo.config.logical_blocks) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	result = int_map_put(lock->zone->lbn_operations,
			     lock->lbn,
			     data_vio,
			     false,
			     (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	if (lock_holder == NULL) {
		/* We got the lock */
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
		vdo_bio_copy_data_out(data_vio->user_bio,
				      (lock_holder->data_block +
				       data_vio->offset));
		acknowledge_data_vio(data_vio);
		complete_data_vio(completion);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_LOGICAL_BLOCK_LOCK;
	result = enqueue_data_vio(&lock_holder->logical.waiters,
				  data_vio);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	/*
	 * Prevent writes and read-modify-writes from blocking indefinitely on
	 * lock holders in the packer.
	 */
	if (!is_read_data_vio(lock_holder) &&
	    cancel_vio_compression(lock_holder)) {
		data_vio->compression.lock_holder = lock_holder;
		launch_data_vio_packer_callback(data_vio,
						vdo_remove_lock_holder_from_packer);
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
	struct int_map *lock_map = lock->zone->lbn_operations;
	struct data_vio *lock_holder;

	if (!lock->locked) {
		/*
		 * The lock is not locked, so it had better not be registered
		 * in the lock map.
		 */
		struct data_vio *lock_holder = int_map_get(lock_map, lock->lbn);

		ASSERT_LOG_ONLY((data_vio != lock_holder),
				"no logical block lock held for block %llu",
				(unsigned long long) lock->lbn);
		return;
	}

	/* Remove the lock from the logical block lock map, releasing the lock. */
	lock_holder = int_map_remove(lock_map, lock->lbn);
	ASSERT_LOG_ONLY((data_vio == lock_holder),
			"logical block lock mismatch for block %llu",
			(unsigned long long) lock->lbn);
	lock->locked = false;
	return;
}

/**
 * Release the lock on the logical block, if any, that a data_vio has acquired.
 *
 * @param data_vio The data_vio releasing its logical block lock
 **/
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

	/*
	 * Another data_vio is waiting for the lock, so just transfer it in a
	 * single lock map operation
	 */
	next_lock_holder =
		waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));

	/* Transfer the remaining lock waiters to the next lock holder. */
	transfer_all_waiters(&lock->waiters,
			     &next_lock_holder->logical.waiters);

	result = int_map_put(lock->zone->lbn_operations,
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
			(unsigned long long) lock->lbn);
	lock->locked = false;

	/*
	 * If there are still waiters, other data_vios must be trying to get
	 * the lock we just transferred. We must ensure that the new lock
	 * holder doesn't block in the packer.
	 */
	if (has_waiters(&next_lock_holder->logical.waiters)) {
		cancel_vio_compression(next_lock_holder);
	}

	/*
	 * Avoid stack overflow on lock transfer.
	 * XXX: this is only an issue in the 1 thread config.
	 */
	data_vio_as_completion(next_lock_holder)->requeue = true;
	launch_locked_request(next_lock_holder);
}

/**
 * Allocate a data block.
 *
 * @param data_vio         The data_vio which needs an allocation
 * @param write_lock_type  The type of write lock to obtain on the block
 * @param callback         The callback which will attempt an allocation in
 *                         the current zone and continue if it succeeds
 * @param error_handler    The handler for errors while allocating
 **/
void data_vio_allocate_data_block(struct data_vio *data_vio,
				  enum pbn_lock_type write_lock_type,
				  vdo_action *callback,
				  vdo_action *error_handler)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct allocation *allocation = &data_vio->allocation;
	struct allocation_selector *selector =
		data_vio->logical.zone->selector;

	ASSERT_LOG_ONLY((allocation->pbn == VDO_ZERO_BLOCK),
			"data_vio does not have an allocation");
	allocation->write_lock_type = write_lock_type;
	allocation->first_allocation_zone =
		vdo_get_next_allocation_zone(selector);
	allocation->zone =
		&vdo->physical_zones[allocation->first_allocation_zone];

	data_vio_as_completion(data_vio)->error_handler = error_handler;
	launch_data_vio_allocated_zone_callback(data_vio, callback);
}

void release_data_vio_allocation_lock(struct data_vio *data_vio, bool reset)
{
	struct allocation *allocation = &data_vio->allocation;
	physical_block_number_t locked_pbn = allocation->pbn;

	assert_data_vio_in_allocated_zone(data_vio);

	if (reset ||
	    vdo_pbn_lock_has_provisional_reference(allocation->lock)) {
		allocation->pbn = VDO_ZERO_BLOCK;
	}

	vdo_release_physical_zone_pbn_lock(allocation->zone,
					   locked_pbn,
					   UDS_FORGET(allocation->lock));
}

void acknowledge_data_vio(struct data_vio *data_vio)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct bio *bio = data_vio->user_bio;
	int error = vdo_map_to_system_error(data_vio_as_completion(data_vio)->result);

	if (bio == NULL) {
		return;
	}

	ASSERT_LOG_ONLY((data_vio->remaining_discard <=
			 (uint32_t) (VDO_BLOCK_SIZE - data_vio->offset)),
			"data_vio to acknowledge is not an incomplete discard");

	data_vio->user_bio = NULL;
	vdo_count_bios(&vdo->stats.bios_acknowledged, bio);
	if (data_vio->is_partial) {
		vdo_count_bios(&vdo->stats.bios_acknowledged_partial, bio);
	}


	vdo_complete_bio(bio, error);
}

/**
 * A function to compress the data in a data_vio.
 *
 * @param data_vio  The data_vio to compress
 **/
void compress_data_vio(struct data_vio *data_vio)
{
	int size;
	char *context = get_work_queue_private_data();

	/*
         * By putting the compressed data at the start of the compressed
         * block data field, we won't need to copy it if this data_vio
         * becomes a compressed write agent.
         */
	size = LZ4_compress_default(data_vio->data_block,
				    data_vio->compression.block->data,
				    VDO_BLOCK_SIZE,
				    VDO_MAX_COMPRESSED_FRAGMENT_SIZE,
				    context);
	if (size > 0) {
		data_vio->compression.size = size;
	} else {
		/*
		 * Use block size plus one as an indicator for uncompressible
		 * data.
		 */
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
	}
}

/**
 * A function to uncompress the data a data_vio has just read.
 *
 * @param data_vio       The data_vio to uncompress
 * @param mapping_state  The mapping state indicating which fragment to
 *                       decompress
 * @param buffer         The buffer to receive the uncompressed data
 **/
int uncompress_data_vio(struct data_vio *data_vio,
			enum block_mapping_state mapping_state,
			char *buffer)
{
	int size;
	uint16_t fragment_offset, fragment_size;
	struct compressed_block *block = data_vio->compression.block;
	int result = vdo_get_compressed_block_fragment(mapping_state,
						       block,
						       &fragment_offset,
						       &fragment_size);

	if (result != VDO_SUCCESS) {
		uds_log_debug("%s: compressed fragment error %d",
			      __func__,
			      result);
		return result;
	}

	size = LZ4_decompress_safe((block->data + fragment_offset),
				   buffer,
				   fragment_size,
				   VDO_BLOCK_SIZE);
	if (size != VDO_BLOCK_SIZE) {
		uds_log_debug("%s: lz4 error", __func__);
		return VDO_INVALID_FRAGMENT;
	}

	return VDO_SUCCESS;
}
