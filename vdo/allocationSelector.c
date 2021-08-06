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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/allocationSelector.c#1 $
 */

#include "allocationSelector.h"
#include "allocationSelectorInternals.h"

#include "memoryAlloc.h"

#include "types.h"

enum {
	ALLOCATIONS_PER_ZONE = 128,
};

/**********************************************************************/
int make_vdo_allocation_selector(zone_count_t physical_zone_count,
				 thread_id_t thread_id,
				 struct allocation_selector **selector_ptr)
{
	struct allocation_selector *selector;
	int result = UDS_ALLOCATE(1,
				  struct allocation_selector,
				  __func__,
				  &selector);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*selector = (struct allocation_selector) {
		.next_allocation_zone = thread_id % physical_zone_count,
		.last_physical_zone = physical_zone_count - 1,
	};

	*selector_ptr = selector;
	return VDO_SUCCESS;
}

/**********************************************************************/
zone_count_t get_next_vdo_allocation_zone(struct allocation_selector *selector)
{
	if (selector->last_physical_zone > 0) {
		if (selector->allocation_count < ALLOCATIONS_PER_ZONE) {
			selector->allocation_count++;
		} else {
			selector->allocation_count = 1;
			if (selector->next_allocation_zone <
			    selector->last_physical_zone) {
				selector->next_allocation_zone++;
			} else {
				selector->next_allocation_zone = 0;
			}
		}
	}

	return selector->next_allocation_zone;
}
