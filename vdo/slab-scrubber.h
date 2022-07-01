/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_SCRUBBER_H
#define SLAB_SCRUBBER_H

#include <linux/list.h>

#include "admin-state.h"
#include "completion.h"
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
	/** The vio for loading slab journal blocks */
	struct vio *vio;
	/** A buffer to store the slab journal blocks */
	char *journal_data;
};

int __must_check
vdo_make_slab_scrubber(struct vdo *vdo,
		       block_count_t slab_journal_size,
		       struct read_only_notifier *read_only_notifier,
		       struct slab_scrubber **scrubber_ptr);

void vdo_free_slab_scrubber(struct slab_scrubber *scrubber);

void vdo_register_slab_for_scrubbing(struct slab_scrubber *scrubber,
				     struct vdo_slab *slab,
				     bool high_priority);

void vdo_scrub_slabs(struct slab_scrubber *scrubber,
		     void *parent,
		     vdo_action *callback,
		     vdo_action *error_handler);

void vdo_scrub_high_priority_slabs(struct slab_scrubber *scrubber,
				   bool scrub_at_least_one,
				   struct vdo_completion *parent,
				   vdo_action *callback,
				   vdo_action *error_handler);

void vdo_stop_slab_scrubbing(struct slab_scrubber *scrubber,
			     struct vdo_completion *parent);

void vdo_resume_slab_scrubbing(struct slab_scrubber *scrubber,
			       struct vdo_completion *parent);

int vdo_enqueue_clean_slab_waiter(struct slab_scrubber *scrubber,
				  struct waiter *waiter);

slab_count_t __must_check
vdo_get_scrubber_slab_count(const struct slab_scrubber *scrubber);

void vdo_dump_slab_scrubber(const struct slab_scrubber *scrubber);

#endif /* SLAB_SCRUBBER_H */
