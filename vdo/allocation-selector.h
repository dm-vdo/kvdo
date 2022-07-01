/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef ALLOCATION_SELECTOR_H
#define ALLOCATION_SELECTOR_H

#include "completion.h"

/**
 * DOC: Allocation selectors
 *
 * An allocation_selector is used by any zone which does data block allocations.
 * The selector is used to round-robin allocation requests to different
 * physical zones. Currently, 128 allocations will be made to a given physical
 * zone before switching to the next.
 */

/**
 * struct allocation_selector: Structure used to select which physical zone to
 *                             allocate from.
 */
struct allocation_selector {
	/**
	 * @allocation_count: The number of allocations done in the current
	 *                    zone.
	 */
	block_count_t allocation_count;
	/** @next_allocation_zone: The physical zone to allocate from next. */
	zone_count_t next_allocation_zone;
	/** @last_physical_cone: The number of the last physical zone. */
	zone_count_t last_physical_zone;
};

int __must_check
vdo_make_allocation_selector(zone_count_t physical_zone_count,
			     thread_id_t thread_id,
			     struct allocation_selector **selector_ptr);

zone_count_t __must_check
vdo_get_next_allocation_zone(struct allocation_selector *selector);

#endif /* ALLOCATION_SELECTOR_H */
