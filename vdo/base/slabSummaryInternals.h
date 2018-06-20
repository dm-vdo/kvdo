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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabSummaryInternals.h#3 $
 */

#ifndef SLAB_SUMMARY_INTERNALS_H
#define SLAB_SUMMARY_INTERNALS_H

#include "slabSummary.h"

#include "atomic.h"

typedef struct slabSummaryEntry {
  /** Bits 7..0: The offset of the tail block within the slab journal */
  TailBlockOffset tailBlockOffset;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  /** Bits 13..8: A hint about the fullness of the slab */
  unsigned int    fullnessHint  : 6;
  /** Bit 14: Whether the refCounts must be loaded from the layer */
  unsigned int    loadRefCounts : 1;
  /** Bit 15: The believed cleanliness of this slab */
  unsigned int    isDirty       : 1;
#else
  /** Bit 15: The believed cleanliness of this slab */
  unsigned int    isDirty       : 1;
  /** Bit 14: Whether the refCounts must be loaded from the layer */
  unsigned int    loadRefCounts : 1;
  /** Bits 13..8: A hint about the fullness of the slab */
  unsigned int    fullnessHint  : 6;
#endif
}  __attribute__((packed)) SlabSummaryEntry;

typedef struct slabSummaryBlock {
  /** The zone to which this block belongs */
  SlabSummaryZone     *zone;
  /** Whether this block has a write outstanding */
  bool                 currentlyWriting;
  /** Whether this block has had modifications since it issued a write */
  bool                 needsWriting;
  /** Ring of updates waiting on the outstanding write */
  WaitQueue            currentUpdateWaiters;
  /** Ring of updates waiting on the next write */
  WaitQueue            nextUpdateWaiters;
  /** The block number of this SummaryBlock on the partition */
  PhysicalBlockNumber  pbn;
  /** The active SlabSummaryEntry array for this block */
  SlabSummaryEntry    *entries;
  /** The VIO used to write this block */
  VIO                 *vio;
  /** The packed entries, one block long, backing the VIO */
  char                *outgoingEntries;
} SlabSummaryBlock;

/**
 * The statistics for all the slab summary zones owned by this slab summary.
 * These fields are all mutated only by their physical zone threads, but are
 * read by other threads when gathering statistics for the entire depot.
 **/
typedef struct atomicSlabSummaryStatistics {
  /** Number of blocks written */
  Atomic64 blocksWritten;
} AtomicSlabSummaryStatistics;

/**
 * Only the actions which need something performed upon completion need to be
 * enumerated here.
 **/
typedef enum {
  NONE_REQUESTED = 0,
  CLOSE_REQUESTED,
  SUSPEND_REQUESTED,
} RequestedAction;

struct slabSummaryZone {
  /** The summary of which this is a zone */
  SlabSummary      *summary;
  /** The number of this zone */
  ZoneCount         zoneNumber;
  /** The completion waiting on the zone to be saved */
  VDOCompletion    *saveWaiter;
  /** The pending action, if any */
  RequestedAction   pendingAction;
  /** Whether this zone is paused */
  bool              suspended;
  /** The array (owned by the blocks) of all entries */
  SlabSummaryEntry *entries;
  /** The array of SlabSummaryEntryBlocks */
  SlabSummaryBlock  summaryBlocks[];
};

struct slabSummary {
  /** The completion used when combining old zones (load) */
  VDOCompletion                completion;
  /** The context for entering read-only mode */
  ReadOnlyModeContext         *readOnlyContext;
  /** The statistics for this slab summary */
  AtomicSlabSummaryStatistics  statistics;
  /** The start of the slab summary partition relative to the layer */
  PhysicalBlockNumber          origin;
  /** The number of bits to shift to get a 7-bit fullness hint */
  unsigned int                 hintShift;
  /** The number of blocks (calculated based on MAX_SLABS) */
  BlockCount                   blocksPerZone;
  /** The number of slabs per block (calculated from block size) */
  SlabCount                    entriesPerBlock;
  /** The entries for all of the zones the partition can hold */
  SlabSummaryEntry            *entries;
  /** The number of zones which were active at the time of the last update */
  ZoneCount                    zonesToCombine;
  /** The current number of active zones */
  ZoneCount                    zoneCount;
  /** The currently active zones */
  SlabSummaryZone             *zones[];
};

/**
 * Treating the current entries buffer as the on-disk value of all zones,
 * update every zone to the correct values for every slab.
 *
 * @param summary       The summary whose entries should be combined
 **/
void combineZones(SlabSummary *summary);

#endif // SLAB_SUMMARY_INTERNALS_H
