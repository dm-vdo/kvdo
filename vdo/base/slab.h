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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slab.h#15 $
 */

#ifndef VDO_SLAB_H
#define VDO_SLAB_H

#include "permassert.h"

#include "adminState.h"
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
 * This is the type declaration for the vdo_slab type. A vdo_slab currently
 * consists of a run of 2^23 data blocks, but that will soon change to
 * dedicate a small number of those blocks for metadata storage for the
 * reference counts and slab journal for the slab.
 **/
struct vdo_slab {
  /** A RingNode to queue this slab in a block_allocator ring */
  RingNode                ringNode;

  /** The struct block_allocator that owns this slab */
  struct block_allocator *allocator;

  /** The reference counts for the data blocks in this slab */
  struct ref_counts      *referenceCounts;
  /** The journal for this slab */
  SlabJournal            *journal;

  /** The slab number of this slab */
  SlabCount               slabNumber;
  /** The offset in the allocator partition of the first block in this slab */
  PhysicalBlockNumber     start;
  /** The offset of the first block past the end of this slab */
  PhysicalBlockNumber     end;
  /** The starting translated PBN of the slab journal */
  PhysicalBlockNumber     journalOrigin;
  /** The starting translated PBN of the reference counts */
  PhysicalBlockNumber     refCountsOrigin;

  /** The administrative state of the slab */
  struct admin_state      state;
  /** The status of the slab */
  SlabRebuildStatus       status;
  /** Whether the slab was ever queued for scrubbing */
  bool                    wasQueuedForScrubbing;

  /** The priority at which this slab has been queued for allocation */
  uint8_t                 priority;
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
 * Convert a vdo_slab's RingNode back to the vdo_slab.
 *
 * @param ringNode  The RingNode to convert
 *
 * @return  The RingNode as a vdo_slab
 **/
static inline struct vdo_slab *slabFromRingNode(RingNode *ringNode)
{
  STATIC_ASSERT(offsetof(struct vdo_slab, ringNode) == 0);
  return (struct vdo_slab *) ringNode;
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
 *                               allocator partition of the first block in the
 *                               slab
 * @param [in]  allocator        The block allocator to which the slab belongs
 * @param [in]  translation      The translation from the depot's partition to
 *                               the physical storage
 * @param [in]  recoveryJournal  The recovery journal of the VDO
 * @param [in]  slabNumber       The slab number of the slab
 * @param [in]  isNew            <code>true</code> if this slab is being
 *                               allocated as part of a resize
 * @param [out] slabPtr          A pointer to receive the new slab
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeSlab(PhysicalBlockNumber       slabOrigin,
             struct block_allocator   *allocator,
             PhysicalBlockNumber       translation,
             struct recovery_journal  *recoveryJournal,
             SlabCount                 slabNumber,
             bool                      isNew,
             struct vdo_slab         **slabPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate the reference counts for a slab.
 *
 * @param slab  The slab whose reference counts need allocation.
 *
 * @return VDO_SUCCESS or an error code
 **/
int allocateRefCountsForSlab(struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Destroy a slab and null out the reference to it.
 *
 * @param slabPtr  The reference to the slab to destroy
 **/
void freeSlab(struct vdo_slab **slabPtr);

/**
 * Get the physical zone number of a slab.
 *
 * @param slab  The slab
 *
 * @return The number of the slab's physical zone
 **/
ZoneCount getSlabZoneNumber(struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Check whether a slab is unrecovered.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is unrecovered
 **/
static inline bool isUnrecoveredSlab(const struct vdo_slab *slab)
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
static inline bool isReplayingSlab(const struct vdo_slab *slab)
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
static inline bool slabIsRebuilding(const struct vdo_slab *slab)
{
  return (slab->status == SLAB_REBUILDING);
}

/**
 * Mark a slab as replaying, during offline recovery.
 *
 * @param slab  The slab to mark
 **/
void markSlabReplaying(struct vdo_slab *slab);

/**
 * Mark a slab as unrecovered, for online recovery.
 *
 * @param slab  The slab to mark
 **/
void markSlabUnrecovered(struct vdo_slab *slab);

/**
 * Get the current number of free blocks in a slab.
 *
 * @param slab  The slab to query
 *
 * @return the number of free blocks in the slab
 **/
BlockCount getSlabFreeBlockCount(const struct vdo_slab *slab)
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
int modifySlabReferenceCount(struct vdo_slab            *slab,
                             const struct journal_point *journalPoint,
                             struct reference_operation  operation)
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
int acquireProvisionalReference(struct vdo_slab     *slab,
                                PhysicalBlockNumber  pbn,
                                struct pbn_lock     *lock)
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
int slabBlockNumberFromPBN(struct vdo_slab     *slab,
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
bool shouldSaveFullyBuiltSlab(const struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Start an administrative operation on a slab.
 *
 * @param slab       The slab to load
 * @param operation  The type of load to perform
 * @param parent     The object to notify when the operation is complete
 **/
void startSlabAction(struct vdo_slab *slab,
                     AdminStateCode   operation,
                     VDOCompletion   *parent);

/**
 * Inform a slab that its journal has been loaded.
 *
 * @param slab    The slab whose journal has been loaded
 * @param result  The result of the load operation
 **/
void notifySlabJournalIsLoaded(struct vdo_slab *slab, int result);

/**
 * Check whether a slab is open, i.e. is neither quiescent nor quiescing.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is open
 **/
bool isSlabOpen(struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Check whether a slab is currently draining.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is performing a drain operation
 **/
bool isSlabDraining(struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Inform a slab that its journal has finished draining.
 *
 * @param slab    The slab whose journal has been drained
 * @param result  The result of the drain operation
 **/
void notifySlabJournalIsDrained(struct vdo_slab *slab, int result);

/**
 * Inform a slab that its ref_counts have finished draining.
 *
 * @param slab    The slab whose ref_counts object has been drained
 * @param result  The result of the drain operation
 **/
void notifyRefCountsAreDrained(struct vdo_slab *slab, int result);

/**
 * Check whether a slab is currently resuming.
 *
 * @param slab  The slab to check
 *
 * @return <code>true</code> if the slab is performing a resume operation
 **/
bool isSlabResuming(struct vdo_slab *slab)
  __attribute__((warn_unused_result));

/**
 * Finish scrubbing a slab now that it has been rebuilt by updating its status,
 * queueing it for allocation, and reopening its journal.
 *
 * @param slab  The slab whose reference counts have been rebuilt from its
 *              journal
 **/
void finishScrubbingSlab(struct vdo_slab *slab);

/**
 * Dump information about a slab to the log for debugging.
 *
 * @param slab   The slab to dump
 **/
void dumpSlab(const struct vdo_slab *slab);

#endif // VDO_SLAB_H
