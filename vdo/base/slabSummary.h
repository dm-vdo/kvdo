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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabSummary.h#1 $
 */

#ifndef SLAB_SUMMARY_H
#define SLAB_SUMMARY_H

#include "completion.h"
#include "fixedLayout.h"
#include "slab.h"
#include "statistics.h"
#include "types.h"
#include "waitQueue.h"

/**
 * The SlabSummary serves to provide hints during recovery about
 * slabs, giving information such as the rough number of free blocks (useful
 * to prioritize scrubbing of slabs), the cleanliness of a slab (so that
 * clean slabs containing free space will be used on restart), and the
 * location of the slab journals' tail block. It obviates the need to read all
 * the entire slab journals to determine the entries that need not be replayed.
 *
 * The SlabSummary lives in their own partition, immediately before the
 * recovery journal at the end of the block map.
 *
 * In the future, a copy of this object may exist for every thread,
 * necessitating more storage. It is stored, thence, at the end of the volume,
 * so that upgrades can change its size.
 *
 * During resize, the SlabSummary moves its backing partition and is saved once
 * moved; the SlabSummary is not permitted to overwrite the previous journal
 * space.
 *
 * Note that SlabSummary does not have its own version information, but
 * relies on the master version number.
 **/

/**
 * The offset of a slab journal tail block.
 **/
typedef uint8_t TailBlockOffset;

/**
 * A slab status is a very small structure for use in determining the ordering
 * of slabs in the scrubbing process.
 **/
typedef struct slabStatus {
  SlabCount slabNumber;
  bool      isClean;
  uint8_t   emptiness;
} SlabStatus;

/**
 * Returns the size on disk of the SlabSummary structure.
 *
 * @param blockSize  The block size of the physical layer
 *
 * @return the blocks required to store the SlabSummary on disk
 **/
BlockCount getSlabSummarySize(BlockSize blockSize)
__attribute__((warn_unused_result));

/**
 * Create a slab summary.
 *
 * @param [in]  layer                      The layer
 * @param [in]  partition                  The partition to hold the summary
 * @param [in]  threadConfig               The thread config of the VDO
 * @param [in]  slabSizeShift              The number of bits in the slab size
 * @param [in]  maximumFreeBlocksPerSlab   The maximum number of free blocks
 *                                         a slab can have
 * @param [in]  readOnlyContext            The context for entering read-only
 *                                         mode
 * @param [out] slabSummaryPtr             A pointer to hold the summary
 *
 * @return VDO_SUCCESS or an error
 **/
int makeSlabSummary(PhysicalLayer        *layer,
                    Partition            *partition,
                    const ThreadConfig   *threadConfig,
                    unsigned int          slabSizeShift,
                    BlockCount            maximumFreeBlocksPerSlab,
                    ReadOnlyModeContext  *readOnlyContext,
                    SlabSummary         **slabSummaryPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy a SlabSummary and NULL out the reference to it.
 *
 * @param [in,out] slabSummaryPtr A pointer to the SlabSummary to free
 **/
void freeSlabSummary(SlabSummary **slabSummaryPtr);

/**
 * Get the portion of the slab summary for a specified zone.
 *
 * @param summary  The slab summary
 * @param zone     The zone
 *
 * @return The portion of the slab summary for the specified zone
 **/
SlabSummaryZone *getSummaryForZone(SlabSummary *summary, ZoneCount zone)
  __attribute__((warn_unused_result));

/**
 * Suspend a zone of the slab summary.
 *
 * @param summaryZone  The zone to suspend
 * @param parent       The object to notify when the suspend is complete
 **/
void suspendSlabSummaryZone(SlabSummaryZone *summaryZone,
                            VDOCompletion   *parent);

/**
 * Resume a zone of the slab summary.
 *
 * @param summaryZone  The zone to resume
 * @param parent       The object to notify when the zone is resumed
 **/
void resumeSlabSummaryZone(SlabSummaryZone *summaryZone,
                           VDOCompletion   *parent);

/**
 * Save a zone of the slab summary.
 *
 * @param summaryZone  The zone to save
 * @param parent       The object to notify when the save is complete
 **/
void saveSlabSummaryZone(SlabSummaryZone *summaryZone, VDOCompletion *parent);

/**
 * Wait for the slab summary to be completely saved.
 *
 * @param summaryZone  The zone to save
 * @param parent       The object to notify when the close is complete
 **/
void closeSlabSummaryZone(SlabSummaryZone *summaryZone, VDOCompletion *parent);

/**
 * Update the entry for a slab.
 *
 * @param summaryZone     The SlabSummaryZone for the zone of the slab
 * @param waiter          The waiter that is updating the summary
 * @param slabNumber      The slab number to update
 * @param tailBlockOffset The offset of slab journal's tail block
 * @param loadRefCounts   Whether the refCounts must be loaded from the layer
 *                        on the next load
 * @param isClean         Whether the slab is clean
 * @param freeBlocks      The number of free blocks
 **/
void updateSlabSummaryEntry(SlabSummaryZone *summaryZone,
                            Waiter          *waiter,
                            SlabCount        slabNumber,
                            TailBlockOffset  tailBlockOffset,
                            bool             loadRefCounts,
                            bool             isClean,
                            BlockCount       freeBlocks);

/**
 * Get the stored tail block offset for a slab.
 *
 * @param summaryZone       The SlabSummaryZone to use
 * @param slabNumber        The slab number to get the offset for
 *
 * @return The tail block offset for the slab
 **/
TailBlockOffset getSummarizedTailBlockOffset(SlabSummaryZone *summaryZone,
                                             SlabCount        slabNumber)
  __attribute__((warn_unused_result));

/**
 * Whether refCounts must be loaded from the layer.
 *
 * @param summaryZone   The SlabSummaryZone to use
 * @param slabNumber    The slab number to get information for
 *
 * @return Whether refCounts must be loaded
 **/
bool mustLoadRefCounts(SlabSummaryZone *summaryZone, SlabCount slabNumber)
  __attribute__((warn_unused_result));

/**
 * Get the stored cleanliness information for a single slab.
 *
 * @param summaryZone   The SlabSummaryZone to use
 * @param slabNumber    The slab number to get information for
 *
 * @return Whether the slab is clean
 **/
bool getSummarizedCleanliness(SlabSummaryZone *summaryZone,
                              SlabCount        slabNumber)
  __attribute__((warn_unused_result));

/**
 * Get the stored emptiness information for a single slab.
 *
 * @param summaryZone    The SlabSummaryZone to use
 * @param slabNumber     The slab number to get information for
 *
 * @return An approximation to the free blocks in the slab
 **/
BlockCount getSummarizedFreeBlockCount(SlabSummaryZone *summaryZone,
                                       SlabCount        slabNumber)
  __attribute__((warn_unused_result));

/**
 * Get the stored RefCounts state information for a single slab. Used
 * in testing only.
 *
 * @param [in]  summaryZone     The SlabSummaryZone to use
 * @param [in]  slabNumber      The slab number to get information for
 * @param [out] freeBlockHint   The approximate number of free blocks
 * @param [out] isClean         Whether the slab is clean
 **/
void getSummarizedRefCountsState(SlabSummaryZone *summaryZone,
                                 SlabCount        slabNumber,
                                 size_t          *freeBlockHint,
                                 bool            *isClean);

/**
 * Get the stored slab statuses for all slabs in a zone.
 *
 * @param [in]     summaryZone   The SlabSummaryZone to use
 * @param [in]     slabCount     The number of slabs to fetch
 * @param [in,out] statuses      An array of SlabStatuses to populate
 **/
void getSummarizedSlabStatuses(SlabSummaryZone *summaryZone,
                               SlabCount        slabCount,
                               SlabStatus      *statuses);

/**
 * Set the origin of the slab summary relative to the physical layer.
 *
 * @param summary    The SlabSummary to update
 * @param partition  The slab summary partition
 **/
void setSlabSummaryOrigin(SlabSummary *summary, Partition *partition);

/**
 * Read in all the slab summary data from the slab summary partition,
 * combine all the previously used zones into a single zone, and then
 * write the combined summary back out to each possible zones' summary
 * region.
 *
 * @param summary         The summary to load
 * @param zonesToCombine  The number of zones to be combined; if set to 0,
 *                        all of the summary will be initialized as new.
 * @param parent          The parent of this operation
 * @param callback        The function to call when the load is complete
 **/
void loadSlabSummary(SlabSummary *summary,
                     ZoneCount    zonesToCombine,
                     void        *parent,
                     VDOAction   *callback);

/**
 * Fetch the cumulative statistics for all slab summary zones in a summary.
 *
 * @param summary       The summary in question
 *
 * @return the cumulative slab summary statistics for the summary
 **/
SlabSummaryStatistics getSlabSummaryStatistics(const SlabSummary *summary)
  __attribute__((warn_unused_result));

#endif // SLAB_SUMMARY_H
