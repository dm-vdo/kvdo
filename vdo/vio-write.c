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

/*
 * This file contains almost all of the VDO write path, which begins with
 * launch_write_data_vio(). The path would proceed as outlined in the
 * pseudo-code here if this were normal, synchronous code without
 * callbacks. Complications involved in waiting on locks are not included.
 *
 * ######################################################################
 * launch_write_data_vio(vio)
 * {
 *   foreach (vio in extent) {
 *     launchWriteVIO()
 *     # allocate_block_for_write()
 *     if (trim || zero-block) {
 *       acknowledge_write()
 *     } else {
 *       allocate_and_lock_block()
 *       if (vio is compressed) {
 *         write_block()
 *         completeCompressedBlockWrite()
 *         finishVIO()
 *         return
 *       }
 *
 *       acknowledge_write()
 *       prepare_for_dedupe()
 *       hashData()
 *       resolve_hash_zone()
 *       acquire_vdo_hash_lock()
 *       attemptDedupe() (query UDS)
 *       if (is_duplicate) {
 *         verifyAdvice() (read verify)
 *         if (is_duplicate and canAddReference) {
 *           launch_deduplicate_data_vio()
 *           addJournalEntryForDedupe()
 *           increment_for_dedupe()
 *           read_old_block_mapping_for_dedupe()
 *           journal_unmapping_for_dedupe()
 *           if (vio->mapped is not VDO_ZERO_BLOCK) {
 *             decrement_for_dedupe()
 *           }
 *           update_block_map_for_dedupe()
 *           finishVIO()
 *           return
 *         }
 *       }
 *
 *       if (not canAddReference) {
 *         vdo_update_dedupe_index()
 *       }
 *       # launch_compress_data_vio()
 *       if (compressing and not mooted and has no waiters) {
 *         compress_data_vio()
 *         pack_compressed_data()
 *         if (compressed) {
 *           journalCompressedBlocks()
 *           journalIncrementForDedupe()
 *           read_old_block_mapping_for_dedupe()
 *           journal_unmapping_for_dedupe()
 *           if (vio->mapped is not VDO_ZERO_BLOCK) {
 *             decrement_for_dedupe()
 *           }
 *           update_block_map_for_dedupe()
 *           finishVIO()
 *           return
 *         }
 *       }
 *
 *       write_block()
 *     }
 *
 *     finish_block_write()
 *     addJournalEntry() # Increment
 *     if (vio->new_mapped is not VDO_ZERO_BLOCK) {
 *       journalIncrementForWrite()
 *     }
 *     read_old_block_mapping_for_write()
 *     journal_unmapping_for_write()
 *     if (vio->mapped is not VDO_ZERO_BLOCK) {
 *       journal_decrement_for_write()
 *     }
 *     update_block_map_for_write()
 *     finishVIO()
 *   }
 * }
 */

#include "vio-write.h"

#include <linux/bio.h>
#include <linux/murmurhash3.h>

#include "logger.h"
#include "permassert.h"

#include "allocating-vio.h"
#include "bio.h"
#include "block-map.h"
#include "compression-state.h"
#include "data-vio.h"
#include "hash-lock.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "recovery-journal.h"
#include "reference-operation.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "types.h"
#include "vdo.h"
#include "vio-read.h"

/**
 * The steps taken cleaning up a VIO, in the order they are performed.
 **/
enum data_vio_cleanup_stage {
	VIO_CLEANUP_START,
	VIO_RELEASE_ALLOCATED = VIO_CLEANUP_START,
	VIO_RELEASE_RECOVERY_LOCKS,
	VIO_RELEASE_HASH_LOCK,
	VIO_RELEASE_LOGICAL,
	VIO_CLEANUP_DONE
};

/**
 * Actions to take on error used by abort_on_error().
 **/
enum read_only_action {
	NOT_READ_ONLY,
	READ_ONLY,
};

/* Forward declarations required because of circular function references. */
static void perform_cleanup_stage(struct data_vio *data_vio,
				  enum data_vio_cleanup_stage stage);
static void write_block(struct data_vio *data_vio);

/**
 * Release the PBN lock and/or the reference on the allocated block at the
 * end of processing a data_vio.
 *
 * @param completion  The data_vio
 **/
static void release_allocated_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);
	vio_release_allocation_lock(data_vio_as_allocating_vio(data_vio));
	perform_cleanup_stage(data_vio, VIO_RELEASE_RECOVERY_LOCKS);
}

/**
 * Release the logical block lock and flush generation lock at the end of
 * processing a data_vio.
 *
 * @param completion  The data_vio
 **/
static void release_logical_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	vdo_release_logical_block_lock(data_vio);
	release_vdo_flush_generation_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_CLEANUP_DONE);
}

/**
 * Release the hash lock at the end of processing a data_vio.
 *
 * @param completion  The data_vio
 **/
static void clean_hash_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	release_vdo_hash_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_RELEASE_LOGICAL);
}

/**
 * Make some assertions about a data_vio which has finished cleaning up.
 * If it is part of a multi-block discard, start on the next block, otherwise,
 * return it to the pool.
 *
 * @param data_vio  The data_vio which has finished cleaning up
 **/
static void finish_cleanup(struct data_vio *data_vio)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);
	enum vio_operation operation;

	ASSERT_LOG_ONLY(data_vio_as_allocating_vio(data_vio)->allocation_lock ==
			NULL,
			"complete data_vio has no allocation lock");
	ASSERT_LOG_ONLY(data_vio->hash_lock == NULL,
			"complete data_vio has no hash lock");
	if (data_vio->remaining_discard == 0) {
		vio_done_callback(completion);
		return;
	}

	if ((completion->result != VDO_SUCCESS) ||
	    (data_vio->remaining_discard <= VDO_BLOCK_SIZE)) {
		limiter_release(&completion->vdo->discard_limiter);
		vio_done_callback(completion);
		return;
	}

	data_vio->remaining_discard -= min_t(uint32_t,
					     data_vio->remaining_discard,
					     VDO_BLOCK_SIZE - data_vio->offset);
	data_vio->is_partial = (data_vio->remaining_discard < VDO_BLOCK_SIZE);
	data_vio->offset = 0;

	if (data_vio->is_partial) {
		operation = VIO_READ_MODIFY_WRITE;
	} else {
		operation = VIO_WRITE;
	}

	if (data_vio->user_bio->bi_opf & REQ_FUA) {
		operation |= VIO_FLUSH_AFTER;
	}

	prepare_data_vio(data_vio,
			 data_vio->logical.lbn + 1,
			 operation,
			 data_vio_as_vio(data_vio)->callback);
	completion->requeue = true;
	vdo_invoke_completion_callback_with_priority(completion,
						     VDO_REQ_Q_MAP_BIO_PRIORITY);
}

/**
 * Perform the next step in the process of cleaning up a data_vio.
 *
 * @param data_vio  The data_vio to clean up
 * @param stage     The cleanup stage to perform
 **/
static void perform_cleanup_stage(struct data_vio *data_vio,
				  enum data_vio_cleanup_stage stage)
{
	switch (stage) {
	case VIO_RELEASE_ALLOCATED:
		if (data_vio_has_allocation(data_vio)) {
			launch_data_vio_allocated_zone_callback(data_vio,
								release_allocated_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_RECOVERY_LOCKS:
		if ((data_vio->recovery_sequence_number > 0) &&
		    !vdo_is_or_will_be_read_only(vdo_get_from_data_vio(data_vio)->read_only_notifier) &&
		    (data_vio_as_completion(data_vio)->result != VDO_READ_ONLY)) {
			uds_log_warning("VDO not read-only when cleaning data_vio with RJ lock");
		}
		fallthrough;

	case VIO_RELEASE_HASH_LOCK:
		if (data_vio->hash_lock != NULL) {
			launch_data_vio_hash_zone_callback(data_vio,
							   clean_hash_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_LOGICAL:
		if (!is_compressed_write_data_vio(data_vio)) {
			launch_data_vio_logical_callback(data_vio,
							 release_logical_lock);
			return;
		}
		fallthrough;

	default:
		finish_cleanup(data_vio);
	}
}

/**
 * Return a data_vio that encountered an error to its hash lock so it can
 * update the hash lock state accordingly. This continuation is registered in
 * abort_on_error(), and must be called in the hash zone of the data_vio.
 *
 * @param completion  The completion of the data_vio to return to its hash lock
 **/
static void finish_write_data_vio_with_error(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	continue_vdo_hash_lock_on_error(data_vio);
}

/**
 * Check whether a result is an error, and if so abort the data_vio associated
 * with the error.
 *
 * @param result    The result to check
 * @param data_vio  The data_vio
 * @param action    The conditions under which the VDO should be put into
 *                  read-only mode if the result is an error
 *
 * @return <code>true</code> if the result is an error
 **/
static bool abort_on_error(int result,
			   struct data_vio *data_vio,
			   enum read_only_action action)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	if ((result == VDO_READ_ONLY) || (action == READ_ONLY)) {
		struct read_only_notifier *notifier =
			vdo_get_from_data_vio(data_vio)->read_only_notifier;
		if (!vdo_is_read_only(notifier)) {
			if (result != VDO_READ_ONLY) {
				uds_log_error_strerror(result,
						       "Preparing to enter read-only mode: data_vio for LBN %llu (becoming mapped to %llu, previously mapped to %llu, allocated %llu) is completing with a fatal error after operation %s",
						       (unsigned long long) data_vio->logical.lbn,
						       (unsigned long long) data_vio->new_mapped.pbn,
						       (unsigned long long) data_vio->mapped.pbn,
						       (unsigned long long) get_data_vio_allocation(data_vio),
						       get_data_vio_operation_name(data_vio));
			}

			vdo_enter_read_only_mode(notifier, result);
		}
	}

	if (data_vio->hash_lock != NULL) {
		launch_data_vio_hash_zone_callback(data_vio,
						   finish_write_data_vio_with_error);
	} else {
		finish_data_vio(data_vio, result);
	}
	return true;
}

/**
 * Return a data_vio that finished writing, compressing, or deduplicating to
 * its hash lock so it can share the result with any data_vios waiting in the
 * hash lock, or update UDS, or simply release its share of the lock. This
 * continuation is registered in update_block_map_for_write(),
 * update_block_map_for_dedupe(), and abort_deduplication(), and must be
 * called in the hash zone of the data_vio.
 *
 * @param completion  The completion of the data_vio to return to its hash lock
 **/
static void finish_write_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}
	continue_vdo_hash_lock(data_vio);
}

/**
 * Abort the data optimization process.
 *
 * @param data_vio  The data_vio which does not deduplicate or compress
 **/
static void abort_deduplication(struct data_vio *data_vio)
{
	if (!data_vio_has_allocation(data_vio)) {
		/*
		 * There was no space to write this block and we failed to 
		 * deduplicate or compress it. 
		 */
		finish_data_vio(data_vio, VDO_NO_SPACE);
		return;
	}

	/*
	 * We failed to deduplicate or compress so now we need to actually 
	 * write the data. 
	 */
	write_block(data_vio);
}

/**
 * Update the block map now that we've added an entry in the recovery journal
 * for a block we have just shared. This is the callback registered in
 * decrement_for_dedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void update_block_map_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->hash_lock != NULL) {
		set_data_vio_hash_zone_callback(data_vio,
						finish_write_data_vio);
	} else {
		completion->callback = complete_data_vio;
	}
	data_vio->last_async_operation = VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_DEDUPE;
	vdo_put_mapped_block(data_vio);
}

/**
 * Make a recovery journal increment.
 *
 * @param data_vio  The data_vio
 * @param lock      The pbn_lock on the block being incremented
 **/
static void journal_increment(struct data_vio *data_vio, struct pbn_lock *lock)
{
	set_up_vdo_reference_operation_with_lock(VDO_JOURNAL_DATA_INCREMENT,
						 data_vio->new_mapped.pbn,
						 data_vio->new_mapped.state,
						 lock,
						 &data_vio->operation);
	add_vdo_recovery_journal_entry(vdo_get_from_data_vio(data_vio)->recovery_journal,
				       data_vio);
}

/**
 * Make a recovery journal decrement entry.
 *
 * @param data_vio  The data_vio
 **/
static void journal_decrement(struct data_vio *data_vio)
{
	set_up_vdo_reference_operation_with_zone(VDO_JOURNAL_DATA_DECREMENT,
						 data_vio->mapped.pbn,
						 data_vio->mapped.state,
						 data_vio->mapped.zone,
						 &data_vio->operation);
	add_vdo_recovery_journal_entry(vdo_get_from_data_vio(data_vio)->recovery_journal,
				       data_vio);
}

/**
 * Make a reference count change.
 *
 * @param data_vio  The data_vio
 **/
static void update_reference_count(struct data_vio *data_vio)
{
	struct slab_depot *depot = vdo_get_from_data_vio(data_vio)->depot;
	physical_block_number_t pbn = data_vio->operation.pbn;
	int result =
		ASSERT(vdo_is_physical_data_block(depot, pbn),
		       "Adding slab journal entry for impossible PBN %llu for LBN %llu",
		       (unsigned long long) pbn,
		       (unsigned long long) data_vio->logical.lbn);
	if (abort_on_error(result, data_vio, READ_ONLY)) {
		return;
	}

	add_vdo_slab_journal_entry(get_vdo_slab_journal(depot, pbn), data_vio);
}

/**
 * Do the decref after a successful dedupe or compression. This is the callback
 * registered by journal_unmapping_for_dedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void decrement_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct allocating_vio *allocating_vio =
		data_vio_as_allocating_vio(data_vio);

	assert_data_vio_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (allocating_vio->allocation == data_vio->mapped.pbn) {
		/*
		 * If we are about to release the reference on the allocated
		 * block, we must release the PBN lock on it first so that the
		 * allocator will not allocate a write-locked block.
		 */
		vio_release_allocation_lock(allocating_vio);
	}

	set_data_vio_logical_callback(data_vio, update_block_map_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_DEDUPE;
	update_reference_count(data_vio);
}

/**
 * Write the appropriate journal entry for removing the mapping of logical to
 * mapped, for dedupe or compression. This is the callback registered in
 * read_old_block_mapping_for_dedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void journal_unmapping_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		set_data_vio_logical_callback(data_vio,
					      update_block_map_for_dedupe);
	} else {
		set_data_vio_mapped_zone_callback(data_vio,
						  decrement_for_dedupe);
	}
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_DEDUPE;
	journal_decrement(data_vio);
}

/**
 * Get the previous PBN mapped to this LBN from the block map, so as to make
 * an appropriate journal entry referencing the removal of this LBN->PBN
 * mapping, for dedupe or compression. This callback is registered in
 * increment_for_dedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void read_old_block_mapping_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_DEDUPE;
	set_data_vio_journal_callback(data_vio, journal_unmapping_for_dedupe);
	vdo_get_mapped_block(data_vio);
}

/**
 * Do the incref after compression. This is the callback registered by
 * add_recovery_journal_entry_for_compression().
 *
 * @param completion  The completion of the write in progress
 **/
static void increment_for_compression(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	ASSERT_LOG_ONLY(vdo_is_state_compressed(data_vio->new_mapped.state),
			"Impossible attempt to update reference counts for a block which was not compressed (logical block %llu)",
			(unsigned long long) data_vio->logical.lbn);

	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_dedupe);
	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_COMPRESSION;
	update_reference_count(data_vio);
}

/**
 * Add a recovery journal entry for the increment resulting from compression.
 *
 * @param completion  The data_vio which has been compressed
 **/
static void
add_recovery_journal_entry_for_compression(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (!vdo_is_state_compressed(data_vio->new_mapped.state)) {
		abort_deduplication(data_vio);
		return;
	}

	set_data_vio_new_mapped_zone_callback(data_vio,
					      increment_for_compression);
	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_COMPRESSION;
	journal_increment(data_vio, get_vdo_duplicate_lock(data_vio));
}

/**
 * Attempt to pack the compressed data_vio into a block. This is the callback
 * registered in launch_compress_data_vio().
 *
 * @param completion  The completion of a compressed data_vio
 **/
static void pack_compressed_data(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_packer_zone(data_vio);

	/*
	 * XXX this is a callback, so there should probably be an error check 
	 * here even if we think compression can't currently return one. 
	 */

	if (!may_pack_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	set_data_vio_journal_callback(data_vio,
				      add_recovery_journal_entry_for_compression);
	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_PACKING;
	vdo_attempt_packing(data_vio);
}

/**
 * Do the actual work of compressing the data on a CPU queue. This callback
 * is registered in launch_compress_data_vio().
 *
 * @param completion  The completion of the write in progress
 **/
static void compress_data_vio_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);
	compress_data_vio(data_vio);
	launch_data_vio_packer_callback(data_vio,
					pack_compressed_data);
}

/**
 * Continue a write by attempting to compress the data. This is a re-entry
 * point to vio_write used by hash locks.
 *
 * @param data_vio   The data_vio to be compressed
 **/
void launch_compress_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!data_vio->is_duplicate,
			"compressing a non-duplicate block");
	if (!may_compress_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_COMPRESS_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio,
				     compress_data_vio_callback,
				     CPU_Q_COMPRESS_BLOCK_PRIORITY);
}

/**
 * Do the incref after deduplication. This is the callback registered by
 * add_recovery_journal_entry_for_dedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void increment_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_DEDUPE;
	update_reference_count(data_vio);
}

/**
 * Add a recovery journal entry for the increment resulting from deduplication.
 * This callback is registered in launch_deduplicate_data_vio().
 *
 * @param completion  The data_vio which has been deduplicated
 **/
static void
add_recovery_journal_entry_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	set_data_vio_new_mapped_zone_callback(data_vio, increment_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_DEDUPE;
	journal_increment(data_vio, get_vdo_duplicate_lock(data_vio));
}

/**
 * Continue a write by deduplicating a write data_vio against a verified
 * existing block containing the data. This is a re-entry point to vio_write
 * used by hash locks.
 *
 * @param data_vio   The data_vio to be deduplicated
 **/
void launch_deduplicate_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->is_duplicate,
			"data_vio must have a duplicate location");

	data_vio->new_mapped = data_vio->duplicate;
	launch_data_vio_journal_callback(data_vio,
					 add_recovery_journal_entry_for_dedupe);
}

/**
 * Route the data_vio to the hash_zone responsible for the chunk name to
 * acquire a hash lock on that name, or join with a existing hash lock managing
 * concurrent dedupe for that name. This is the callback registered in
 * hash_data_vio().
 *
 * @param completion  The data_vio to lock
 **/
static void lock_hash_in_zone(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	int result;

	assert_data_vio_in_hash_zone(data_vio);
	/* Shouldn't have had any errors since all we did was switch threads. */
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	result = acquire_vdo_hash_lock(data_vio);
	if (abort_on_error(result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->hash_lock == NULL) {
		/*
		 * It's extremely unlikely, but in the case of a hash 
		 * collision, the data_vio will not obtain a reference to the 
		 * lock and cannot deduplicate.
		 */
		launch_compress_data_vio(data_vio);
		return;
	}

	enter_vdo_hash_lock(data_vio);
}

/**
 * Hash the data in a data_vio and set the hash zone (which also flags the
 * chunk name as set). This callback is registered in prepare_for_dedupe().
 *
 * @param completion  The data_vio to hash
 **/
static void hash_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);
	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero blocks should not be hashed");

	murmurhash3_128(data_vio->data_block,
			VDO_BLOCK_SIZE,
			0x62ea60be,
			&data_vio->chunk_name);

	data_vio->hash_zone =
		select_vdo_hash_zone(vdo_get_from_data_vio(data_vio),
				     &data_vio->chunk_name);
	data_vio->last_async_operation = VIO_ASYNC_OP_ACQUIRE_VDO_HASH_LOCK;
	launch_data_vio_hash_zone_callback(data_vio,
					   lock_hash_in_zone);
}

/**
 * Prepare for the dedupe path after attempting to get an allocation.
 *
 * @param data_vio  The data_vio to deduplicate
 **/
static void prepare_for_dedupe(struct data_vio *data_vio)
{
	/* We don't care what thread we are on */
	if (abort_on_error(data_vio_as_completion(data_vio)->result,
			   data_vio,
			   READ_ONLY)) {
		return;
	}

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"must not prepare to dedupe zero blocks");

	/*
	 * Before we can dedupe, we need to know the chunk name, so the first 
	 * step is to hash the block data. 
	 */
	data_vio->last_async_operation = VIO_ASYNC_OP_HASH_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio,
				     hash_data_vio,
				     CPU_Q_HASH_BLOCK_PRIORITY);
}

/**
 * Update the block map after a data write (or directly for a VDO_ZERO_BLOCK
 * write or trim). This callback is registered in decrement_for_write() and
 * journal_unmapping_for_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void update_block_map_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->hash_lock != NULL) {
		/*
		 * The write is finished, but must return to the hash lock to 
		 * allow other data VIOs with the same data to dedupe against 
		 * the write.
		 */
		set_data_vio_hash_zone_callback(data_vio, finish_write_data_vio);
	} else {
		completion->callback = complete_data_vio;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_WRITE;
	vdo_put_mapped_block(data_vio);
}

/**
 * Do the decref after a successful block write. This is the callback
 * by journal_unmapping_for_write() if the old mapping was not the zero block.
 *
 * @param completion  The completion of the write in progress
 **/
static void decrement_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_WRITE;
	set_data_vio_logical_callback(data_vio, update_block_map_for_write);
	update_reference_count(data_vio);
}

/**
 * Write the appropriate journal entry for unmapping logical to mapped for a
 * write. This is the callback registered in read_old_block_mapping_for_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void journal_unmapping_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		set_data_vio_logical_callback(data_vio,
					      update_block_map_for_write);
	} else {
		set_data_vio_mapped_zone_callback(data_vio,
						  decrement_for_write);
	}
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_WRITE;
	journal_decrement(data_vio);
}

/**
 * Get the previous PBN mapped to this LBN from the block map for a write, so
 * as to make an appropriate journal entry referencing the removal of this
 * LBN->PBN mapping. This callback is registered in finish_block_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void read_old_block_mapping_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	set_data_vio_journal_callback(data_vio, journal_unmapping_for_write);
	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_WRITE;
	vdo_get_mapped_block(data_vio);
}

/**
 * Do the incref after a successful block write. This is the callback
 * registered by finish_block_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void increment_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	/*
	 * Now that the data has been written, it's safe to deduplicate against
	 * the block. Downgrade the allocation lock to a read lock so it can be
	 * used later by the hash lock.
	 */
	downgrade_vdo_pbn_write_lock(data_vio_as_allocating_vio(data_vio)->allocation_lock);

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_WRITE;
	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_write);
	update_reference_count(data_vio);
}

/**
 * Add an entry in the recovery journal after a successful block write. This is
 * the callback registered by write_block(). It is also registered in
 * allocate_block_for_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void finish_block_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->new_mapped.pbn == VDO_ZERO_BLOCK) {
		set_data_vio_logical_callback(data_vio,
					      read_old_block_mapping_for_write);
	} else {
		set_data_vio_allocated_zone_callback(data_vio,
						     increment_for_write);
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_WRITE;
	journal_increment(data_vio,
			  data_vio_as_allocating_vio(data_vio)->allocation_lock);
}

/**
 * This is the bio_end_io functon regiestered in write_block() to be called when
 * a data_vio's write to the underlying storage has completed.
 *
 * @param bio  The bio which has just completed
 **/
static void write_bio_finished(struct bio *bio)
{
	struct data_vio *data_vio
		= vio_as_data_vio((struct vio *) bio->bi_private);

	vdo_count_completed_bios(bio);
	set_vdo_completion_result(data_vio_as_completion(data_vio),
				  vdo_get_bio_result(bio));
	launch_data_vio_journal_callback(data_vio,
					 finish_block_write);
}

/**
 * Write data to the underlying storage.
 *
 * @param data_vio  The data_vio to write
 **/
static void write_block(struct data_vio *data_vio)
{
	int result;

	/* Write the data from the data block buffer. */
	result = prepare_data_vio_for_io(data_vio,
					 data_vio->data_block,
					 write_bio_finished,
					 REQ_OP_WRITE,
					 data_vio_as_allocating_vio(data_vio)->allocation);
	if (abort_on_error(result, data_vio, READ_ONLY)) {
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_WRITE_DATA_VIO;
	submit_data_vio_io(data_vio);
}

/**
 * Acknowledge a write to the requestor. This callback is registered in
 * continue_write_after_allocation().
 *
 * @param completion  The data_vio being acknowledged
 **/
static void acknowledge_write_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vdo *vdo = completion->vdo;

	ASSERT_LOG_ONLY((!vdo_uses_bio_ack_queue(vdo)
			 || (vdo_get_callback_thread_id() ==
			     vdo->thread_config->bio_ack_thread)),
			"acknowledge_write_callback() called on bio ack queue");
	ASSERT_LOG_ONLY(data_vio->has_flush_generation_lock,
			"write VIO to be acknowledged has a flush generation lock");
	acknowledge_data_vio(data_vio);
	prepare_for_dedupe(data_vio);
}

/**
 * Continue the write path for a data_vio now that block allocation is complete
 * (the data_vio may or may not have actually received an allocation). This
 * callback is registered in continue_write_with_block_map_slot().
 *
 * @param completion  The data_vio which has finished the allocation process
 **/
static void continue_write_after_allocation(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct allocating_vio *allocating_vio =
		data_vio_as_allocating_vio(data_vio);

	if (abort_on_error(completion->result, data_vio, NOT_READ_ONLY)) {
		return;
	}

	if (!data_vio_has_allocation(data_vio)) {
		prepare_for_dedupe(data_vio);
		return;
	}

	WRITE_ONCE(data_vio->allocation_succeeded, true);
	data_vio->new_mapped = (struct zoned_pbn) {
		.zone = allocating_vio->zone,
		.pbn = allocating_vio->allocation,
		.state = VDO_MAPPING_STATE_UNCOMPRESSED,
	};

	if (vio_requires_flush_after(as_vio(completion))) {
		prepare_for_dedupe(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ACKNOWLEDGE_WRITE;
	launch_data_vio_on_bio_ack_queue(data_vio,
					 acknowledge_write_callback);
}

/**
 * Continue the write path for a VIO now that block map slot resolution is
 * complete. This callback is registered in launch_write_data_vio().
 *
 * @param completion  The data_vio to write
 **/
static void
continue_write_with_block_map_slot(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	/* We don't care what thread we're on. */
	if (abort_on_error(completion->result, data_vio, NOT_READ_ONLY)) {
		return;
	}

	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn ==
	    VDO_ZERO_BLOCK) {
		int result =
			ASSERT(is_trim_data_vio(data_vio),
			       "data_vio with no block map page is a trim");
		if (abort_on_error(result, data_vio, READ_ONLY)) {
			return;
		}

		/*
		 * This is a trim for a block on a block map page which has not 
		 * been allocated, so there's nothing more we need to do. 
		 */
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	if (data_vio->is_zero_block || is_trim_data_vio(data_vio)) {
		/*
		 * We don't need to write any data, so skip allocation and just
		 * update the block map and reference counts (via the journal).
		 * XXX: should we acknowledge here?
		 */
		data_vio->new_mapped.pbn = VDO_ZERO_BLOCK;
		launch_data_vio_journal_callback(data_vio, finish_block_write);
		return;
	}

	vio_allocate_data_block(data_vio_as_allocating_vio(data_vio),
				get_vdo_logical_zone_allocation_selector(data_vio->logical.zone),
				VIO_WRITE_LOCK, continue_write_after_allocation);
}

/**
 * Start the asynchronous processing of a data_vio for a write request which has
 * acquired a lock on its logical block by joining the current flush generation
 * and then attempting to allocate a physical block.
 *
 * @param data_vio  The data_vio doing the write
 **/
void launch_write_data_vio(struct data_vio *data_vio)
{
	int result;

	if (vdo_is_read_only(vdo_get_from_data_vio(data_vio)->read_only_notifier)) {
		finish_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	/* Write requests join the current flush generation. */
	result = acquire_vdo_flush_generation_lock(data_vio);
	if (abort_on_error(result, data_vio, NOT_READ_ONLY)) {
		return;
	}

	/* Go find the block map slot for the LBN mapping. */
	data_vio->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
	vdo_find_block_map_slot(data_vio,
				continue_write_with_block_map_slot,
				get_vdo_logical_zone_thread_id(data_vio->logical.zone));
}

/**
 * Clean up a data_vio which has finished processing a write.
 *
 * @param data_vio  The data_vio to clean up
 **/
void cleanup_write_data_vio(struct data_vio *data_vio)
{
	perform_cleanup_stage(data_vio, VIO_CLEANUP_START);
}
