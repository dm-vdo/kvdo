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
 * $Id: //eng/vdo-releases/sulfur-rhel9.0-beta/src/c++/vdo/base/slabScrubberInternals.h#1 $
 */

#ifndef SLAB_SCRUBBER_INTERNALS_H
#define SLAB_SCRUBBER_INTERNALS_H

#include <linux/list.h>

#include "slabScrubber.h"

#include "adminState.h"
#include "extent.h"

struct slab_scrubber {
	struct vdo_completion completion;
	/** The queue of slabs to scrub first */
	struct list_head high_priority_slabs;
	/** The queue of slabs to scrub once there are no high_priority_slabs */
	struct list_head slabs;
	/** The queue of VIOs waiting for a slab to be scrubbed */
	struct wait_queue waiters;

	/*
	 * The number of slabs that are unrecovered or being scrubbed. This
	 * field is modified by the physical zone thread, but is queried by
	 * other threads.
	 */
	slab_count_t slab_count;

	/** The administrative state of the scrubber */
	struct admin_state admin_state;
	/** Whether to only scrub high-priority slabs */
	bool high_priority_only;
	/** The context for entering read-only mode */
	struct read_only_notifier *read_only_notifier;
	/** The slab currently being scrubbed */
	struct vdo_slab *slab;
	/** The extent for loading slab journal blocks */
	struct vdo_extent *extent;
	/** A buffer to store the slab journal blocks */
	char *journal_data;
};

#endif // SLAB_SCRUBBER_INTERNALS_H
