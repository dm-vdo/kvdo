/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepot.h#18 $
 */

#ifndef SLAB_DEPOT_H
#define SLAB_DEPOT_H

#include "buffer.h"

#include "adminState.h"
#include "atomic.h"
#include "completion.h"
#include "fixedLayout.h"
#include "journalPoint.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

/**
 * A slab_depot is responsible for managing all of the slabs and block
 * allocators of a VDO. It has a single array of slabs in order to eliminate
 * the need for additional math in order to compute which physical zone a PBN
 * is in. It also has a block_allocator per zone.
 *
 * Load operations are required to be performed on a single thread. Normal
 * operations are assumed to be performed in the appropriate zone. Allocations
 * and reference count updates must be done from the thread of their physical
 * zone. Requests to commit slab journal tail blocks from the recovery journal
 * must be done on the journal zone thread. Save operations are required to be
 * launched from the same thread as the original load operation.
 **/

typedef enum {
  NORMAL_LOAD,
  RECOVERY_LOAD,
  REBUILD_LOAD
} SlabDepotLoadType;

/**
 * Calculate the number of slabs a depot would have.
 *
 * @param depot  The depot
 *
 * @return The number of slabs
 **/
SlabCount calculateSlabCount(struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Create a slab depot.
 *
 * @param [in]  blockCount        The number of blocks initially available
 * @param [in]  firstBlock        The number of the first block which may be
 *                                allocated
 * @param [in]  slabConfig        The slab configuration
 * @param [in]  threadConfig      The thread configuration of the VDO
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  vioPoolSize       The size of the VIO pool
 * @param [in]  layer             The physical layer below this depot
 * @param [in]  summaryPartition  The partition which holds the slab summary
 * @param [in]  readOnlyNotifier  The context for entering read-only mode
 * @param [in]  recoveryJournal   The recovery journal of the VDO
 * @param [in]  vdoState          A pointer to the VDO's atomic state
 * @param [out] depotPtr          A pointer to hold the depot
 *
 * @return A success or error code
 **/
int makeSlabDepot(BlockCount                  blockCount,
                  PhysicalBlockNumber         firstBlock,
                  SlabConfig                  slabConfig,
                  const ThreadConfig         *threadConfig,
                  Nonce                       nonce,
                  BlockCount                  vioPoolSize,
                  PhysicalLayer              *layer,
                  struct partition           *summaryPartition,
                  struct read_only_notifier  *readOnlyNotifier,
                  struct recovery_journal    *recoveryJournal,
                  Atomic32                   *vdoState,
                  struct slab_depot         **depotPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a slab depot and null out the reference to it.
 *
 * @param depotPtr  The reference to the depot to destroy
 **/
void freeSlabDepot(struct slab_depot **depotPtr);

/**
 * Get the size of the encoded state of a slab depot.
 *
 * @return The encoded size of the depot's state
 **/
size_t getSlabDepotEncodedSize(void)
  __attribute__((warn_unused_result));

/**
 * Encode the state of a slab depot into a buffer.
 *
 * @param depot   The depot to encode
 * @param buffer  The buffer to encode into
 *
 * @return UDS_SUCCESS or an error
 **/
int encodeSlabDepot(const struct slab_depot *depot, Buffer *buffer)
  __attribute__((warn_unused_result));

/**
 * Decode the state of a slab depot saved in a buffer.
 *
 * @param [in]  buffer            The buffer containing the saved state
 * @param [in]  threadConfig      The thread config of the VDO
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  layer             The physical layer below this depot
 * @param [in]  summaryPartition  The partition which holds the slab summary
 * @param [in]  readOnlyNotifier  The context for entering read-only mode
 * @param [in]  recoveryJournal   The recovery journal of the VDO
 * @param [out] depotPtr          A pointer to hold the depot
 *
 * @return A success or error code
 **/
int decodeSodiumSlabDepot(Buffer                     *buffer,
                          const ThreadConfig         *threadConfig,
                          Nonce                       nonce,
                          PhysicalLayer              *layer,
                          struct partition           *summaryPartition,
                          struct read_only_notifier  *readOnlyNotifier,
                          struct recovery_journal    *recoveryJournal,
                          struct slab_depot         **depotPtr)
  __attribute__((warn_unused_result));

/**
 * Decode the state of a slab depot saved in a buffer.
 *
 * @param [in]  buffer            The buffer containing the saved state
 * @param [in]  threadConfig      The thread config of the VDO
 * @param [in]  nonce             The nonce of the VDO
 * @param [in]  layer             The physical layer below this depot
 * @param [in]  summaryPartition  The partition which holds the slab summary
 * @param [in]  readOnlyNotifier  The context for entering read-only mode
 * @param [in]  recoveryJournal   The recovery journal of the VDO
 * @param [in]  vdoState          A pointer to the VDO's atomic state
 * @param [out] depotPtr          A pointer to hold the depot
 *
 * @return A success or error code
 **/
int decodeSlabDepot(Buffer                     *buffer,
                    const ThreadConfig         *threadConfig,
                    Nonce                       nonce,
                    PhysicalLayer              *layer,
                    struct partition           *summaryPartition,
                    struct read_only_notifier  *readOnlyNotifier,
                    struct recovery_journal    *recoveryJournal,
                    Atomic32                   *vdoState,
                    struct slab_depot         **depotPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate the RefCounts for all slabs in the depot. This method may be called
 * only before entering normal operation from the load thread.
 *
 * @param depot  The depot whose RefCounts need allocation
 *
 * @return VDO_SUCCESS or an error
 **/
int allocateSlabRefCounts(struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the block allocator for a specified physical zone from a depot.
 *
 * @param depot       The depot
 * @param zoneNumber  The physical zone
 *
 * @return The block allocator for the specified zone
 **/
struct block_allocator *getBlockAllocatorForZone(struct slab_depot *depot,
                                         ZoneCount                  zoneNumber)
  __attribute__((warn_unused_result));

/**
 * Get the number of the slab that contains a specified block.
 *
 * @param depot          The slab depot
 * @param pbn            The physical block number
 * @param slabNumberPtr  A pointer to hold the slab number
 *
 * @return VDO_SUCCESS or an error
 **/
int getSlabNumber(const struct slab_depot *depot,
                  PhysicalBlockNumber      pbn,
                  SlabCount               *slabNumberPtr)
  __attribute__((warn_unused_result));

/**
 * Get the slab object for the slab that contains a specified block. Will put
 * the VDO in read-only mode if the PBN is not a valid data block nor the zero
 * block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number
 *
 * @return The slab containing the block, or NULL if the block number is the
 *         zero block or otherwise out of range
 **/
struct vdo_slab *getSlab(const struct slab_depot *depot,
                         PhysicalBlockNumber      pbn)
  __attribute__((warn_unused_result));

/**
 * Get the slab journal for the slab that contains a specified block.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number within the block depot partition
 *               of any block in the slab
 *
 * @return The slab journal of the slab containing the block, or NULL if the
 *         block number is for the zero block or otherwise out of range
 **/
struct slab_journal *getSlabJournal(const struct slab_depot *depot,
                                    PhysicalBlockNumber      pbn)
  __attribute__((warn_unused_result));

/**
 * Determine how many new references a block can acquire. This method must be
 * called from the the physical zone thread of the PBN.
 *
 * @param depot  The slab depot
 * @param pbn    The physical block number that is being queried
 *
 * @return the number of available references
 **/
uint8_t getIncrementLimit(struct slab_depot *depot, PhysicalBlockNumber pbn)
  __attribute__((warn_unused_result));

/**
 * Determine whether the given PBN refers to a data block.
 *
 * @param depot  The depot
 * @param pbn    The physical block number to ask about
 *
 * @return <code>True</code> if the PBN corresponds to a data block
 **/
bool isPhysicalDataBlock(const struct slab_depot *depot,
                         PhysicalBlockNumber      pbn)
  __attribute__((warn_unused_result));

/**
 * Get the total number of data blocks allocated across all the slabs in the
 * depot, which is the total number of blocks with a non-zero reference count.
 * This may be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of blocks with a non-zero reference count
 **/
BlockCount getDepotAllocatedBlocks(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the total of the statistics from all the block allocators in the depot.
 *
 * @param depot  The slab depot
 *
 * @return The statistics from all block allocators in the depot
 **/
BlockAllocatorStatistics
getDepotBlockAllocatorStatistics(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the total number of data blocks in all the slabs in the depot. This may
 * be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of data blocks in all slabs
 **/
BlockCount getDepotDataBlocks(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the total number of free blocks remaining in all the slabs in the
 * depot, which is the total number of blocks that have a zero reference
 * count. This may be called from any thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of blocks with a zero reference count
 **/
BlockCount getDepotFreeBlocks(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the total number of slabs in the depot
 *
 * @param depot  The slab depot
 *
 * @return The total number of slabs
 **/
SlabCount getDepotSlabCount(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the total number of unrecovered slabs in the depot, which is the total
 * number of unrecovered slabs from all zones. This may be called from any
 * thread.
 *
 * @param depot  The slab depot
 *
 * @return The total number of slabs that are unrecovered
 **/
SlabCount getDepotUnrecoveredSlabCount(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the aggregated slab journal statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The aggregated statistics for all slab journals in the depot
 **/
SlabJournalStatistics
getDepotSlabJournalStatistics(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the cumulative RefCounts statistics for the depot.
 *
 * @param depot  The slab depot
 *
 * @return The cumulative statistics for all RefCounts in the depot
 **/
RefCountsStatistics getDepotRefCountsStatistics(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Asynchronously load any slab depot state that isn't included in the
 * SuperBlock component. This method may be called only before entering normal
 * operation from the load thread.
 *
 * @param depot        The depot to load
 * @param operation    The type of load to perform
 * @param parent       The completion to finish when the load is complete
 * @param context      Additional context for the load operation; may be NULL
 **/
void loadSlabDepot(struct slab_depot *depot,
                   AdminStateCode     operation,
                   VDOCompletion     *parent,
                   void              *context);

/**
 * Prepare the slab depot to come online and start allocating blocks. This
 * method may be called only before entering normal operation from the load
 * thread. It must be called before allocation may proceed.
 *
 * @param depot     The depot to prepare
 * @param loadType  The load type
 * @param parent    The completion to finish when the operation is complete
 **/
void prepareToAllocate(struct slab_depot *depot,
                       SlabDepotLoadType  loadType,
                       VDOCompletion     *parent);

/**
 * Update the slab depot to reflect its new size in memory. This size is saved
 * to disk as part of the super block.
 *
 * @param depot  The depot to update
 **/
void updateSlabDepotSize(struct slab_depot *depot);

/**
 * Allocate new memory needed for a resize of a slab depot to the given size.
 *
 * @param depot    The depot to prepare to resize
 * @param newSize  The number of blocks in the new depot
 *
 * @return VDO_SUCCESS or an error
 **/
int prepareToGrowSlabDepot(struct slab_depot *depot, BlockCount newSize)
  __attribute__((warn_unused_result));

/**
 * Use the new slabs allocated for resize.
 *
 * @param depot   The depot
 * @param parent  The object to notify when complete
 **/
void useNewSlabs(struct slab_depot *depot, VDOCompletion *parent);

/**
 * Abandon any new slabs in this depot, freeing them as needed.
 *
 * @param depot  The depot
 **/
void abandonNewSlabs(struct slab_depot *depot);

/**
 * Drain all slab depot I/O. If saving, or flushing, all dirty depot metadata
 * will be written out. If saving or suspending, the depot will be left in a
 * suspended state.
 *
 * @param depot      The depot to drain
 * @param operation  The drain operation (flush, rebuild, suspend, or save)
 * @param parent     The completion to finish when the drain is complete
 **/
void drainSlabDepot(struct slab_depot *depot,
                    AdminStateCode     operation,
                    VDOCompletion     *parent);

/**
 * Resume a suspended slab depot.
 *
 * @param depot   The depot to resume
 * @param parent  The completion to finish when the depot has resumed
 **/
void resumeSlabDepot(struct slab_depot *depot, VDOCompletion *parent);

/**
 * Commit all dirty tail blocks which are locking a given recovery journal
 * block. This method must be called from the journal zone thread.
 *
 * @param depot                The depot
 * @param recoveryBlockNumber  The sequence number of the recovery journal
 *                             block whose locks should be released
 **/
void
commitOldestSlabJournalTailBlocks(struct slab_depot *depot,
                                  SequenceNumber     recoveryBlockNumber);

/**
 * Get the SlabConfig of a depot.
 *
 * @param depot  The slab depot
 *
 * @return The slab configuration of the specified depot
 **/
const SlabConfig *getSlabConfig(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the slab summary.
 *
 * @param depot  The slab depot
 *
 * @return The slab summary
 **/
SlabSummary *getSlabSummary(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Get the portion of the slab summary for a given physical zone.
 *
 * @param depot  The slab depot
 * @param zone   The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
SlabSummaryZone *getSlabSummaryForZone(const struct slab_depot *depot,
                                       ZoneCount                zone)
  __attribute__((warn_unused_result));

/**
 * Scrub all unrecovered slabs.
 *
 * @param depot         The depot to scrub
 * @param parent        The object to notify when scrubbing has been launched
 *                      for all zones
 **/
void scrubAllUnrecoveredSlabs(struct slab_depot *depot, VDOCompletion *parent);

/**
 * Check whether there are outstanding unrecovered slabs.
 *
 * @param depot  The slab depot
 *
 * @return Whether there are outstanding unrecovered slabs
 **/
bool hasUnrecoveredSlabs(struct slab_depot *depot);

/**
 * Get the physical size to which this depot is prepared to grow.
 *
 * @param depot  The slab depot
 *
 * @return The new number of blocks the depot will be grown to, or 0 if the
 *         depot is not prepared to grow
 **/
BlockCount getNewDepotSize(const struct slab_depot *depot)
  __attribute__((warn_unused_result));

/**
 * Dump the slab depot, in a thread-unsafe fashion.
 *
 * @param depot  The slab depot
 **/
void dumpSlabDepot(const struct slab_depot *depot);

#endif // SLAB_DEPOT_H
