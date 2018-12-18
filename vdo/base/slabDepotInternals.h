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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotInternals.h#4 $
 */

#ifndef SLAB_DEPOT_INTERNALS_H
#define SLAB_DEPOT_INTERNALS_H

#include "slabDepot.h"

#include "atomic.h"

/**
 * A function which is to be applied asynchronously to all of the allocators of
 * a slab depot.
 *
 * @param allocator  The allocator to apply the action to
 * @param parent     The object to notify when the action has been applied to
 *                   all of the allocators
 **/
typedef void AllocatorAction(BlockAllocator *allocator, VDOCompletion *parent);

/**
 * An action to be performed on the depot.
 **/
typedef struct {
  /** The action to be applied to each allocator */
  AllocatorAction *action;
  /** The method to run back on the journal thread after the action is done */
  VDOAction       *conclusion;
  /** The object to notify when the action is complete */
  VDOCompletion   *parent;
} DepotAction;

struct slabDepot {
  VDOCompletion         completion;
  ZoneCount             zoneCount;
  ZoneCount             oldZoneCount;
  SlabConfig            slabConfig;
  SlabSummary          *slabSummary;
  ReadOnlyModeContext  *readOnlyContext;

  PhysicalBlockNumber   firstBlock;
  PhysicalBlockNumber   lastBlock;
  PhysicalBlockNumber   origin;

  /** slabSize == (1 << slabSizeShift) */
  unsigned int          slabSizeShift;

  /** The completion for loading slabs */
  VDOCompletion        *slabCompletion;

  /** The current action being performed by the depot */
  DepotAction           currentAction;
  /** The next action to be performed by the depot */
  DepotAction           nextAction;

  /** The zone to which the action is being applied */
  ZoneCount             actingZone;

  /** Determines how slabs should be queued during load */
  SlabDepotLoadType     loadType;

  /**
   * The completion for notifying slab journals to release recovery journal
   * locks.
   */
  SequenceNumber        activeReleaseRequest;
  SequenceNumber        newReleaseRequest;
  bool                  lockReleaseActive;
  ThreadID              recoveryJournalThreadID;

  /** Whether a save has been requested */
  bool                  saveRequested;

  /** The completion for scrubbing or resizing */
  VDOCompletion         scrubbingCompletion;
  Atomic32              zonesToScrub;

  /** Cached journal pointer for slab creation */
  RecoveryJournal      *journal;

  /** Array of pointers to individually allocated slabs */
  Slab                **slabs;
  /** The number of slabs currently allocated and stored in 'slabs' */
  SlabCount             slabCount;

  /** Array of pointers to a larger set of slabs (used during resize) */
  Slab                **newSlabs;
  /** The number of slabs currently allocated and stored in 'newSlabs' */
  SlabCount             newSlabCount;
  /** The size that 'newSlabs' was allocated for */
  BlockCount            newSize;

  /** The last block before resize, for rollback */
  PhysicalBlockNumber   oldLastBlock;
  /** The last block after resize, for resize */
  PhysicalBlockNumber   newLastBlock;

  /** The block allocators for this depot */
  BlockAllocator       *allocators[];
};

/**
 * Destroy a slab.
 *
 * @param slab  The slab to destroy
 **/
void destroySlab(Slab *slab);

/**
 * Inform a slab's depot that the slab has been created.
 *
 * @param slab  The slab to register
 **/
void registerSlabWithDepot(Slab *slab);

/**
 * Notify a slab depot that one of its allocators has stopped scrubbing slabs.
 *
 * @param depot   The depot to notify
 * @param result  The result of the scrubbing operation
 **/
void notifyZoneStoppedScrubbing(SlabDepot *depot, int result);

/**
 * Check whether two depots are equivalent (i.e. represent the same
 * state and have the same reference counter). This method is used for unit
 * testing.
 *
 * @param depotA The first depot to compare
 * @param depotB The second depot to compare
 *
 * @return <code>true</code> if the two depots are equivalent
 **/
bool areEquivalentDepots(SlabDepot *depotA, SlabDepot *depotB)
  __attribute__((warn_unused_result));

/**
 * Start allocating from the highest numbered slab in each zone.
 *
 * @param depot   The depot
 **/
void allocateFromLastSlab(SlabDepot *depot);

#endif /* SLAB_DEPOT_INTERNALS_H */
