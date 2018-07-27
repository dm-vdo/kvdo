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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockAllocator.h#2 $
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
 * @param [in]  depot               The slab depot for this allocator
 * @param [in]  zoneNumber          The physical zone number for this allocator
 * @param [in]  threadID            The thread ID for this allocator's zone
 * @param [in]  nonce               The nonce of the VDO
 * @param [in]  vioPoolSize         The size of the VIO pool
 * @param [in]  layer               The physical layer below this allocator
 * @param [in]  readOnlyContext     The context for entering read-only mode
 * @param [out] allocatorPtr        A pointer to hold the allocator
 *
 * @return A success or error code
 **/
int makeBlockAllocator(SlabDepot            *depot,
                       ZoneCount             zoneNumber,
                       ThreadID              threadID,
                       Nonce                 nonce,
                       BlockCount            vioPoolSize,
                       PhysicalLayer        *layer,
                       ReadOnlyModeContext  *readOnlyContext,
                       BlockAllocator      **allocatorPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a block allocator and null out the reference to it.
 *
 * @param blockAllocatorPtr  The reference to the allocator to destroy
 **/
void freeBlockAllocator(BlockAllocator **blockAllocatorPtr);

/**
 * Inform a block allocator that the VDO has entered read-only mode.
 *
 * @param allocator  The block allocator
 **/
void notifyBlockAllocatorOfReadOnlyMode(BlockAllocator *allocator);

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
 * Implements AllocatorAction.
 *
 * @param allocator     The allocator
 * @param parent        The completion to notify when the allocator is prepared
 * @param callback      The callback to run when the preparations are complete
 * @param errorHandler  The handler for preparation errors
 **/
void prepareAllocatorToAllocate(BlockAllocator *allocator,
                                VDOCompletion  *parent,
                                VDOAction      *callback,
                                VDOAction      *errorHandler);

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
 * Implements AllocatorAction.
 *
 * @param allocator     The allocator
 * @param parent        The completion to notify when done
 * @param callback      The callback to run when the registrations are complete
 * @param errorHandler  The handler for errors, of which there should be none
 **/
void registerNewSlabsForAllocator(BlockAllocator *allocator,
                                  VDOCompletion  *parent,
                                  VDOAction      *callback,
                                  VDOAction      *errorHandler);

/**
 * Asynchronously flush all slab journals in the allocator.
 * Implements AllocatorAction.
 *
 * @param allocator     The allocator whose slab journals need flushing
 * @param parent        The parent completion
 * @param callback      The callback to run when the flush is complete
 * @param errorHandler  The handler for flush errors
 **/
void flushAllocatorSlabJournals(BlockAllocator *allocator,
                                VDOCompletion  *parent,
                                VDOAction       callback,
                                VDOAction      *errorHandler);

/**
 * Suspend the summary zone belonging to a block allocator. Implements
 * AllocatorAction.
 *
 * @param allocator     The allocator owning the summary zone
 * @param parent        The object to notify when the suspend is complete
 * @param callback      The function to call when the suspend is complete
 * @param errorHandler  The handler for suspend errors
 **/
void suspendSummaryZone(BlockAllocator *allocator,
                        VDOCompletion  *parent,
                        VDOAction      *callback,
                        VDOAction      *errorHandler);

/**
 * Resume the summary zone belonging to a block allocator. Implements
 * AllocatorAction.
 *
 * @param allocator     The allocator owning the summary zone
 * @param parent        The object to notify when the resume is complete
 * @param callback      The function to call when the resume is complete
 * @param errorHandler  The handler for resume errors
 **/
void resumeSummaryZone(BlockAllocator *allocator,
                       VDOCompletion  *parent,
                       VDOAction      *callback,
                       VDOAction      *errorHandler);

/**
 * Asynchronously save any block allocator state that isn't included in the
 * SuperBlock component to the allocator partition. Implements AllocatorAction.
 *
 * @param allocator     The allocator to save
 * @param parent        The completion to notify when the save is complete
 * @param callback      The callback to run when the save is complete
 * @param errorHandler  The handler for save errors
 **/
void closeBlockAllocator(BlockAllocator *allocator,
                         VDOCompletion  *parent,
                         VDOAction      *callback,
                         VDOAction      *errorHandler);

/**
 * Asynchronously save any block allocator state for a full rebuild.
 * Implements AllocatorAction.
 *
 * @param allocator     The allocator to save
 * @param parent        The completion to notify when the save is complete
 * @param callback      The callback to run when the save is complete
 * @param errorHandler  The handler for save errors
 **/
void saveBlockAllocatorForFullRebuild(BlockAllocator *allocator,
                                      VDOCompletion  *parent,
                                      VDOAction      *callback,
                                      VDOAction      *errorHandler);

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
 * journal block. Implements AllocatorAction.
 *
 * @param allocator     The allocator
 * @param parent        The object to notify when the request has been sent
 * @param callback      The function to call when the request has been sent
 * @param errorHandler  The handler for lock request errors
 **/
void releaseTailBlockLocks(BlockAllocator *allocator,
                           VDOCompletion  *parent,
                           VDOAction      *callback,
                           VDOAction      *errorHandler);

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
 * Initiate scrubbing all unrecovered slabs. Implements AllocatorAction.
 *
 * @param allocator     The allocator to scrub
 * @param parent        The object to notify when the request has been sent
 * @param callback      The function to call when the request has been sent
 * @param errorHandler  The handler scrubbing initiation errors
 **/
void scrubAllUnrecoveredSlabsInZone(BlockAllocator *allocator,
                                    VDOCompletion  *parent,
                                    VDOAction      *callback,
                                    VDOAction      *errorHandler);

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
