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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/allocationSelector.h#6 $
 */

#ifndef ALLOCATION_SELECTOR_H
#define ALLOCATION_SELECTOR_H

#include "completion.h"

/**
 * An allocation_selector is used by any zone which does data block allocations.
 * The selector is used to round-robin allocation requests to different
 * physical zones. Currently, 128 allocations will be made to a given physical
 * zone before switching to the next.
 **/

/**
 * Make a new allocation selector.
 *
 * @param [in]  physical_zone_count  The number of physical zones
 * @param [in]  thread_id            The ID of the thread using this selector
 * @param [out] selector_ptr         A pointer to receive the new selector
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
make_allocation_selector(zone_count_t physical_zone_count,
			 thread_id_t thread_id,
			 struct allocation_selector **selector_ptr);

/**
 * Free an allocation_selector and null out the reference to it.
 *
 * @param selector_ptr  A reference to the selector to free
 **/
void free_allocation_selector(struct allocation_selector **selector_ptr);

/**
 * Get number of the physical zone from which to allocate next.
 *
 * @param selector  The selector to query
 *
 * @return The number of the physical zone from which to allocate
 **/
zone_count_t __must_check
get_next_allocation_zone(struct allocation_selector *selector);

#endif /* ALLOCATION_SELECTOR_H */
