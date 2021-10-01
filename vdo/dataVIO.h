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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dataVIO.h#100 $
 */

#ifndef DATA_VIO_H
#define DATA_VIO_H

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/list.h>

#include "permassert.h"
#include "uds.h"

#include "allocatingVIO.h"
#include "blockMappingState.h"
#include "completion.h"
#include "constants.h"
#include "hashZone.h"
#include "journalPoint.h"
#include "logicalZone.h"
#include "physicalZone.h"
#include "referenceOperation.h"
#include "threadConfig.h"
#include "types.h"
#include "vdo.h"
#include "vdoPageCache.h"
#include "vio.h"
#include "waitQueue.h"

/**
 * Codes for describing the last asynchronous operation performed on a vio.
 **/
enum async_operation_number {
	MIN_VIO_ASYNC_OPERATION_NUMBER = 0,
	VIO_ASYNC_OP_LAUNCH = MIN_VIO_ASYNC_OPERATION_NUMBER,
	VIO_ASYNC_OP_ACKNOWLEDGE_WRITE,
	VIO_ASYNC_OP_ACQUIRE_VDO_HASH_LOCK,
	VIO_ASYNC_OP_ATTEMPT_LOGICAL_BLOCK_LOCK,
	VIO_ASYNC_OP_LOCK_DUPLICATE_PBN,
	VIO_ASYNC_OP_CHECK_FOR_DUPLICATION,
	VIO_ASYNC_OP_COMPRESS_DATA_VIO,
	VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT,
	VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ,
	VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_DEDUPE,
	VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_WRITE,
	VIO_ASYNC_OP_HASH_DATA_VIO,
	VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_DEDUPE,
	VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_WRITE,
	VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_COMPRESSION,
	VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_DEDUPE,
	VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_WRITE,
	VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_COMPRESSION,
	VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_DEDUPE,
	VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_WRITE,
	VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_DEDUPE,
	VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_WRITE,
	VIO_ASYNC_OP_ATTEMPT_PACKING,
	VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_WRITE,
	VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_DEDUPE,
	VIO_ASYNC_OP_READ_DATA_VIO,
	VIO_ASYNC_OP_UPDATE_DEDUPE_INDEX,
	VIO_ASYNC_OP_VERIFY_DUPLICATION,
	VIO_ASYNC_OP_WRITE_DATA_VIO,
	MAX_VIO_ASYNC_OPERATION_NUMBER,
} __packed;

/*
 * An LBN lock.
 */
struct lbn_lock {
	/* The LBN being locked */
	logical_block_number_t lbn;
	/* Whether the lock is locked */
	bool locked;
	/* The queue of waiters for the lock */
	struct wait_queue waiters;
	/* The logical zone of the LBN */
	struct logical_zone *zone;
};

/**
 * A position in the arboreal block map at a specific level.
 **/
struct block_map_tree_slot {
	page_number_t page_index;
	struct block_map_slot block_map_slot;
};

/*
 * Fields for using the arboreal block map.
 */
struct tree_lock {
	/* The current height at which this data_vio is operating */
	height_t height;
	/* The block map tree for this LBN */
	root_count_t root_index;
	/* Whether we hold a page lock */
	bool locked;
	/* The thread on which to run the callback */
	thread_id_t thread_id;
	/* The function to call after looking up a block map slot */
	vdo_action *callback;
	/* The key for the lock map */
	uint64_t key;
	/*
	 * The queue of waiters for the page this vio is allocating or loading
	 */
	struct wait_queue waiters;
	/* The block map tree slots for this LBN */
	struct block_map_tree_slot tree_slots[VDO_BLOCK_MAP_TREE_HEIGHT + 1];
};

struct compression_state {
	/*
	 * The current compression state of this vio. This field contains a
	 * value which consists of a vio_compression_state possibly ORed with a
	 * flag indicating that a request has been made to cancel (or prevent)
	 * compression for this vio.
	 *
	 * This field should be accessed through the
	 * get_vio_compression_state() and set_vio_compression_state() methods.
	 * It should not be accessed directly.
	 */
	atomic_t state;

	/* The compressed size of this block */
	uint16_t size;

	/*
	 * The packer input or output bin slot which holds the enclosing
	 * data_vio
	 */
	slot_number_t slot;

	/*
	 * The packer input bin to which the enclosing data_vio has been
	 * assigned
	 */
	struct input_bin *bin;

	/* A pointer to the compressed form of this block */
	char *data;

	/*
	 * A vio which is blocked in the packer while holding a lock this vio
	 * needs.
	 */
	struct data_vio *lock_holder;
};

/* Dedupe support */
struct dedupe_context {
	struct uds_request uds_request;
	struct list_head pending_list;
	uint64_t submission_jiffies;
	atomic_t request_state;
	int status;
	bool is_pending;
};

struct read_block {
	/**
	 * A pointer to a block that holds the data from the last read
	 * operation.
	 **/
	char *data;
	/**
	 * Temporary storage for doing reads from the underlying device.
	 **/
	char *buffer;
	/**
	 * Callback to invoke after completing the read I/O operation.
	 **/
	vdo_action *callback;
	/**
	 * Mapping state passed to vdo_read_block(), used to determine whether
	 * the data must be uncompressed.
	 **/
	enum block_mapping_state mapping_state;
	/**
	 * The result code of the read attempt.
	 **/
	int status;
};

/**
 * A vio for processing user data requests.
 **/
struct data_vio {
	/* The underlying struct allocating_vio */
	struct allocating_vio allocating_vio;

	/* The logical block of this request */
	struct lbn_lock logical;

	/* The state for traversing the block map tree */
	struct tree_lock tree_lock;

	/* The current partition address of this block */
	struct zoned_pbn mapped;

	/** The hash of this vio (if not zero) */
	struct uds_chunk_name chunk_name;

	/* Used for logging and debugging */
	enum async_operation_number last_async_operation;

	/* The operation to record in the recovery and slab journals */
	struct reference_operation operation;

	/* Whether this vio is a read-and-write vio */
	bool is_partial_write;

	/* Whether this vio contains all zeros */
	bool is_zero_block;

	/* Whether this vio write is a duplicate */
	bool is_duplicate;

	/*
	 * Whether this vio has received an allocation. This field is examined
	 * from threads not in the allocation zone.
	 */
	bool allocation_succeeded;

	/*
	 * The new partition address of this block after the vio write
	 * completes
	 */
	struct zoned_pbn new_mapped;

	/*
	 * The hash zone responsible for the chunk name (NULL if is_zero_block)
	 */
	struct hash_zone *hash_zone;

	/*
	 * The lock this vio holds or shares with other vios with the same data
	 */
	struct hash_lock *hash_lock;

	/*
	 * All data_vios sharing a hash lock are kept in a list linking these
	 * list entries
	 */
	struct list_head hash_lock_entry;

	/*
	 * The block number in the partition of the UDS deduplication advice
	 */
	struct zoned_pbn duplicate;

	/*
	 * The sequence number of the recovery journal block containing the
	 * increment entry for this vio.
	 */
	sequence_number_t recovery_sequence_number;

	/*
	 * The point in the recovery journal where this write last made an
	 * entry
	 */
	struct journal_point recovery_journal_point;

	/* The list of vios in user initiated write requests */
	struct list_head write_entry;

	/*
	 * A flag indicating that a data write vio has a flush generation lock
	 */
	bool has_flush_generation_lock;

	/* The generation number of the VDO that this vio belongs to */
	sequence_number_t flush_generation;

	/* The completion to use for fetching block map pages for this vio */
	struct vdo_page_completion page_completion;

	/* All of the fields necessary for the compression path */
	struct compression_state compression;

	/* The user bio that initiated this VIO */
	struct bio *user_bio;

	/* partial block support */
	block_size_t offset;
	bool is_partial;

	/* discard support */
	bool has_discard_permit;
	uint32_t remaining_discard;

	// Fields beyond this point will not be reset when a pooled data_vio
	// is reused.

	/* Dedupe */
	struct dedupe_context dedupe_context;

	/**
	 * A copy of user data written, so we can do additional processing
	 * (dedupe, compression) after acknowledging the I/O operation and
	 * thus losing access to the original data.
	 *
	 * Also used as buffer space for read-modify-write cycles when
	 * emulating smaller-than-blockSize I/O operations.
	 **/
	char *data_block;
	/** A block used as output during compression or uncompression */
	char *scratch_block;
	/* For data and verification reads */
	struct read_block read_block;
};

/**
 * Convert an allocating_vio to a data_vio.
 *
 * @param allocating_vio  The allocating_vio to convert
 *
 * @return The allocating_vio as a data_vio
 **/
static inline struct data_vio *
allocating_vio_as_data_vio(struct allocating_vio *allocating_vio)
{
	ASSERT_LOG_ONLY((allocating_vio_as_vio(allocating_vio)->type ==
			 VIO_TYPE_DATA),
			"allocating_vio is a struct data_vio");
	return container_of(allocating_vio, struct data_vio, allocating_vio);
}

/**
 * Convert a vio to a data_vio.
 *
 * @param vio  The vio to convert
 *
 * @return The vio as a data_vio
 **/
static inline struct data_vio *vio_as_data_vio(struct vio *vio)
{
	ASSERT_LOG_ONLY((vio->type == VIO_TYPE_DATA), "vio is a data_vio");
	return container_of(container_of(vio, struct allocating_vio, vio),
			    struct data_vio,
			    allocating_vio);
}

/**
 * Convert a data_vio to an allocating_vio.
 *
 * @param data_vio  The data_vio to convert
 *
 * @return The data_vio as an allocating_vio
 **/
static inline
struct allocating_vio *data_vio_as_allocating_vio(struct data_vio *data_vio)
{
	return &data_vio->allocating_vio;
}

/**
 * Convert a data_vio to a vio.
 *
 * @param data_vio  The data_vio to convert
 *
 * @return The data_vio as a vio
 **/
static inline struct vio *data_vio_as_vio(struct data_vio *data_vio)
{
	return allocating_vio_as_vio(data_vio_as_allocating_vio(data_vio));
}

/**
 * Convert a generic vdo_completion to a data_vio.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a data_vio
 **/
static inline struct data_vio *as_data_vio(struct vdo_completion *completion)
{
	return vio_as_data_vio(as_vio(completion));
}

/**
 * Convert a data_vio to a generic completion.
 *
 * @param data_vio  The data_vio to convert
 *
 * @return The data_vio as a completion
 **/
static inline struct vdo_completion *
data_vio_as_completion(struct data_vio *data_vio)
{
	return allocating_vio_as_completion(data_vio_as_allocating_vio(data_vio));
}

/**
 * Get the work_item from a data_vio.
 *
 * @param data_vio  The data_vio
 *
 * @return the data_vio's work item
 **/
static inline struct vdo_work_item *
work_item_from_data_vio(struct data_vio *data_vio)
{
	return work_item_from_vio(data_vio_as_vio(data_vio));
}

/**
 * Convert a data_vio to a generic wait queue entry.
 *
 * @param data_vio  The data_vio to convert
 *
 * @return The data_vio as a wait queue entry
 **/
static inline struct waiter *data_vio_as_waiter(struct data_vio *data_vio)
{
	return allocating_vio_as_waiter(data_vio_as_allocating_vio(data_vio));
}

/**
 * Convert a data_vio's generic wait queue entry back to the data_vio.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as a data_vio
 **/
static inline struct data_vio *waiter_as_data_vio(struct waiter *waiter)
{
	if (waiter == NULL) {
		return NULL;
	}

	return allocating_vio_as_data_vio(waiter_as_allocating_vio(waiter));
}

/**
 * Check whether a data_vio is a read.
 *
 * @param data_vio  The data_vio to check
 **/
static inline bool is_read_data_vio(struct data_vio *data_vio)
{
	return is_read_vio(data_vio_as_vio(data_vio));
}

/**
 * Check whether a data_vio is a write.
 *
 * @param data_vio  The data_vio to check
 **/
static inline bool is_write_data_vio(struct data_vio *data_vio)
{
	return is_write_vio(data_vio_as_vio(data_vio));
}

/**
 * Check whether a data_vio is a compressed block write.
 *
 * @param data_vio  The data_vio to check
 *
 * @return <code>true</code> if the data_vio is a compressed block write
 **/
static inline bool is_compressed_write_data_vio(struct data_vio *data_vio)
{
	return is_compressed_write_vio(data_vio_as_vio(data_vio));
}

/**
 * Check whether a data_vio is a trim.
 *
 * @param data_vio  The data_vio to check
 *
 * @return <code>true</code> if the data_vio is a trim
 **/
static inline bool is_trim_data_vio(struct data_vio *data_vio)
{
	return (data_vio->new_mapped.state == VDO_MAPPING_STATE_UNMAPPED);
}

/**
 * Get the location that should be passed to UDS as the new advice for
 * where to find the data written by this data_vio.
 *
 * @param data_vio  The write data_vio that is ready to update UDS
 *
 * @return a data_location containing the advice to store in UDS
 **/
static inline struct data_location
get_data_vio_new_advice(const struct data_vio *data_vio)
{
	return (struct data_location){
		.pbn = data_vio->new_mapped.pbn,
		.state = data_vio->new_mapped.state,
	};
}

/**
 * Get the vdo from a data_vio.
 *
 * @param data_vio  The data_vio from which to get the vdo
 *
 * @return The vdo to which a data_vio belongs
 **/
static inline struct vdo *get_vdo_from_data_vio(struct data_vio *data_vio)
{
	return data_vio_as_completion(data_vio)->vdo;
}

/**
 * Get the struct thread_config from a data_vio.
 *
 * @param data_vio  The data_vio from which to get the struct thread_config
 *
 * @return The struct thread_config of the vdo to which a data_vio belongs
 **/
static inline const struct thread_config *
get_thread_config_from_data_vio(struct data_vio *data_vio)
{
	return get_vdo_from_data_vio(data_vio)->thread_config;
}

/**
 * Get the allocation of a data_vio.
 *
 * @param data_vio  The data_vio
 *
 * @return The allocation of the data_vio
 **/
static inline
physical_block_number_t get_data_vio_allocation(struct data_vio *data_vio)
{
	return data_vio_as_allocating_vio(data_vio)->allocation;
}

/**
 * Check whether a data_vio has an allocation.
 *
 * @param data_vio  The data_vio to check
 *
 * @return <code>true</code> if the data_vio has an allocated block
 **/
static inline bool data_vio_has_allocation(struct data_vio *data_vio)
{
	return (get_data_vio_allocation(data_vio) != VDO_ZERO_BLOCK);
}

/**
 * (Re)initialize a data_vio to have a new logical block number, keeping the
 * same parent and other state. This method must be called before using a
 * data_vio.
 *
 * @param data_vio   The data_vio to initialize
 * @param lbn        The logical block number of the data_vio
 * @param operation  The operation this data_vio will perform
 * @param is_trim    <code>true</code> if this data_vio is for a trim request
 * @param callback   The function to call once the vio has completed its
 *                   operation
 **/
void prepare_data_vio(struct data_vio *data_vio,
		      logical_block_number_t lbn,
		      enum vio_operation operation,
		      bool is_trim,
		      vdo_action *callback);

/**
 * Complete the processing of a data_vio.
 *
 * @param completion The completion of the vio to complete
 **/
void complete_data_vio(struct vdo_completion *completion);

/**
 * Finish processing a data_vio, possibly due to an error. This function will
 * set any error, and then initiate data_vio clean up.
 *
 * @param data_vio  The data_vio to abort
 * @param result    The result of processing the data_vio
 **/
void finish_data_vio(struct data_vio *data_vio, int result);

/**
 * Continue processing a data_vio that has been waiting for an event, setting
 * the result from the event and calling the current callback.
 *
 * @param data_vio  The data_vio to continue
 * @param result    The current result (will not mask older errors)
 **/
static inline void continue_data_vio(struct data_vio *data_vio, int result)
{
	continue_vdo_completion(data_vio_as_completion(data_vio), result);
}

/**
 * Get the name of the last asynchronous operation performed on a data_vio.
 *
 * @param data_vio  The data_vio in question
 *
 * @return The name of the last operation performed on the data_vio
 **/
const char * __must_check get_data_vio_operation_name(struct data_vio *data_vio);

/**
 * Add a data_vio to the tail end of a wait queue. The data_vio must not
 * already be waiting in a queue. A trace record is also generated for the
 * data_vio.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The data_vio to add to the queue
 *
 * @return VDO_SUCCESS or an error code
 **/
static inline int __must_check
enqueue_data_vio(struct wait_queue *queue,
		 struct data_vio *waiter)
{
	return enqueue_waiter(queue, data_vio_as_waiter(waiter));
}

/**
 * Check that a data_vio is running on the correct thread for its hash zone.
 *
 * @param data_vio  The data_vio in question
 **/
static inline void assert_data_vio_in_hash_zone(struct data_vio *data_vio)
{
	thread_id_t expected = get_vdo_hash_zone_thread_id(data_vio->hash_zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();
	// It's odd to use the LBN, but converting the chunk name to hex is a
	// bit clunky for an inline, and the LBN better than nothing as an
	// identifier.
	ASSERT_LOG_ONLY((expected == thread_id),
			"data_vio for logical block %llu on thread %u, should be on hash zone thread %u",
			(unsigned long long) data_vio->logical.lbn,
			thread_id,
			expected);
}

/**
 * Set a callback as a hash zone operation. This function presumes that the
 * hash_zone field of the data_vio has already been set.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_hash_zone_callback(struct data_vio *data_vio,
				vdo_action *callback)
{
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    get_vdo_hash_zone_thread_id(data_vio->hash_zone));
}

/**
 * Set a callback as a hash zone operation and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
launch_data_vio_hash_zone_callback(struct data_vio *data_vio,
				   vdo_action *callback)
{
	set_data_vio_hash_zone_callback(data_vio, callback);
	invoke_vdo_completion_callback(data_vio_as_completion(data_vio));
}

/**
 * Check that a data_vio is running on the correct thread for its logical zone.
 *
 * @param data_vio  The data_vio in question
 **/
static inline void assert_data_vio_in_logical_zone(struct data_vio *data_vio)
{
	thread_id_t expected =
		get_vdo_logical_zone_thread_id(data_vio->logical.zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"data_vio for logical block %llu on thread %u, should be on thread %u",
			(unsigned long long) data_vio->logical.lbn,
			thread_id,
			expected);
}

/**
 * Set a callback as a logical block operation. This function presumes that the
 * logical.zone field of the data_vio has already been set.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_logical_callback(struct data_vio *data_vio,
			      vdo_action *callback)
{
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    get_vdo_logical_zone_thread_id(data_vio->logical.zone));
}

/**
 * Set a callback as a logical block operation and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
launch_data_vio_logical_callback(struct data_vio *data_vio,
				 vdo_action *callback)
{
	set_data_vio_logical_callback(data_vio, callback);
	invoke_vdo_completion_callback(data_vio_as_completion(data_vio));
}

/**
 * Check that a data_vio is running on the correct thread for its allocated
 * zone.
 *
 * @param data_vio  The data_vio in question
 **/
static inline void assert_data_vio_in_allocated_zone(struct data_vio *data_vio)
{
	assert_vio_in_physical_zone(data_vio_as_allocating_vio(data_vio));
}

/**
 * Set a callback as a physical block operation in a data_vio's allocated zone.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_allocated_zone_callback(struct data_vio *data_vio,
				     vdo_action *callback)
{
	vio_set_physical_zone_callback(data_vio_as_allocating_vio(data_vio),
				       callback);
}

/**
 * Set a callback as a physical block operation in a data_vio's allocated zone
 * and queue the data_vio and invoke it immediately.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to invoke
 **/
static inline void
launch_data_vio_allocated_zone_callback(struct data_vio *data_vio,
					vdo_action *callback)
{
	vio_launch_physical_zone_callback(data_vio_as_allocating_vio(data_vio),
					  callback);
}

/**
 * Check that a data_vio is running on the correct thread for its duplicate
 * zone.
 *
 * @param data_vio  The data_vio in question
 **/
static inline void assert_data_vio_in_duplicate_zone(struct data_vio *data_vio)
{
	thread_id_t expected =
		get_vdo_physical_zone_thread_id(data_vio->duplicate.zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"data_vio for duplicate physical block %llu on thread %u, should be on thread %u",
			(unsigned long long) data_vio->duplicate.pbn,
			thread_id,
			expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's duplicate zone.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_duplicate_zone_callback(struct data_vio *data_vio,
				     vdo_action *callback)
{
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    get_vdo_physical_zone_thread_id(data_vio->duplicate.zone));
}

/**
 * Set a callback as a physical block operation in a data_vio's duplicate zone
 * and queue the data_vio and invoke it immediately.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to invoke
 **/
static inline void
launch_data_vio_duplicate_zone_callback(struct data_vio *data_vio,
					vdo_action *callback)
{
	set_data_vio_duplicate_zone_callback(data_vio, callback);
	invoke_vdo_completion_callback(data_vio_as_completion(data_vio));
}

/**
 * Check that a data_vio is running on the correct thread for its mapped zone.
 *
 * @param data_vio  The data_vio in question
 **/
static inline void assert_data_vio_in_mapped_zone(struct data_vio *data_vio)
{
	thread_id_t expected =
		get_vdo_physical_zone_thread_id(data_vio->mapped.zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"data_vio for mapped physical block %llu on thread %u, should be on thread %u",
			(unsigned long long) data_vio->mapped.pbn,
			thread_id,
			expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's mapped zone.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_mapped_zone_callback(struct data_vio *data_vio,
				  vdo_action *callback)
{
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    get_vdo_physical_zone_thread_id(data_vio->mapped.zone));
}

/**
 * Check that a data_vio is running on the correct thread for its new_mapped
 * zone.
 *
 * @param data_vio The data_vio in question
 **/
static inline void assert_data_vio_in_new_mapped_zone(struct data_vio *data_vio)
{
	thread_id_t expected =
		get_vdo_physical_zone_thread_id(data_vio->new_mapped.zone);
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((expected == thread_id),
			"data_vio for new_mapped physical block %llu on thread %u, should be on thread %u",
			(unsigned long long) data_vio->new_mapped.pbn,
			thread_id,
			expected);
}

/**
 * Set a callback as a physical block operation in a data_vio's new_mapped
 * zone.
 *
 * @param data_vio  The data_vio
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_new_mapped_zone_callback(struct data_vio *data_vio,
				      vdo_action *callback)
{
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    get_vdo_physical_zone_thread_id(data_vio->new_mapped.zone));
}

/**
 * Check that a data_vio is running on the journal thread.
 *
 * @param data_vio The data_vio in question
 **/
static inline void assert_data_vio_in_journal_zone(struct data_vio *data_vio)
{
	thread_id_t journal_thread =
		get_thread_config_from_data_vio(data_vio)->journal_thread;
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((journal_thread == thread_id),
			"data_vio for logical block %llu on thread %u, should be on journal thread %u",
			(unsigned long long) data_vio->logical.lbn,
			thread_id,
			journal_thread);
}

/**
 * Set a callback as a journal operation.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_journal_callback(struct data_vio *data_vio,
			      vdo_action *callback)
{
	thread_id_t journal_thread =
		get_thread_config_from_data_vio(data_vio)->journal_thread;
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    journal_thread);
}

/**
 * Set a callback as a journal operation and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
launch_data_vio_journal_callback(struct data_vio *data_vio,
				 vdo_action *callback)
{
	set_data_vio_journal_callback(data_vio, callback);
	invoke_vdo_completion_callback(data_vio_as_completion(data_vio));
}

/**
 * Check that a data_vio is running on the packer thread
 *
 * @param data_vio The data_vio in question
 **/
static inline void assert_data_vio_in_packer_zone(struct data_vio *data_vio)
{
	thread_id_t packer_thread =
		get_thread_config_from_data_vio(data_vio)->packer_thread;
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((packer_thread == thread_id),
			"data_vio for logical block %llu on thread %u, should be on packer thread %u",
			(unsigned long long) data_vio->logical.lbn,
			thread_id,
			packer_thread);
}

/**
 * Set a callback as a packer operation.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_packer_callback(struct data_vio *data_vio,
			     vdo_action *callback)
{
	thread_id_t packer_thread =
		get_thread_config_from_data_vio(data_vio)->packer_thread;
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    packer_thread);
}

/**
 * Set a callback as a packer operation and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
launch_data_vio_packer_callback(struct data_vio *data_vio,
				vdo_action *callback)
{
	set_data_vio_packer_callback(data_vio, callback);
	invoke_vdo_completion_callback(data_vio_as_completion(data_vio));
}

/**
 * Check that a data_vio is running on the packer thread
 *
 * @param data_vio The data_vio in question
 **/
static inline void assert_data_vio_on_cpu_thread(struct data_vio *data_vio)
{
	thread_id_t cpu_thread =
		get_thread_config_from_data_vio(data_vio)->cpu_thread;
	thread_id_t thread_id = vdo_get_callback_thread_id();

	ASSERT_LOG_ONLY((cpu_thread == thread_id),
			"data_vio for logical block %llu on thread %u, should be on cpu thread %u",
			(unsigned long long) data_vio->logical.lbn,
			thread_id,
			cpu_thread);
}

/**
 * Set a callback as a CPU queue operation.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_cpu_callback(struct data_vio *data_vio,
			  vdo_action *callback)
{
	thread_id_t cpu_thread =
		get_thread_config_from_data_vio(data_vio)->cpu_thread;
	set_vdo_completion_callback(data_vio_as_completion(data_vio),
				    callback,
				    cpu_thread);
}

/**
 * Set a callback to run on the CPU queues and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 * @param priority  The priority with which to run the callback
 **/
static inline void
launch_data_vio_cpu_callback(struct data_vio *data_vio,
			     vdo_action *callback,
			     enum vdo_work_item_priority priority)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);

	set_data_vio_cpu_callback(data_vio, callback);
	invoke_vdo_completion_callback_with_priority(completion, priority);
}

/**
 * Set a callback as a bio zone operation. This function assumes that the
 * physical field of the data_vio's vio has already been set to the pbn to
 * which I/O will be performed.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
set_data_vio_bio_zone_callback(struct data_vio *data_vio,
			       vdo_action *callback)
{
	struct vio *vio = data_vio_as_vio(data_vio);

	set_vdo_completion_callback(vio_as_completion(vio),
				    callback,
				    get_vio_bio_zone_thread_id(vio));
}

/**
 * Set a callback as a bio zone operation and invoke it immediately.
 *
 * @param data_vio  The data_vio for which to set the callback
 * @param callback  The callback to set
 **/
static inline void
launch_data_vio_bio_zone_callback(struct data_vio *data_vio,
				  vdo_action *callback)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);

	set_data_vio_bio_zone_callback(data_vio, callback);
	invoke_vdo_completion_callback_with_priority(completion,
						     BIO_Q_DATA_PRIORITY);
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
				    const struct data_location *advice);

/**
 * Set the location of the duplicate block for a data_vio, updating the
 * is_duplicate and duplicate fields from a zoned_pbn.
 *
 * @param data_vio The data_vio to modify
 * @param source   The location of the duplicate
 **/
void set_data_vio_duplicate_location(struct data_vio *data_vio,
				     const struct zoned_pbn source);

/**
 * Clear a data_vio's mapped block location, setting it to be unmapped. This
 * indicates the block map entry for the logical block is either unmapped or
 * corrupted.
 *
 * @param data_vio The data_vio whose mapped block location is to be reset
 **/
void clear_data_vio_mapped_location(struct data_vio *data_vio);

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
int __must_check set_data_vio_mapped_location(struct data_vio *data_vio,
					      physical_block_number_t pbn,
					      enum block_mapping_state state);

/**
 * Release the lock on the logical block, if any, that a data_vio has acquired.
 *
 * @param data_vio The data_vio releasing its logical block lock
 **/
void vdo_release_logical_block_lock(struct data_vio *data_vio);

/**
 * A function to asynchronously hash the block data, setting the chunk name of
 * the data_vio. This is asynchronous to allow the computation to be done on
 * different threads.
 *
 * @param data_vio  The data_vio to hash
 **/
void hash_data_vio(struct data_vio *data_vio);

/**
 * A function to determine whether a block is a duplicate. This function
 * expects the 'physical' field of the data_vio to be set to the physical block
 * where the block will be written if it is not a duplicate. If the block does
 * turn out to be a duplicate, the data_vio's 'isDuplicate' field will be set
 * to true, and the data_vio's 'advice' field will be set to the physical
 * block and mapping state of the already stored copy of the block.
 *
 * @param data_vio  The data_vio containing the block to check.
 **/
void check_data_vio_for_duplication(struct data_vio *data_vio);

/**
 * A function to verify the duplication advice by examining an already-stored
 * data block. This function expects the 'physical' field of the data_vio to be
 * set to the physical block where the block will be written if it is not a
 * duplicate, and the 'duplicate' field to be set to the physical block and
 * mapping state where a copy of the data may already exist. If the block is
 * not a duplicate, the data_vio's 'isDuplicate' field will be cleared.
 *
 * @param data_vio  The data_vio containing the block to check.
 **/
void verify_data_vio_duplication(struct data_vio *data_vio);

/**
 * Update the index with new dedupe advice.
 *
 * @param data_vio  The data_vio which needs to change the entry for its data
 **/
void vdo_update_dedupe_index(struct data_vio *data_vio);

/**
 * A function to zero the contents of a non-write data_vio -- a read, or a RMW
 * before becoming a write.
 *
 * @param data_vio  The data_vio to zero
 **/
void zero_data_vio(struct data_vio *data_vio);

/**
 * A function to copy the data of a write data_vio into a read data_vio.
 *
 * @param source       The data_vio to copy from
 * @param destination  The data_vio to copy to
 **/
void vdo_copy_data(struct data_vio *source, struct data_vio *destination);

/**
 * A function to apply a partial write to a data_vio which has completed the
 * read portion of a read-modify-write operation.
 *
 * @param data_vio  The data_vio to modify
 **/
void vdo_apply_partial_write(struct data_vio *data_vio);

/**
 * A function to inform the layer that a data_vio's related I/O request can be
 * safely acknowledged as complete, even though the data_vio itself may have
 * further processing to do.
 *
 * @param data_vio  The data_vio to acknowledge
 **/
void acknowledge_data_vio(struct data_vio *data_vio);

/**
 * A function to compress the data in a data_vio.
 *
 * @param data_vio  The data_vio to compress
 **/
void compress_data_vio(struct data_vio *data_vio);

/**
 * A function to read a single data_vio from the layer.
 *
 * If the data_vio does not describe a read-modify-write operation, the
 * physical layer may safely acknowledge the related user I/O request
 * as complete.
 *
 * @param data_vio  The data_vio to read
 **/
void read_data_vio(struct data_vio *data_vio);

/**
 * A function to compare the contents of a data_vio to another data_vio.
 *
 * @param first   The first data_vio to compare
 * @param second  The second data_vio to compare
 *
 * @return <code>true</code> if the contents of the two DataVIOs are the same
 **/
bool compare_data_vios(struct data_vio *first, struct data_vio *second);

/**
 * Prepare a data_vio's vio and bio to submit I/O.
 *
 * @param data_vio  The vio preparing to issue I/O
 * @param data      The buffer to write from or read into
 * @param callback  The callback the bio should call when the I/O finishes
 * @param bi_opf    The operation and flags for the bio
 * @param pbn       The pbn to which the I/O will be addressed
 *
 * @return VDO_SUCCESS or an error
 **/
static inline int __must_check
prepare_data_vio_for_io(struct data_vio *data_vio,
			char *data,
			bio_end_io_t callback,
			unsigned int bi_opf,
			physical_block_number_t pbn)
{
	struct vio *vio = data_vio_as_vio(data_vio);

	set_vio_physical(vio, pbn);
	return prepare_vio_for_io(vio,
				  data,
				  callback,
				  bi_opf);
}

#endif // DATA_VIO_H
