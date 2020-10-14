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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotInternals.h#30 $
 */

#ifndef SLAB_DEPOT_INTERNALS_H
#define SLAB_DEPOT_INTERNALS_H

#include "slabDepot.h"

#include "atomicDefs.h"

#include "actionManager.h"

struct slab_depot {
	zone_count_t zone_count;
	zone_count_t old_zone_count;
	struct slab_config slab_config;
	struct slab_summary *slab_summary;
	struct read_only_notifier *read_only_notifier;
	struct action_manager *action_manager;

	physical_block_number_t first_block;
	physical_block_number_t last_block;
	physical_block_number_t origin;

	/** slabSize == (1 << slab_size_shift) */
	unsigned int slab_size_shift;

	/** Determines how slabs should be queued during load */
	slab_depot_load_type load_type;

	/** The state for notifying slab journals to release recovery journal */
	sequence_number_t active_release_request;
	sequence_number_t new_release_request;

	/** State variables for scrubbing complete handling */
	atomic_t *vdo_state;
	atomic_t zones_to_scrub;

	/** Cached journal pointer for slab creation */
	struct recovery_journal *journal;

	/** Array of pointers to individually allocated slabs */
	struct vdo_slab **slabs;
	/** The number of slabs currently allocated and stored in 'slabs' */
	slab_count_t slab_count;

	/** Array of pointers to a larger set of slabs (used during resize) */
	struct vdo_slab **new_slabs;
	/** The number of slabs currently allocated and stored in 'new_slabs' */
	slab_count_t new_slab_count;
	/** The size that 'new_slabs' was allocated for */
	block_count_t new_size;

	/** The last block before resize, for rollback */
	physical_block_number_t old_last_block;
	/** The last block after resize, for resize */
	physical_block_number_t new_last_block;

	/** The block allocators for this depot */
	struct block_allocator *allocators[];
};

/**
 * Destroy a slab.
 *
 * @param slab  The slab to destroy
 **/
void destroy_slab(struct vdo_slab *slab);

/**
 * Inform a slab's depot that the slab has been created.
 *
 * @param slab  The slab to register
 **/
void register_slab_with_depot(struct vdo_slab *slab);

/**
 * Notify a slab depot that one of its allocators has finished scrubbing slabs.
 * This method should only be called if the scrubbing was successful. This
 * callback is registered by each block allocator in
 * scrubAllUnrecoveredSlabsInZone().
 *
 * @param completion  A completion whose parent must be a slab depot
 **/
void notify_zone_finished_scrubbing(struct vdo_completion *completion);

/**
 * Check whether two depots are equivalent (i.e. represent the same
 * state and have the same reference counter). This method is used for unit
 * testing.
 *
 * @param depot_a The first depot to compare
 * @param depot_b The second depot to compare
 *
 * @return <code>true</code> if the two depots are equivalent
 **/
bool __must_check
are_equivalent_depots(struct slab_depot *depot_a, struct slab_depot *depot_b);

/**
 * Start allocating from the highest numbered slab in each zone.
 *
 * @param depot   The depot
 **/
void allocate_from_last_slab(struct slab_depot *depot);

#endif /* SLAB_DEPOT_INTERNALS_H */
