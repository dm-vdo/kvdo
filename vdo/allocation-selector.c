// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "allocation-selector.h"

#include "memory-alloc.h"

#include "types.h"

enum {
	ALLOCATIONS_PER_ZONE = 128,
};

/**
 * vdo_make_allocation_selector() - Make a new allocation selector.
 * @physical_zone_count [in] The number of physical zones.
 * @thread_id [in] The ID of the thread using this selector.
 * @selector_ptr [out] A pointer to receive the new selector.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_allocation_selector(zone_count_t physical_zone_count,
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

/**
 * vdo_get_next_allocation_zone() - Get number of the physical zone from
 *                                  which to allocate next.
 * @selector: The selector to query.
 *
 * Return: The number of the physical zone from which to allocate.
 */
zone_count_t vdo_get_next_allocation_zone(struct allocation_selector *selector)
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
