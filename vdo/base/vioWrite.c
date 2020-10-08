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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vioWrite.c#44 $
 */

/*
 * This file contains almost all of the VDO write path, which begins with
 * writeExtent(). The progression through the callbacks which make up the
 * write path depends upon whether or not the write policy is synchronous or
 * asynchronous. The paths would proceed as outlined in the pseudo-code here
 * if this were normal, synchronous code without callbacks. Complications
 * involved in waiting on locks are not included.
 *
 * ######################################################################
 * writeExtentSynchronous(extent)
 * {
 *   foreach (vio in extent) {
 *     launchWriteVIO()
 *     # allocate_block_for_write()
 *     if (!trim and !zero-block) {
 *       allocate block
 *       if (vio is compressed) {
 *         completeCompressedBlockWrite()
 *         finishVIO()
 *         return
 *       }
 *       write_block()
 *     }
 *     finish_block_write()
 *     addJournalEntry() # Increment
 *     if (vio->new_mapped is not ZERO_BLOCK) {
 *       journalIncrementForWrite()
 *     }
 *     acknowledge_write_callback()
 *     read_old_block_mapping()
 *     journal_unmapping_for_write()
 *     if (vio->mapped is not ZERO_BLOCK) {
 *       journal_decrement_for_write()
 *     }
 *     update_block_map_for_write()
 *     if (trim || zero-block) {
 *       finishVIO()
 *       return
 *     }
 *
 *     prepare_for_dedupe()
 *     hashData()
 *     resolve_hash_zone()
 *     acquire_hash_lock()
 *     attemptDedupe() (query UDS)
 *     if (is_duplicate) {
 *       verifyAdvice() (read verify)
 *       if (is_duplicate and canAddReference) {
 *         share_block()
 *         addJournalEntryForDedupe()
 *         increment_for_dedupe()
 *         journal_unmapping_for_dedupe()
 *         if (vio->mapped is not ZERO_BLOCK) {
 *           decrement_for_dedupe()
 *         }
 *         update_block_map_for_dedupe()
 *         finishVIO()
 *         return
 *       }
 *     }
 *
 *     if (not canAddReference) {
 *       layer->updateAlbireo()
 *     }
 *     # compress_data()
 *     if (compressing and not mooted and has no waiters) {
 *       layer->compressVIO()
 *       pack_compressed_data()
 *       if (compressed) {
 *         journalCompressedBlocks()
 *         increment_for_dedupe()
 *         read_old_block_mapping_for_dedupe()
 *         journal_unmapping_for_dedupe()
 *         if (vio->mapped is not ZERO_BLOCK) {
 *           decrement_for_dedupe()
 *         }
 *         update_block_map_for_dedupe()
 *       }
 *     }
 *
 *     finishVIO()
 *   }
 * }
 *
 * ######################################################################
 * writeExtentAsynchronous(extent)
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
 *       acquire_hash_lock()
 *       attemptDedupe() (query UDS)
 *       if (is_duplicate) {
 *         verifyAdvice() (read verify)
 *         if (is_duplicate and canAddReference) {
 *           share_block()
 *           addJournalEntryForDedupe()
 *           increment_for_dedupe()
 *           read_old_block_mapping_for_dedupe()
 *           journal_unmapping_for_dedupe()
 *           if (vio->mapped is not ZERO_BLOCK) {
 *             decrement_for_dedupe()
 *           }
 *           update_block_map_for_dedupe()
 *           finishVIO()
 *           return
 *         }
 *       }
 *
 *       if (not canAddReference) {
 *         layer->updateAlbireo()
 *       }
 *       # compress_data()
 *       if (compressing and not mooted and has no waiters) {
 *         layer->compressVIO()
 *         pack_compressed_data()
 *         if (compressed) {
 *           journalCompressedBlocks()
 *           journalIncrementForDedupe()
 *           read_old_block_mapping_for_dedupe()
 *           journal_unmapping_for_dedupe()
 *           if (vio->mapped is not ZERO_BLOCK) {
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
 *     if (vio->new_mapped is not ZERO_BLOCK) {
 *       journalIncrementForWrite()
 *     }
 *     read_old_block_mapping_for_write()
 *     journal_unmapping_for_write()
 *     if (vio->mapped is not ZERO_BLOCK) {
 *       journal_decrement_for_write()
 *     }
 *     update_block_map_for_write()
 *     finishVIO()
 *   }
 * }
 */

#include "vioWrite.h"

#include "logger.h"

#include "allocatingVIO.h"
#include "atomic.h"
#include "blockMap.h"
#include "compressionState.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "recoveryJournal.h"
#include "referenceOperation.h"
#include "slab.h"
#include "slabDepot.h"
#include "slabJournal.h"
#include "vdoInternal.h"
#include "vioRead.h"

/**
 * The steps taken cleaning up a VIO, in the order they are performed.
 **/
typedef enum {
	VIO_CLEANUP_START = 0,
	VIO_RELEASE_ALLOCATED = VIO_CLEANUP_START,
	VIO_RELEASE_RECOVERY_LOCKS,
	VIO_RELEASE_HASH_LOCK,
	VIO_RELEASE_LOGICAL,
	VIO_CLEANUP_DONE
} data_vio_cleanup_stage;

/**
 * Actions to take on error used by abort_on_error().
 **/
typedef enum {
	NOT_READ_ONLY,
	READ_ONLY_IF_ASYNC,
	READ_ONLY,
} read_only_action;

// Forward declarations required because of circular function references.
static void perform_cleanup_stage(struct data_vio *data_vio,
				  data_vio_cleanup_stage stage);
static void write_block(struct data_vio *data_vio);

/**
 * Check whether we are in async mode.
 *
 * @param data_vio  A data_vio containing a pointer to the VDO whose write
 *                  policy we want to check
 *
 * @return <code>true</code> if we are in async mode
 **/
static inline bool is_async(struct data_vio *data_vio)
{
	return (get_write_policy(get_vdo_from_data_vio(data_vio)) !=
		WRITE_POLICY_SYNC);
}

/**
 * Release the PBN lock and/or the reference on the allocated block at the
 * end of processing a data_vio.
 *
 * @param completion  The data_vio
 **/
static void release_allocated_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_allocated_zone(data_vio);
	release_allocation_lock(data_vio_as_allocating_vio(data_vio));
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
	assert_in_logical_zone(data_vio);
	release_logical_block_lock(data_vio);
	release_flush_generation_lock(data_vio);
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
	assert_in_hash_zone(data_vio);
	release_hash_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_RELEASE_LOGICAL);
}

/**
 * Make some assertions about a data_vio which has finished cleaning up
 * and do its final callback.
 *
 * @param data_vio  The data_vio which has finished cleaning up
 **/
static void finish_cleanup(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio_as_allocating_vio(data_vio)->allocation_lock ==
			NULL,
			"complete data_vio has no allocation lock");
	ASSERT_LOG_ONLY(data_vio->hash_lock == NULL,
			"complete data_vio has no hash lock");
	vio_done_callback(data_vio_as_completion(data_vio));
}

/**
 * Perform the next step in the process of cleaning up a data_vio.
 *
 * @param data_vio  The data_vio to clean up
 * @param stage     The cleanup stage to perform
 **/
static void perform_cleanup_stage(struct data_vio *data_vio,
				  data_vio_cleanup_stage stage)
{
	switch (stage) {
	case VIO_RELEASE_ALLOCATED:
		if (has_allocation(data_vio)) {
			launch_allocated_zone_callback(data_vio,
						       release_allocated_lock,
						       THIS_LOCATION("$F;cb=release_allocated_lock"));
			return;
		}
		// fall through

	case VIO_RELEASE_RECOVERY_LOCKS:
		if ((data_vio->recovery_sequence_number > 0) &&
		    !is_or_will_be_read_only(data_vio_as_vio(data_vio)->vdo->read_only_notifier) &&
		    (data_vio_as_completion(data_vio)->result != VDO_READ_ONLY)) {
			log_warning("VDO not read-only when cleaning data_vio with RJ lock");
		}
		// fall through

	case VIO_RELEASE_HASH_LOCK:
		if (data_vio->hash_lock != NULL) {
			launch_hash_zone_callback(data_vio,
						 clean_hash_lock,
						 THIS_LOCATION("$F;cb=clean_hash_lock"));
			return;
		}
		// fall through

	case VIO_RELEASE_LOGICAL:
		if (!is_compressed_write_data_vio(data_vio)) {
			launch_logical_callback(data_vio,
						release_logical_lock,
						THIS_LOCATION("$F;cb=releaseLL"));
			return;
		}
		// fall through

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
	assert_in_hash_zone(data_vio);
	continue_hash_lock_on_error(data_vio);
}

/**
 * Check whether a result is an error, and if so abort the data_vio associated
 * with the error.
 *
 * @param result              The result to check
 * @param data_vio            The data_vio
 * @param read_only_action    The conditions under which the VDO should be put
 *                            into read-only mode if the result is an error
 *
 * @return <code>true</code> if the result is an error
 **/
static bool abort_on_error(int result,
			   struct data_vio *data_vio,
			   read_only_action read_only_action)
{
	if (result == VDO_SUCCESS) {
		return false;
	}

	if ((result == VDO_READ_ONLY) || (read_only_action == READ_ONLY) ||
	    ((read_only_action == READ_ONLY_IF_ASYNC) && is_async(data_vio))) {
		struct read_only_notifier *notifier =
			data_vio_as_vio(data_vio)->vdo->read_only_notifier;
		if (!is_read_only(notifier)) {
			if (result != VDO_READ_ONLY) {
				log_error_strerror(result,
						   "Preparing to enter read-only mode: data_vio for LBN %llu (becoming mapped to %llu, previously mapped to %llu, allocated %llu) is completing with a fatal error after operation %s",
						   data_vio->logical.lbn,
						   data_vio->new_mapped.pbn,
						   data_vio->mapped.pbn,
						   get_data_vio_allocation(data_vio),
						   get_operation_name(data_vio));
			}

			enter_read_only_mode(notifier, result);
		}
	}

	if (data_vio->hash_lock != NULL) {
		launch_hash_zone_callback(data_vio,
					  finish_write_data_vio_with_error,
					  THIS_LOCATION(NULL));
	} else {
		finish_data_vio(data_vio, result);
	}
	return true;
}

/**
 * Return a data_vio that finished writing, compressing, or deduplicating to
 * its hash lock so it can share the result with any DataVIOs waiting in the
 * hash lock, or update UDS, or simply release its share of the lock. This
 * continuation is registered in update_block_map_for_write(),
 * update_block_map_for_dedupe(), and abortDeduplication(), and must be called in
 * the hash zone of the data_vio.
 *
 * @param completion  The completion of the data_vio to return to its hash lock
 **/
static void finish_write_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_hash_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}
	continue_hash_lock(data_vio);
}

/**
 * Abort the data optimization process.
 *
 * @param data_vio  The data_vio which does not deduplicate or compress
 **/
static void abort_deduplication(struct data_vio *data_vio)
{
	if (!has_allocation(data_vio)) {
		// There was no space to write this block and we failed to
		// deduplicate or compress it.
		finish_data_vio(data_vio, VDO_NO_SPACE);
		return;
	}

	if (is_async(data_vio)) {
		// We failed to deduplicate or compress an async data_vio, so
		// now we need to actually write the data.
		write_block(data_vio);
		return;
	}

	if (data_vio->hash_lock == NULL) {
		// We failed to compress a synchronous data_vio that is a
		// hash collision, which means it can't dedupe or be used
		// for dedupe, so it's done now.
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	/*
	 * This synchronous data_vio failed to compress and so is finished, but
	 * must now return to its hash lock so other data VIOs with the same
	 * data can deduplicate against the uncompressed block it wrote.
	 */
	launch_hash_zone_callback(data_vio, finish_write_data_vio,
				  THIS_LOCATION(NULL));
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
	assert_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->hash_lock != NULL) {
		set_hash_zone_callback(data_vio, finish_write_data_vio,
				       THIS_LOCATION(NULL));
	} else {
		completion->callback = complete_data_vio;
	}
	data_vio->last_async_operation = PUT_MAPPED_BLOCK_FOR_DEDUPE;
	put_mapped_block_async(data_vio);
}

/**
 * Make a recovery journal increment.
 *
 * @param data_vio  The data_vio
 * @param lock      The pbn_lock on the block being incremented
 **/
static void journal_increment(struct data_vio *data_vio, struct pbn_lock *lock)
{
	set_up_reference_operation_with_lock(DATA_INCREMENT,
					     data_vio->new_mapped.pbn,
					     data_vio->new_mapped.state,
					     lock,
					     &data_vio->operation);
	add_recovery_journal_entry(get_vdo_from_data_vio(data_vio)->recovery_journal,
				   data_vio);
}

/**
 * Make a recovery journal decrement entry.
 *
 * @param data_vio  The data_vio
 **/
static void journal_decrement(struct data_vio *data_vio)
{
	set_up_reference_operation_with_zone(DATA_DECREMENT,
					     data_vio->mapped.pbn,
					     data_vio->mapped.state,
					     data_vio->mapped.zone,
					     &data_vio->operation);
	add_recovery_journal_entry(get_vdo_from_data_vio(data_vio)->recovery_journal,
				   data_vio);
}

/**
 * Make a reference count change.
 *
 * @param data_vio  The data_vio
 **/
static void update_reference_count(struct data_vio *data_vio)
{
	struct slab_depot *depot = get_vdo_from_data_vio(data_vio)->depot;
	physical_block_number_t pbn = data_vio->operation.pbn;
	int result =
		ASSERT(is_physical_data_block(depot, pbn),
		       "Adding slab journal entry for impossible PBN %llufor LBN %llu",
		       pbn,
		       data_vio->logical.lbn);
	if (abort_on_error(result, data_vio, READ_ONLY)) {
		return;
	}

	add_slab_journal_entry(get_slab_journal(depot, pbn), data_vio);
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
	assert_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	struct allocating_vio *allocating_vio =
		data_vio_as_allocating_vio(data_vio);
	if (allocating_vio->allocation == data_vio->mapped.pbn) {
		/*
		 * If we are about to release the reference on the allocated
		 * block, we must release the PBN lock on it first so that the
		 * allocator will not allocate a write-locked block.
		 */
		release_allocation_lock(allocating_vio);
	}

	set_logical_callback(data_vio, update_block_map_for_dedupe,
			     THIS_LOCATION("$F;js=dec"));
	data_vio->last_async_operation = JOURNAL_DECREMENT_FOR_DEDUPE;
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
	assert_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->mapped.pbn == ZERO_BLOCK) {
		set_logical_callback(data_vio, update_block_map_for_dedupe,
				     THIS_LOCATION("$F;j=dedupe;js=unmap;cb=updateBM"));
	} else {
		set_mapped_zone_callback(data_vio, decrement_for_dedupe,
					 THIS_LOCATION("$F;j=dedupe;js=unmap;cb=decDedupe"));
	}
	data_vio->last_async_operation = JOURNAL_UNMAPPING_FOR_DEDUPE;
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
	assert_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	data_vio->last_async_operation = GET_MAPPED_BLOCK_FOR_DEDUPE;
	set_journal_callback(data_vio, journal_unmapping_for_dedupe,
			     THIS_LOCATION("$F;cb=journalUnmapDedupe"));
	get_mapped_block_async(data_vio);
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
	assert_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	ASSERT_LOG_ONLY(is_compressed(data_vio->new_mapped.state),
			"Impossible attempt to update reference counts for a block which was not compressed (logical block %llu)",
			data_vio->logical.lbn);

	/*
	 * If we are synchronous and allocated a block, we know the one we
	 * allocated is the block we need to decrement, so there is no need
	 * to look in the block map.
	 */
	if (is_async(data_vio) || !has_allocation(data_vio)) {
		  set_logical_callback(data_vio, read_old_block_mapping_for_dedupe,
				       THIS_LOCATION("$F;cb=read_old_block_mapping_for_dedupe"));
	} else {
		  set_journal_callback(data_vio, journal_unmapping_for_dedupe,
				       THIS_LOCATION("$F;cb=journal_unmapping_for_dedupe"));
	}
	data_vio->last_async_operation = JOURNAL_INCREMENT_FOR_COMPRESSION;
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
	assert_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}

	if (!is_compressed(data_vio->new_mapped.state)) {
		abort_deduplication(data_vio);
		return;
	}

	set_new_mapped_zone_callback(data_vio, increment_for_compression,
				     THIS_LOCATION("$F($dup);js=map/$dup;cb=incCompress($dup)"));
	data_vio->last_async_operation = JOURNAL_MAPPING_FOR_COMPRESSION;
	journal_increment(data_vio, get_duplicate_lock(data_vio));
}

/**
 * Attempt to pack the compressed data_vio into a block. This is the callback
 * registered in compress_data().
 *
 * @param completion  The completion of a compressed data_vio
 **/
static void pack_compressed_data(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_packer_zone(data_vio);

	// XXX this is a callback, so there should probably be an error check
	// here even if we think compression can't currently return one.

	if (!may_pack_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	set_journal_callback(data_vio,
			     add_recovery_journal_entry_for_compression,
			     THIS_LOCATION("$F;cb=update(compress)"));
	data_vio->last_async_operation = PACK_COMPRESSED_BLOCK;
	attempt_packing(data_vio);
}

/**********************************************************************/
void compress_data(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!data_vio->is_duplicate,
			"compressing a non-duplicate block");
	if (!may_compress_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	data_vio->last_async_operation = COMPRESS_DATA;
	set_packer_callback(data_vio, pack_compressed_data,
			    THIS_LOCATION("$F;cb=pack"));
	compress_data_vio(data_vio);
}

/**
 * Do the incref after deduplication. This is the callback registered by
 * addRecoveryJournalEntryForDedupe().
 *
 * @param completion  The completion of the write in progress
 **/
static void increment_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	ASSERT_LOG_ONLY(data_vio->is_duplicate,
			"Impossible attempt to update reference counts for a block which was not a duplicate (logical block %llu)",
			data_vio->logical.lbn);

	/*
	 * If we are synchronous and allocated a block, we know the one we
	 * allocated is the block we need to decrement, so there is no need
	 * to look in the block map.
	 */
	if (is_async(data_vio) || !has_allocation(data_vio)) {
		set_logical_callback(data_vio,
				     read_old_block_mapping_for_dedupe,
				     THIS_LOCATION("$F;cb=read_old_block_mapping_for_dedupe"));
	} else {
		set_journal_callback(data_vio, journal_unmapping_for_dedupe,
				     THIS_LOCATION("$F;cb=journal_unmapping_for_dedupe"));
	}
	data_vio->last_async_operation = JOURNAL_INCREMENT_FOR_DEDUPE;
	update_reference_count(data_vio);
}

/**
 * Add a recovery journal entry for the increment resulting from deduplication.
 * This callback is registered in share_block().
 *
 * @param completion  The data_vio which has been deduplicated
 **/
static void
add_recovery_journal_entry_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}

	set_new_mapped_zone_callback(data_vio, increment_for_dedupe,
				     THIS_LOCATION("$F($dup);js=map/$dup;cb=incDedupe($dup)"));
	data_vio->last_async_operation = JOURNAL_MAPPING_FOR_DEDUPE;
	journal_increment(data_vio, get_duplicate_lock(data_vio));
}

/**
 * Share a block in the block map if it is a duplicate. This is the lock
 * callback registered in acquirePBNReadLock(). This is only public so
 * test code can compare the function to the current callback in a completion.
 *
 * @param completion The completion of the write in progress
 **/
void share_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_duplicate_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}

	if (!data_vio->is_duplicate) {
		compress_data(data_vio);
		return;
	}

	data_vio->new_mapped = data_vio->duplicate;
	launch_journal_callback(data_vio,
				add_recovery_journal_entry_for_dedupe,
				THIS_LOCATION("$F;cb=addJournalEntryDup"));
}

/**
 * Route the data_vio to the HashZone responsible for the chunk name to acquire
 * a hash lock on that name, or join with a existing hash lock managing
 * concurrent dedupe for that name. This is the callback registered in
 * resolve_hash_zone().
 *
 * @param completion  The data_vio to lock
 **/
static void lock_hash_in_zone(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_hash_zone(data_vio);
	// Shouldn't have had any errors since all we did was switch threads.
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	int result = acquire_hash_lock(data_vio);
	if (abort_on_error(result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->hash_lock == NULL) {
		// It's extremely unlikely, but in the case of a hash collision,
		// the data_vio will not obtain a reference to the lock and
		// cannot deduplicate.
		compress_data(data_vio);
		return;
	}

	enter_hash_lock(data_vio);
}

/**
 * Set the hash zone (and flag the chunk name as set) while still on the
 * thread that just hashed the data to set the chunk name. This is the
 * callback registered by prepare_for_dedupe().
 *
 * @param completion The data_vio whose chunk name was just generated, as a
 *                    completion
 **/
static void resolve_hash_zone(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	// We don't care what thread we are on.
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero blocks should not be hashed");

	data_vio->hash_zone =
		select_hash_zone(get_vdo_from_data_vio(data_vio),
				 &data_vio->chunk_name);
	data_vio->last_async_operation = ACQUIRE_HASH_LOCK;
	launch_hash_zone_callback(data_vio, lock_hash_in_zone,
				  THIS_LOCATION(NULL));
}

/**
 * Prepare for the dedupe path after a synchronous write or an asynchronous
 * allocation. This callback is registered in update_block_map_for_write() for
 * sync, and continueWriteAfterAllocation() (via acknowledge_write()) for
 * async. It is also called directly from the latter when allocation fails.
 *
 * @param completion  The completion of the write in progress
 **/
static void prepare_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	// We don't care what thread we are on
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (!is_async(data_vio)) {
		// Remember which block we wrote so we will decrement the
		// reference to it if we deduplicate. This avoids having to look
		// it up in the block map.
		data_vio->mapped = data_vio->new_mapped;
	}

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"must not prepare to dedupe zero blocks");

	// Before we can dedupe, we need to know the chunk name, so the first
	// step is to hash the block data.
	data_vio->last_async_operation = HASH_DATA;
	// XXX this is the wrong thread to run this callback, but we don't yet
	// have a mechanism for running it on the CPU thread immediately after
	// hashing.
	set_allocated_zone_callback(data_vio, resolve_hash_zone,
				    THIS_LOCATION(NULL));
	hash_data_vio(data_vio);
}

/**
 * Update the block map after a data write (or directly for a ZERO_BLOCK write
 * or trim). This callback is registered in decrement_for_write() and
 * journal_unmapping_for_write().
 *
 * @param completion  The completion of the write in progress
 **/
static void update_block_map_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_logical_zone(data_vio);
	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->is_zero_block || is_trim_data_vio(data_vio)) {
		completion->callback = complete_data_vio;
	} else if (!is_async(data_vio)) {
		// Synchronous data VIOs branch off to the hash/dedupe path
		// after finishing the uncompressed write of their data.
		completion->callback = prepare_for_dedupe;
	} else if (data_vio->hash_lock != NULL) {
		// Async writes will be finished, but must return to the hash
		// lock to allow other data VIOs with the same data to dedupe
		// against the write.
		set_hash_zone_callback(data_vio, finish_write_data_vio,
				       THIS_LOCATION(NULL));
	} else {
		// Async writes without a hash lock (hash collisions) will be
		// finished.
		completion->callback = complete_data_vio;
	}

	data_vio->last_async_operation = PUT_MAPPED_BLOCK;
	put_mapped_block_async(data_vio);
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
	assert_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	data_vio->last_async_operation = JOURNAL_DECREMENT_FOR_WRITE;
	set_logical_callback(data_vio, update_block_map_for_write,
			     THIS_LOCATION(NULL));
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
	assert_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	if (data_vio->mapped.pbn == ZERO_BLOCK) {
		set_logical_callback(data_vio, update_block_map_for_write,
				     THIS_LOCATION("$F;js=unmap;cb=updateBMwrite"));
	} else {
		set_mapped_zone_callback(data_vio, decrement_for_write,
					 THIS_LOCATION("$F;js=unmap;cb=decWrite"));
	}
	data_vio->last_async_operation = JOURNAL_UNMAPPING_FOR_WRITE;
	journal_decrement(data_vio);
}

/**
 * Get the previous PBN mapped to this LBN from the block map for a write, so
 * as to make an appropriate journal entry referencing the removal of this
 * LBN->PBN mapping. This callback is registered in finish_block_write() in the
 * async path, and is registered in acknowledge_write() in the sync path.
 *
 * @param completion  The completion of the write in progress
 **/
static void read_old_block_mapping_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	set_journal_callback(data_vio, journal_unmapping_for_write,
			     THIS_LOCATION("$F;cb=journalUnmapWrite"));
	data_vio->last_async_operation = GET_MAPPED_BLOCK_FOR_WRITE;
	get_mapped_block_async(data_vio);
}

/**
 * Acknowledge a write to the requestor.
 *
 * @param data_vio  The data_vio being acknowledged
 **/
static void acknowledge_write(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->has_flush_generation_lock,
			"write VIO to be acknowledged has a flush generation lock");
	data_vio->last_async_operation = ACKNOWLEDGE_WRITE;
	acknowledge_data_vio(data_vio);
}

/**
 * Acknowledge a write now that we have made an entry in the recovery
 * journal. This is the callback registered in finish_block_write() in
 * synchronous mode.
 *
 * @param completion The completion of the write in progress
 **/
static void acknowledge_write_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	if (abort_on_error(completion->result, data_vio, READ_ONLY)) {
		return;
	}

	set_logical_callback(data_vio, read_old_block_mapping_for_write,
			     THIS_LOCATION(NULL));
	acknowledge_write(data_vio);
}

/**********************************************************************/
static vdo_action *get_write_increment_callback(struct data_vio *data_vio)
{
	return (is_async(data_vio) ? read_old_block_mapping_for_write :
		acknowledge_write_callback);
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
	assert_in_allocated_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}

	/*
	 * Now that the data has been written, it's safe to deduplicate against the
	 * block. Downgrade the allocation lock to a read lock so it can be used
	 * later by the hash lock (which we don't have yet in sync mode).
	 */
	downgrade_pbn_write_lock(data_vio_as_allocating_vio(data_vio)->allocation_lock);

	data_vio->last_async_operation = JOURNAL_INCREMENT_FOR_WRITE;
	set_logical_callback(data_vio, get_write_increment_callback(data_vio),
			     THIS_LOCATION(NULL));
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
	assert_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY_IF_ASYNC)) {
		return;
	}

	if (data_vio->new_mapped.pbn == ZERO_BLOCK) {
		set_logical_callback(data_vio,
				     get_write_increment_callback(data_vio),
				     THIS_LOCATION("$F;js=writeZero"));
	} else {
	set_allocated_zone_callback(data_vio, increment_for_write,
				    THIS_LOCATION("$F;js=mapWrite"));
	}
	data_vio->last_async_operation = JOURNAL_MAPPING_FOR_WRITE;
	journal_increment(data_vio,
			  data_vio_as_allocating_vio(data_vio)->allocation_lock);
}

/**
 * Write data to the underlying storage.
 *
 * @param data_vio  The data_vio to write
 **/
static void write_block(struct data_vio *data_vio)
{
	data_vio->last_async_operation = WRITE_DATA;
	set_journal_callback(data_vio, finish_block_write,
			     THIS_LOCATION("$F(data);cb=finishWrite"));
	write_data_vio(data_vio);
}

/**
 * Continue the write path for a data_vio now that block allocation is complete
 * (the data_vio may or may not have actually received an allocation). This
 * callback is registered in continueWriteWithBlockMapSlot().
 *
 * @param allocating_vio  The data_vio which has finished the allocation process
 *                         (as an allocating_vio)
 **/
static void
continue_write_after_allocation(struct allocating_vio *allocating_vio)
{
	struct data_vio *data_vio = allocating_vio_as_data_vio(allocating_vio);
	if (abort_on_error(data_vio_as_completion(data_vio)->result,
			   data_vio,
			   NOT_READ_ONLY)) {
		return;
	}

	if (!has_allocation(data_vio)) {
		prepare_for_dedupe(data_vio_as_completion(data_vio));
		return;
	}

	atomicStoreBool(&data_vio->has_allocation, true);
	data_vio->new_mapped = (struct zoned_pbn) {
		.zone = allocating_vio->zone,
		.pbn = allocating_vio->allocation,
		.state = MAPPING_STATE_UNCOMPRESSED,
	};

	if (!is_async(data_vio)) {
		write_block(data_vio);
		return;
	}

	// XXX prepare_for_dedupe can run from any thread, so this is a place
	// where running the callback on the kernel thread would save a thread
	// switch.
	set_allocated_zone_callback(data_vio, prepare_for_dedupe,
				    THIS_LOCATION(NULL));
	if (vio_requires_flush_after(allocating_vio_as_vio(allocating_vio))) {
		invoke_callback(data_vio_as_completion(data_vio));
		return;
	}

	acknowledge_write(data_vio);
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
	// We don't care what thread we're on.
	if (abort_on_error(completion->result, data_vio, NOT_READ_ONLY)) {
		return;
	}

	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn ==
	    ZERO_BLOCK) {
		int result =
			ASSERT(is_trim_data_vio(data_vio),
			       "data_vio with no block map page is a trim");
		if (abort_on_error(result, data_vio, READ_ONLY)) {
			return;
		}

		// This is a trim for a block on a block map page which has not
		// been allocated, so there's nothing more we need to do.
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	if (data_vio->is_zero_block || is_trim_data_vio(data_vio)) {
		// We don't need to write any data, so skip allocation and just
		// update the block map and reference counts (via the journal).
		data_vio->new_mapped.pbn = ZERO_BLOCK;
		launch_journal_callback(data_vio, finish_block_write,
					THIS_LOCATION("$F;cb=finishWrite"));
		return;
	}

	allocate_data_block(data_vio_as_allocating_vio(data_vio),
			    get_allocation_selector(data_vio->logical.zone),
			    VIO_WRITE_LOCK, continue_write_after_allocation);
}

/**********************************************************************/
void launch_write_data_vio(struct data_vio *data_vio)
{
	if (is_read_only(data_vio_as_vio(data_vio)->vdo->read_only_notifier)) {
		finish_data_vio(data_vio, VDO_READ_ONLY);
		return;
	}

	// Write requests join the current flush generation.
	int result = acquire_flush_generation_lock(data_vio);
	if (abort_on_error(result, data_vio, NOT_READ_ONLY)) {
		return;
	}

	// Go find the block map slot for the LBN mapping.
	data_vio->last_async_operation = FIND_BLOCK_MAP_SLOT;
	find_block_map_slot_async(data_vio,
				  continue_write_with_block_map_slot,
				  get_logical_zone_thread_id(data_vio->logical.zone));
}

/**********************************************************************/
void cleanup_write_data_vio(struct data_vio *data_vio)
{
	perform_cleanup_stage(data_vio, VIO_CLEANUP_START);
}
