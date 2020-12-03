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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockAllocatorInternals.h#29 $
 */

#ifndef BLOCK_ALLOCATOR_INTERNALS_H
#define BLOCK_ALLOCATOR_INTERNALS_H

#include "adminState.h"
#include "blockAllocator.h"
#include "priorityTable.h"
#include "slabScrubber.h"
#include "statistics.h"
#include "vioPool.h"

enum {
	/*
	 * The number of vios in the vio pool is proportional to the throughput
	 * of the VDO.
	 */
	VIO_POOL_SIZE = 128,
};

typedef enum {
	DRAIN_ALLOCATOR_START = 0,
	DRAIN_ALLOCATOR_STEP_SCRUBBER,
	DRAIN_ALLOCATOR_STEP_SLABS,
	DRAIN_ALLOCATOR_STEP_SUMMARY,
	DRAIN_ALLOCATOR_STEP_FINISHED,
} BlockAllocatorDrainStep;

/**
 * A sub-structure for applying actions in parallel to all an allocator's
 * slabs.
 **/
struct slab_actor {
	/** The number of slabs performing a slab action */
	slab_count_t slab_action_count;
	/** The method to call when a slab action has been completed by all
	 * slabs */
	vdo_action *callback;
};

struct block_allocator {
	struct vdo_completion completion;
	/** The slab depot for this allocator */
	struct slab_depot *depot;
	/** The slab summary zone for this allocator */
	struct slab_summary_zone *summary;
	/** The notifier for entering read-only mode */
	struct read_only_notifier *read_only_notifier;
	/** The nonce of the VDO */
	nonce_t nonce;
	/** The physical zone number of this allocator */
	zone_count_t zone_number;
	/** The thread ID for this allocator's physical zone */
	thread_id_t thread_id;
	/** The number of slabs in this allocator */
	slab_count_t slab_count;
	/** The number of the last slab owned by this allocator */
	slab_count_t last_slab;
	/** The reduced priority level used to preserve unopened slabs */
	unsigned int unopened_slab_priority;
	/** The state of this allocator */
	struct admin_state state;
	/** The actor for applying an action to all slabs */
	struct slab_actor slab_actor;

	/** The slab from which blocks are currently being allocated */
	struct vdo_slab *open_slab;
	/** A priority queue containing all slabs available for allocation */
	struct priority_table *prioritized_slabs;
	/** The slab scrubber */
	struct slab_scrubber *slab_scrubber;
	/** What phase of the close operation the allocator is to perform */
	BlockAllocatorDrainStep drain_step;

	/*
	 * These statistics are all mutated only by the physical zone thread,
	 * but are read by other threads when gathering statistics for the
	 * entire depot.
	 */
	/**
	 * The count of allocated blocks in this zone. Not in
	 * block_allocator_statistics for historical reasons.
	 **/
	uint64_t allocated_blocks;
	/** Statistics for this block allocator */
	struct block_allocator_statistics statistics;
	/** Cumulative statistics for the slab journals in this zone */
	struct slab_journal_statistics slab_journal_statistics;
	/** Cumulative statistics for the RefCounts in this zone */
	struct ref_counts_statistics ref_counts_statistics;

	/**
	 * This is the head of a queue of slab journals which have entries in
	 * their tail blocks which have not yet started to commit. When the
	 * recovery journal is under space pressure, slab journals which have
	 * uncommitted entries holding a lock on the recovery journal head are
	 * forced to commit their blocks early. This list is kept in order, with
	 * the tail containing the slab journal holding the most recent recovery
	 * journal lock.
	 **/
	struct list_head dirty_slab_journals;

	/** The vio pool for reading and writing block allocator metadata */
	struct vio_pool *vio_pool;
};

/**
 * Construct allocator metadata vios. Exposed for unit tests.
 *
 * Implements VIOConstructor
 **/
int __must_check
make_allocator_pool_vios(PhysicalLayer *layer,
			 void *parent,
			 void *buffer,
			 struct vio **vio_ptr);

/**
 * Replace the vio pool in a block allocator. This method exists for unit
 * tests.
 *
 * @param allocator  The block allocator
 * @param size       The number of entries in the pool
 * @param layer      The physical layer from which to allocate vios
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check replace_vio_pool(struct block_allocator *allocator,
				  size_t size,
				  PhysicalLayer *layer);

/**
 * Prepare slabs for allocation or scrubbing. This method is exposed for
 * testing.
 *
 * @param allocator  The allocator to prepare
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
prepare_slabs_for_allocation(struct block_allocator *allocator);

/**
 * Start allocating from the highest numbered slab.
 *
 * @param allocator   The allocator
 **/
void allocate_from_allocator_last_slab(struct block_allocator *allocator);

#endif // BLOCK_ALLOCATOR_INTERNALS_H
