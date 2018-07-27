/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockAllocatorInternals.h#4 $
 */

#ifndef BLOCK_ALLOCATOR_INTERNALS_H
#define BLOCK_ALLOCATOR_INTERNALS_H

#include "atomic.h"
#include "blockAllocator.h"
#include "priorityTable.h"
#include "ringNode.h"
#include "slabScrubber.h"

enum {
  /*
   * The number of VIOs in the VIO pool is proportional to the throughput of
   * the VDO.
   */
  VIO_POOL_SIZE = 128,
};

typedef enum {
  CLOSE_ALLOCATOR_START = 0,
  CLOSE_ALLOCATOR_STEP_SAVE_SLABS,
  CLOSE_ALLOCATOR_STEP_CLOSE_SLAB_SUMMARY,
  CLOSE_ALLOCATOR_VIO_POOL,
} BlockAllocatorCloseStep;

typedef struct {
  VDOCompletion  completion;
  RingNode      *ringToScrub;
} SlabRingRebuildCompletion;

/**
 * These fields are only modified by the physical zone thread, but are queried
 * by other threads.
 **/
typedef struct atomicAllocatorStatistics {
  /** The count of allocated blocks in this zone */
  Atomic64 allocatedBlocks;
  /** The number of slabs from which blocks have ever been allocated */
  Atomic64 slabsOpened;
  /** The number of times since loading that a slab been re-opened */
  Atomic64 slabsReopened;
} AtomicAllocatorStatistics;

/**
 * The statistics for all the slab journals in the slabs owned by this
 * allocator. These fields are all mutated only by the physical zone thread,
 * but are read by other threads when gathering statistics for the entire
 * depot.
 **/
typedef struct atomicSlabJournalStatistics {
  /** Number of times the on-disk journal was full */
  Atomic64 diskFullCount;
  /** Number of times an entry was added over the flush threshold */
  Atomic64 flushCount;
  /** Number of times an entry was added over the block threshold */
  Atomic64 blockedCount;
  /** Number of times the tail block was written */
  Atomic64 blocksWritten;
  /** Number of times we had to wait for the tail block commit */
  Atomic64 tailBusyCount;
} AtomicSlabJournalStatistics;

/**
 * The statistics for all the RefCounts in the slabs owned by this
 * allocator. These fields are all mutated only by the physical zone thread,
 * but are read by other threads when gathering statistics for the entire
 * depot.
 **/
typedef struct atomicRefCountStatistics {
  /** Number of blocks written */
  Atomic64 blocksWritten;
} AtomicRefCountStatistics;

struct blockAllocator {
  VDOCompletion                completion;
  /** The slab depot for this allocator */
  SlabDepot                   *depot;
  /** The slab summary zone for this allocator */
  SlabSummaryZone             *summary;
  /** The context for entering read-only mode */
  ReadOnlyModeContext         *readOnlyContext;
  /** The nonce of the VDO */
  Nonce                        nonce;
  /** The physical zone number of this allocator */
  ZoneCount                    zoneNumber;
  /** The thread ID for this allocator's physical zone */
  ThreadID                     threadID;
  /** The number of slabs in this allocator */
  SlabCount                    slabCount;
  /** The number of the last slab owned by this allocator */
  SlabCount                    lastSlab;
  /** The reduced priority level used to preserve unopened slabs */
  unsigned int                 unopenedSlabPriority;
  /** Whether a save has been requested */
  bool                         saveRequested;

  /** The slab from which blocks are currently being allocated */
  Slab                        *openSlab;
  /** A priority queue containing all slabs available for allocation */
  PriorityTable               *prioritizedSlabs;
  /** The slab scrubber */
  SlabScrubber                *slabScrubber;
  /** The completion for saving slabs */
  VDOCompletion               *slabCompletion;
  /** What phase of the close operation the allocator is to perform */
  BlockAllocatorCloseStep      closeStep;

  /** Statistics for this block allocator */
  AtomicAllocatorStatistics    statistics;
  /** Cumulative statistics for the slab journals in this zone */
  AtomicSlabJournalStatistics  slabJournalStatistics;
  /** Cumulative statistics for the RefCounts in this zone */
  AtomicRefCountStatistics     refCountStatistics;

  /**
   * This is the head of a queue of slab journals which have entries in their
   * tail blocks which have not yet started to commit. When the recovery
   * journal is under space pressure, slab journals which have uncommitted
   * entries holding a lock on the recovery journal head are forced to commit
   * their blocks early. This list is kept in order, with the tail containing
   * the slab journal holding the most recent recovery journal lock.
   **/
  RingNode                     dirtySlabJournals;

  /** The VIO pool for reading and writing block allocator metadata */
  ObjectPool                  *vioPool;
};

/**
 * Construct allocator metadata VIOs. Exposed for unit tests.
 *
 * Implements VIOConstructor
 **/
int makeAllocatorPoolVIOs(PhysicalLayer  *layer,
                          void           *parent,
                          void           *buffer,
                          VIO           **vioPtr)
  __attribute__((warn_unused_result));

/**
 * Replace the VIO pool in a block allocator. This method exists for unit
 * tests.
 *
 * @param allocator  The block allocator
 * @param size       The number of entries in the pool
 * @param layer      The physical layer from which to allocate VIOs
 *
 * @return VDO_SUCCESS or an error
 **/
int replaceVIOPool(BlockAllocator *allocator,
                   size_t          size,
                   PhysicalLayer  *layer)
  __attribute__((warn_unused_result));

/**
 * Prepare slabs for allocation or scrubbing. This method is exposed for
 * testing.
 *
 * @param allocator  The allocator to prepare
 *
 * @return VDO_SUCCESS or an error code
 **/
int prepareSlabsForAllocation(BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Start allocating from the highest numbered slab.
 *
 * @param allocator   The allocator
 **/
void allocateFromAllocatorLastSlab(BlockAllocator *allocator);

#endif // BLOCK_ALLOCATOR_INTERNALS_H
