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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockAllocator.h#5 $
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
 * @param [in]  zoneNumber        The physical zone number for this allocator
 * @param [in]  threadID          The thread ID for this allocator's zone
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  vioPoolSize       The size of the VIO pool
 * @param [in]  layer             The physical layer below this allocator
 * @param [in]  readOnlyNotifier  The context for entering read-only mode
 * @param [out] allocatorPtr      A pointer to hold the allocator
 *
 * @return A success or error code
 **/
int makeBlockAllocator(SlabDepot         *depot,
                       ZoneCount          zoneNumber,
                       ThreadID           threadID,
                       Nonce              nonce,
                       BlockCount         vioPoolSize,
                       PhysicalLayer     *layer,
                       ReadOnlyNotifier  *readOnlyNotifier,
                       BlockAllocator   **allocatorPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a block allocator and null out the reference to it.
 *
 * @param blockAllocatorPtr  The reference to the allocator to destroy
 **/
void freeBlockAllocator(BlockAllocator **blockAllocatorPtr);

/**
 * Queue a slab for allocation. Can be called only once per slab.
 *
 * @param slab  The slab to queue
 **/
void queueSlab(Slab *slab);

/**
 * Update the block allocator to reflect an increment or decrement of the free
 * block count in a slab. This adjusts the allocated block count and
 * reprioritizes the slab when appropriate.
 *
 * @param slab       The slab whose free block count changed
 * @param increment  True if the free block count went up by one,
 *                   false if it went down by one
 **/
void adjustFreeBlockCount(Slab *slab, bool increment);

/**
 * Allocate a physical block.
 *
 * The block allocated will have a provisional reference and the
 * reference must be either confirmed with a subsequent call to
 * incrementReferenceCount() or vacated with a subsequent call to
 * decrementReferenceCount().
 *
 * @param [in]  allocator       The block allocator
 * @param [out] blockNumberPtr  A pointer to receive the allocated block number
 *
 * @return UDS_SUCCESS or an error code
 **/
int allocateBlock(BlockAllocator      *allocator,
                  PhysicalBlockNumber *blockNumberPtr)
  __attribute__((warn_unused_result));

/**
 * Release an unused provisional reference.
 *
 * @param allocator  The block allocator
 * @param pbn        The block to dereference
 * @param why        Why the block was referenced (for logging)
 **/
void releaseBlockReference(BlockAllocator      *allocator,
                           PhysicalBlockNumber  pbn,
                           const char          *why);

/**
 * Get the number of allocated blocks, which is the total number of
 * blocks in all slabs that have a non-zero reference count.
 *
 * @param allocator  The block allocator
 *
 * @return The number of blocks with a non-zero reference count
 **/
BlockCount getAllocatedBlocks(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Get the number of unrecovered slabs.
 *
 * @param allocator  The block allocator
 *
 * @return The number of slabs that are unrecovered
 **/
BlockCount getUnrecoveredSlabCount(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Prepare the block allocator to come online and start allocating blocks.
 *
 * <p>Implements ZoneAction.
 **/
void prepareAllocatorToAllocate(void          *context,
                                ZoneCount      zoneNumber,
                                VDOCompletion *parent);

/**
 * Register a slab with the allocator, ready for use.
 *
 * @param allocator  The allocator to use
 * @param slab       The slab in question
 * @param resizing   Whether the registration is part of a resize
 **/
void registerSlabWithAllocator(BlockAllocator *allocator,
                               Slab           *slab,
                               bool            resizing);

/**
 * Register the new slabs belonging to this allocator.
 *
 * <p>Implements ZoneAction.
 **/
void registerNewSlabsForAllocator(void          *context,
                                  ZoneCount      zoneNumber,
                                  VDOCompletion *parent);

/**
 * Asynchronously flush all slab journals in the allocator.
 *
 * <p>Implements ZoneAction.
 **/
void flushAllocatorSlabJournals(void          *context,
                                ZoneCount      zoneNumber,
                                VDOCompletion *parent);

/**
 * Suspend the summary zone belonging to a block allocator.
 *
 * <p>Implements ZoneAction.
 **/
void suspendSummaryZone(void          *context,
                        ZoneCount      zoneNumber,
                        VDOCompletion *parent);

/**
 * Resume the summary zone belonging to a block allocator.
 *
 * <p>Implements ZoneAction.
 **/
void resumeSummaryZone(void          *context,
                       ZoneCount      zoneNumber,
                       VDOCompletion *parent);

/**
 * Asynchronously save any block allocator state that isn't included in the
 * SuperBlock component to the allocator partition.
 *
 * <p>Implements ZoneAction.
 **/
void closeBlockAllocator(void          *context,
                         ZoneCount      zoneNumber,
                         VDOCompletion *parent);

/**
 * Asynchronously save any block allocator state for a full rebuild.
 *
 * <p>Implements ZoneAction.
 **/
void saveBlockAllocatorForFullRebuild(void          *context,
                                      ZoneCount      zoneNumber,
                                      VDOCompletion *parent);

/**
 * Save a slab which has been rebuilt.
 *
 * @param slab          The slab which has been rebuilt
 * @param parent        The parent to notify when the slab has been saved
 * @param callback      The function to call when the slab has been saved
 * @param errorHandler  The handler for save errors
 **/
void saveRebuiltSlab(Slab          *slab,
                     VDOCompletion *parent,
                     VDOAction     *callback,
                     VDOAction     *errorHandler);

/**
 * Request a commit of all dirty tail blocks which are locking a given recovery
 * journal block.
 *
 * <p>Implements ZoneAction.
 **/
void releaseTailBlockLocks(void          *context,
                           ZoneCount      zoneNumber,
                           VDOCompletion *parent);

/**
 * Get the slab summary zone for an allocator.
 *
 * @param allocator  The allocator
 *
 * @return The SlabSummaryZone for that allocator
 **/
SlabSummaryZone *getSlabSummaryZone(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Acquire a VIO from a block allocator's VIO pool (asynchronous).
 *
 * @param allocator  The allocator from which to get a VIO
 * @param waiter     The object requesting the VIO
 *
 * @return VDO_SUCCESS or an error
 **/
int acquireVIO(BlockAllocator *allocator, Waiter *waiter)
  __attribute__((warn_unused_result));

/**
 * Return a VIO to a block allocator's VIO pool
 *
 * @param allocator  The block allocator which owns the VIO
 * @param entry      The VIO being returned
 **/
void returnVIO(BlockAllocator *allocator, VIOPoolEntry *entry);

/**
 * Initiate scrubbing all unrecovered slabs.
 *
 * <p>Implements ZoneAction.
 **/
void scrubAllUnrecoveredSlabsInZone(void          *context,
                                    ZoneCount      zoneNumber,
                                    VDOCompletion *parent);

/**
 * Queue a waiter for a clean slab.
 *
 * @param allocator  The allocator to wait on
 * @param waiter     The waiter
 *
 * @return VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no
 *         slabs to scrub, and some other error otherwise
 **/
int enqueueForCleanSlab(BlockAllocator *allocator, Waiter *waiter)
  __attribute__((warn_unused_result));

/**
 * Increase the scrubbing priority of a slab.
 *
 * @param slab  The slab
 **/
void increaseScrubbingPriority(Slab *slab);

/**
 * Get the statistics for this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
BlockAllocatorStatistics
getBlockAllocatorStatistics(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Get the aggregated slab journal statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
SlabJournalStatistics getSlabJournalStatistics(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Get the cumulative RefCounts statistics for the slabs in this allocator.
 *
 * @param allocator  The allocator to query
 *
 * @return A copy of the current statistics for the allocator
 **/
RefCountsStatistics getRefCountsStatistics(const BlockAllocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Dump information about a block allocator to the log for debugging.
 *
 * @param allocator  The allocator to dump
 **/
void dumpBlockAllocator(const BlockAllocator *allocator);

#endif // BLOCK_ALLOCATOR_H
