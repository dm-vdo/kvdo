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
 */

#ifndef SLAB_SCRUBBER_H
#define SLAB_SCRUBBER_H

#include <linux/list.h>

#include "admin-state.h"
#include "completion.h"
#include "extent.h"
#include "types.h"
#include "wait-queue.h"

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

int __must_check
make_vdo_slab_scrubber(struct vdo *vdo,
		       block_count_t slab_journal_size,
		       struct read_only_notifier *read_only_notifier,
		       struct slab_scrubber **scrubber_ptr);

void free_vdo_slab_scrubber(struct slab_scrubber *scrubber);

void vdo_register_slab_for_scrubbing(struct slab_scrubber *scrubber,
				     struct vdo_slab *slab,
				     bool high_priority);

void scrub_vdo_slabs(struct slab_scrubber *scrubber,
		     void *parent,
		     vdo_action *callback,
		     vdo_action *error_handler);

void scrub_high_priority_vdo_slabs(struct slab_scrubber *scrubber,
				   bool scrub_at_least_one,
				   struct vdo_completion *parent,
				   vdo_action *callback,
				   vdo_action *error_handler);

void stop_vdo_slab_scrubbing(struct slab_scrubber *scrubber,
			     struct vdo_completion *parent);

void resume_vdo_slab_scrubbing(struct slab_scrubber *scrubber,
			       struct vdo_completion *parent);

int enqueue_clean_vdo_slab_waiter(struct slab_scrubber *scrubber,
				  struct waiter *waiter);

slab_count_t __must_check
get_scrubber_vdo_slab_count(const struct slab_scrubber *scrubber);

void dump_vdo_slab_scrubber(const struct slab_scrubber *scrubber);

#endif /* SLAB_SCRUBBER_H */
