/*
 * Copyright Red Hat
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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/slabDepotInternals.h#1 $
 */

#ifndef SLAB_DEPOT_INTERNALS_H
#define SLAB_DEPOT_INTERNALS_H

#include "slabDepot.h"

#include <linux/atomic.h>

#include "types.h"

struct slab_depot {
	zone_count_t zone_count;
	zone_count_t old_zone_count;
	struct vdo *vdo;
	struct slab_config slab_config;
	struct slab_summary *slab_summary;
	struct action_manager *action_manager;

	physical_block_number_t first_block;
	physical_block_number_t last_block;
	physical_block_number_t origin;

	/** slab_size == (1 << slab_size_shift) */
	unsigned int slab_size_shift;

	/** Determines how slabs should be queued during load */
	enum slab_depot_load_type load_type;

	/** The state for notifying slab journals to release recovery journal */
	sequence_number_t active_release_request;
	sequence_number_t new_release_request;

	/** State variables for scrubbing complete handling */
	atomic_t zones_to_scrub;

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
 * Notify a slab depot that one of its allocators has finished scrubbing slabs.
 * This method should only be called if the scrubbing was successful. This
 * callback is registered by each block allocator in
 * scrub_all_unrecovered_vdo_slabs_in_zone().
 *
 * @param completion  A completion whose parent must be a slab depot
 **/
void vdo_notify_zone_finished_scrubbing(struct vdo_completion *completion);


#endif /* SLAB_DEPOT_INTERNALS_H */
