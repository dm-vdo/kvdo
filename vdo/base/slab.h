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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slab.h#1 $
 */

#ifndef VDO_SLAB_H
#define VDO_SLAB_H

#include "permassert.h"

#include "fixedLayout.h"
#include "journalPoint.h"
#include "referenceOperation.h"
#include "ringNode.h"
#include "types.h"

typedef uint32_t SlabBlockNumber;

typedef enum {
  SLAB_REBUILT = 0,
  SLAB_REPLAYING,
  SLAB_REQUIRES_SCRUBBING,
  SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING,
  SLAB_REBUILDING,
} SlabRebuildStatus;

/**
 * This is the type declaration for the Slab type. (The struct tag is named
 * vdoSlab to avoid a conflict with the linux kernel type). A Slab currently
 * consists of a run of 2^23 data blocks, but that will soon change to
 * dedicate a small number of those blocks for metadata storage for the
 * reference counts and slab journal for the slab.
 **/
struct vdoSlab {
  /** A RingNode to queue this slab in a BlockAllocator ring */
  RingNode             ringNode;

  /** The BlockAllocator that owns this slab */
  BlockAllocator      *allocator;

  /** The reference counts for the data blocks in this slab */
  RefCounts           *referenceCounts;
  /** The journal for this slab */
  SlabJournal         *journal;

  /** The slab number of this slab */
  SlabCount            slabNumber;
  /** The offset in the allocator partition of the first block in this slab */
  PhysicalBlockNumber  start;
  /** The offset of the first block past the end of this slab */
  PhysicalBlockNumber  end;
  /** The starting translated PBN of the slab journal */
  PhysicalBlockNumber  journalOrigin;
  /** The starting translated PBN of the reference counts */
  PhysicalBlockNumber  refCountsOrigin;

  /** The status of the slab */
  SlabRebuildStatus    status;
  /** Whether the slab was ever queued for scrubbing */
  bool                 wasQueuedForScrubbing;

  /** The priority at which this slab has been queued for allocation */
  uint8_t              priority;
};

/**
 * Measure and initialize the configuration to use for each slab.
 *
 * @param [in]  slabSize           The number of blocks per slab
 * @param [in]  slabJournalBlocks  The number of blocks for the slab journal
 * @param [out] slabConfig         The slab configuration to initialize
 *
 * @return VDO_SUCCESS or an error code
 **/
int configureSlab(BlockCount  slabSize,
                  BlockCount  slabJournalBlocks,
                  SlabConfig *slabConfig)
  __attribute__((warn_unused_result));

/**
 * Convert a Slab's RingNode back to the Slab.
 *
 * @param ringNode  The RingNode to convert
 *
 * @return  The RingNode as a Slab
 **/
static inline Slab *slabFromRingNode(RingNode *ringNode)
{
  STATIC_ASSERT(offsetof(Slab, ringNode) == 0);
  return (Slab *) ringNode;
}

/**
 * Get the physical block number of the start of the slab journal
 * relative to the start block allocator partition.
 *
 * @param slabConfig  The slab configuration of the VDO
 * @param origin      The first block of the slab
 **/
__attribute__((warn_unused_result))
PhysicalBlockNumber getSlabJournalStartBlock(const SlabConfig    *slabConfig,
                                             PhysicalBlockNumber  origin);

/**
 * Construct a new, empty slab.
 *
 * @param [in]  slabOrigin       The physical block number within the block
 *                               allocator partition of the first block in
 *                               the slab
 * @param [in]  allocator        The block allocator to which the slab belongs
 * @param [in]  translation      The translation from the depot's partition
 *                               to the physical storage
 * @param [in]  recoveryJournal  The recovery journal of the VDO
 * @param [in]  layer            The layer to which the slab will write
 *                               its metadata
 * @param [in]  slabNumber       The slab number of the slab
 * @param [out] slabPtr          A pointer to receive the new slab
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeSlab(PhysicalBlockNumber   slabOrigin,
             BlockAllocator       *allocator,
             PhysicalBlockNumber   translation,
             RecoveryJournal      *recoveryJournal,
             PhysicalLayer        *layer,
             SlabCount             slabNumber,
             Slab                **slabPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate the reference counts for a slab.
 *
 * @param layer     The layer (for completions)
 * @param slab      The slab whose reference counts need allocation.
 *
 * @return VDO_SUCCESS or an error code
 **/
int allocateRefCountsForSlab(PhysicalLayer *layer, Slab *slab)
  __attribute__((warn_unused_result));

/**
 * Destroy a slab and null out the reference to it.
 *
 * @param slabPtr  The reference to the slab to destroy
 **/
void freeSlab(Slab **slabPtr);

/**
 * Get the physical zone number of a slab.
 *
 * @param slab  The slab
 *
 * @return The number of the slab's physical zone
 **/
ZoneCount getSlabZoneNumber(Slab *slab)
  __attribute__((warn_unused_result));

/**
 * Check whether a slab is unrecovered.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is unrecovered
 **/
static inline bool isUnrecoveredSlab(const Slab *slab)
{
  return (slab->status != SLAB_REBUILT);
}

/**
 * Check whether a slab is being replayed into.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is replaying
 **/
static inline bool isReplayingSlab(const Slab *slab)
{
  return (slab->status == SLAB_REPLAYING);
}

/**
 * Check whether a slab is being rebuilt.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is being rebuilt
 **/
static inline bool slabIsRebuilding(const Slab *slab)
{
  return (slab->status == SLAB_REBUILDING);
}

/**
 * Mark a slab as replaying, during offline recovery.
 *
 * @param slab  The slab to mark
 **/
void markSlabReplaying(Slab *slab);

/**
 * Mark a slab as unrecovered, for online recovery.
 *
 * @param slab  The slab to mark
 **/
void markSlabUnrecovered(Slab *slab);

/**
 * Get the current number of free blocks in a slab.
 *
 * @param slab  The slab to query
 *
 * @return the number of free blocks in the slab
 **/
BlockCount getSlabFreeBlockCount(const Slab *slab)
  __attribute__((warn_unused_result));

/**
 * Increment or decrement the reference count of a block in a slab.
 *
 * @param slab          The slab containing the block (may be NULL when
 *                      referencing the zero block)
 * @param journalPoint  The slab journal entry corresponding to this change
 * @param operation     The operation to perform on the reference count
 *
 * @return VDO_SUCCESS or an error
 **/
int modifySlabReferenceCount(Slab               *slab,
                             const JournalPoint *journalPoint,
                             ReferenceOperation  operation)
  __attribute__((warn_unused_result));

/**
 * Acquire a provisional reference on behalf of a PBN lock if the block it
 * locks is unreferenced.
 *
 * @param slab  The slab which contains the block
 * @param pbn   The physical block to reference
 * @param lock  The lock
 *
 * @return VDO_SUCCESS or an error
 **/
int acquireProvisionalReference(Slab                *slab,
                                PhysicalBlockNumber  pbn,
                                PBNLock             *lock)
  __attribute__((warn_unused_result));

/**
 * Determine the index within the slab of a particular physical block number.
 *
 * @param [in]  slab                    The slab
 * @param [in]  physicalBlockNumber     The physical block number
 * @param [out] slabBlockNumberPtr      A pointer to the slab block number
 *
 * @return VDO_SUCCESS or an error code
 **/
int slabBlockNumberFromPBN(Slab                *slab,
                           PhysicalBlockNumber  physicalBlockNumber,
                           SlabBlockNumber     *slabBlockNumberPtr)
  __attribute__((warn_unused_result));

/**
 * Check whether the reference counts for a given rebuilt slab should be saved.
 * Implements SlabStatusChecker.
 *
 * @param slab  The slab to check
 *
 * @return true if the slab should be saved
 **/
bool shouldSaveFullyBuiltSlab(const Slab *slab)
  __attribute__((warn_unused_result));

/**
 * Dump information about a slab to the log for debugging.
 *
 * @param slab   The slab to dump
 **/
void dumpSlab(const Slab *slab);

#endif // VDO_SLAB_H
