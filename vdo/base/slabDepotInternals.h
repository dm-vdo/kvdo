/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotInternals.h#20 $
 */

#ifndef SLAB_DEPOT_INTERNALS_H
#define SLAB_DEPOT_INTERNALS_H

#include "slabDepot.h"

#include "atomic.h"

#include "actionManager.h"

struct slab_depot {
  ZoneCount                   zoneCount;
  ZoneCount                   oldZoneCount;
  SlabConfig                  slabConfig;
  struct slab_summary        *slabSummary;
  struct read_only_notifier  *readOnlyNotifier;
  struct action_manager      *actionManager;

  PhysicalBlockNumber         firstBlock;
  PhysicalBlockNumber         lastBlock;
  PhysicalBlockNumber         origin;

  /** slabSize == (1 << slabSizeShift) */
  unsigned int                slabSizeShift;

  /** Determines how slabs should be queued during load */
  SlabDepotLoadType           loadType;

  /** The state for notifying slab journals to release recovery journal */
  SequenceNumber              activeReleaseRequest;
  SequenceNumber              newReleaseRequest;

  /** State variables for scrubbing complete handling */
  Atomic32                   *vdoState;
  Atomic32                    zonesToScrub;

  /** Cached journal pointer for slab creation */
  struct recovery_journal    *journal;

  /** Array of pointers to individually allocated slabs */
  struct vdo_slab           **slabs;
  /** The number of slabs currently allocated and stored in 'slabs' */
  SlabCount                   slabCount;

  /** Array of pointers to a larger set of slabs (used during resize) */
  struct vdo_slab           **newSlabs;
  /** The number of slabs currently allocated and stored in 'newSlabs' */
  SlabCount                   newSlabCount;
  /** The size that 'newSlabs' was allocated for */
  BlockCount                  newSize;

  /** The last block before resize, for rollback */
  PhysicalBlockNumber         oldLastBlock;
  /** The last block after resize, for resize */
  PhysicalBlockNumber         newLastBlock;

  /** The block allocators for this depot */
  struct block_allocator     *allocators[];
};

/**
 * Destroy a slab.
 *
 * @param slab  The slab to destroy
 **/
void destroySlab(struct vdo_slab *slab);

/**
 * Inform a slab's depot that the slab has been created.
 *
 * @param slab  The slab to register
 **/
void registerSlabWithDepot(struct vdo_slab *slab);

/**
 * Notify a slab depot that one of its allocators has finished scrubbing slabs.
 * This method should only be called if the scrubbing was successful. This
 * callback is registered by each block allocator in
 * scrubAllUnrecoveredSlabsInZone().
 *
 * @param completion  A completion whose parent must be a slab depot
 **/
void notifyZoneFinishedScrubbing(struct vdo_completion *completion);

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
bool areEquivalentDepots(struct slab_depot *depotA, struct slab_depot *depotB)
  __attribute__((warn_unused_result));

/**
 * Start allocating from the highest numbered slab in each zone.
 *
 * @param depot   The depot
 **/
void allocateFromLastSlab(struct slab_depot *depot);

#endif /* SLAB_DEPOT_INTERNALS_H */
