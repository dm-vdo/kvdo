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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockAllocator.h#23 $
 */

#ifndef BLOCK_ALLOCATOR_H
#define BLOCK_ALLOCATOR_H

#include "completion.h"
#include "fixedLayout.h"
#include "statistics.h"
#include "types.h"
#include "vioPool.h"
#include "waitQueue.h"

/**
 * Create a block allocator.
 *
 * @param [in]  depot             The slab depot for this allocator
 * @param [in]  zone_number         The physical zone number for this allocator
 * @param [in]  thread_id           The thread ID for this allocator's zone
 * @param [in]  nonce               The nonce of the VDO
 * @param [in]  vio_pool_size       The size of the VIO pool
 * @param [in]  layer               The physical layer below this allocator
 * @param [in]  read_only_notifier  The context for entering read-only mode
 * @param [out] allocator_ptr       A pointer to hold the allocator
 *
 * @return A success or error code
 **/
int make_block_allocator(struct slab_depot *depot,
			 ZoneCount zone_number,
			 ThreadID thread_id,
			 Nonce nonce,
			 BlockCount vio_pool_size,
			 PhysicalLayer *layer,
			 struct read_only_notifier *read_only_notifier,
			 struct block_allocator **allocator_ptr)
	__attribute__((warn_unused_result));

/**
 * Destroy a block allocator and null out the reference to it.
 *
 * @param block_allocator_ptr  The reference to the allocator to destroy
 **/
void free_block_allocator(struct block_allocator **block_allocator_ptr);

/**
 * Queue a slab for allocation or scrubbing.
 *
 * @param slab  The slab to queue
 **/
void queue_slab(struct vdo_slab *slab);

/**
 * Update the block allocator to reflect an increment or decrement of the free
 * block count in a slab. This adjusts the allocated block count and
 * reprioritizes the slab when appropriate.
 *
 * @param slab       The slab whose free block count changed
 * @param increment  True if the free block count went up by one,
 *                   false if it went down by one
 **/
void adjust_free_block_count(struct vdo_slab *slab, bool increment);

/**
 * Allocate a physical block.
 *
 * The block allocated will have a provisional reference and the
 * reference must be either confirmed with a subsequent call to
 * incrementReferenceCount() or vacated with a subsequent call to
 * decrementReferenceCount().
 *
 * @param [in]  allocator         The block allocator
 * @param [out] block_number_ptr  A pointer to receive the allocated block number
 *
 * @return UDS_SUCCESS or an error code
 **/
int allocate_block(struct block_allocator *allocator,
		   PhysicalBlockNumber *block_number_ptr)
	__attribute__((warn_unused_result));

/**
 * Release an unused provisional reference.
 *
 * @param allocator  The block allocator
 * @param pbn        The block to dereference
 * @param why        Why the block was referenced (for logging)
 **/
void release_block_reference(struct block_allocator *allocator,
			     PhysicalBlockNumber pbn,
			     const char *why);

/**
 * Get the number of allocated blocks, which is the total number of
 * blocks in all slabs that have a non-zero reference count.
 *
 * @param allocator  The block allocator
 *
 * @return The number of blocks with a non-zero reference count
 **/
BlockCount get_allocated_blocks(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Get the number of unrecovered slabs.
 *
 * @param allocator  The block allocator
 *
 * @return The number of slabs that are unrecovered
 **/
BlockCount get_unrecovered_slab_count(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Load the state of an allocator from disk.
 *
 * <p>Implements ZoneAction.
 **/
void load_block_allocator(void *context,
			  ZoneCount zone_number,
			  struct vdo_completion *parent);

/**
 * Inform a block allocator that its slab journals have been recovered from the
 * recovery journal.
 *
 * @param allocator  The allocator to inform
 * @param result     The result of the recovery operation
 **/
void notify_slab_journals_are_recovered(struct block_allocator *allocator,
					int result);

/**
 * Prepare the block allocator to come online and start allocating blocks.
 *
 * <p>Implements ZoneAction.
 **/
void prepare_allocator_to_allocate(void *context,
				   ZoneCount zone_number,
				   struct vdo_completion *parent);

/**
 * Register a slab with the allocator, ready for use.
 *
 * @param allocator  The allocator to use
 * @param slab       The slab in question
 **/
void register_slab_with_allocator(struct block_allocator *allocator,
				  struct vdo_slab *slab);

/**
 * Register the new slabs belonging to this allocator.
 *
 * <p>Implements ZoneAction.
 **/
void register_new_slabs_for_allocator(void *context,
				      ZoneCount zone_number,
				      struct vdo_completion *parent);

/**
 * Drain all allocator I/O. Depending upon the type of drain, some or all
 * dirty metadata may be written to disk. The type of drain will be determined
 * from the state of the allocator's depot.
 *
 * <p>Implements ZoneAction.
 **/
void drain_block_allocator(void *context,
			   ZoneCount zone_number,
			   struct vdo_completion *parent);

/**
 * Resume a quiescent allocator.
 *
 * <p>Implements ZoneAction.
 **/
void resume_block_allocator(void *context,
			    ZoneCount zone_number,
			    struct vdo_completion *parent);

/**
 * Request a commit of all dirty tail blocks which are locking a given recovery
 * journal block.
 *
 * <p>Implements ZoneAction.
 **/
void release_tail_block_locks(void *context,
			      ZoneCount zone_number,
			      struct vdo_completion *parent);

/**
 * Get the slab summary zone for an allocator.
 *
 * @param allocator  The allocator
 *
 * @return The slab_summary_zone for that allocator
 **/
struct slab_summary_zone *
get_slab_summary_zone(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Acquire a VIO from a block allocator's VIO pool (asynchronous).
 *
 * @param allocator  The allocator from which to get a VIO
 * @param waiter     The object requesting the VIO
 *
 * @return VDO_SUCCESS or an error
 **/
int acquire_vio(struct block_allocator *allocator, struct waiter *waiter)
	__attribute__((warn_unused_result));

/**
 * Return a VIO to a block allocator's VIO pool
 *
 * @param allocator  The block allocator which owns the VIO
 * @param entry      The VIO being returned
 **/
void return_vio(struct block_allocator *allocator,
		struct vio_pool_entry *entry);

/**
 * Initiate scrubbing all unrecovered slabs.
 *
 * <p>Implements ZoneAction.
 **/
void scrub_all_unrecovered_slabs_in_zone(void *context,
					 ZoneCount zone_number,
					 struct vdo_completion *parent);

/**
 * Queue a waiter for a clean slab.
 *
 * @param allocator  The allocator to wait on
 * @param waiter     The waiter
 *
 * @return VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise
 **/
int enqueue_for_clean_slab(struct block_allocator *allocator,
			   struct waiter *waiter)
	__attribute__((warn_unused_result));

/**
 * Increase the scrubbing priority of a slab.
 *
 * @param slab  The slab
 **/
void increase_scrubbing_priority(struct vdo_slab *slab);

/**
 * Get the statistics for this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct block_allocator_statistics
get_block_allocator_statistics(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Get the aggregated slab journal statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct slab_journal_statistics
get_slab_journal_statistics(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Get the cumulative RefCounts statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
struct ref_counts_statistics
get_ref_counts_statistics(const struct block_allocator *allocator)
	__attribute__((warn_unused_result));

/**
 * Dump information about a block allocator to the log for debugging.
 *
 * @param allocator  The allocator to dump
 **/
void dump_block_allocator(const struct block_allocator *allocator);

#endif // BLOCK_ALLOCATOR_H
