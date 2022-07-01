/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DIRTY_LISTS_H
#define DIRTY_LISTS_H

#include <linux/list.h>

#include "types.h"

/**
 * struct dirty_lists - Lists of dirty elements.
 *
 * A collection of lists of dirty elements ordered by age. An element is always
 * placed on the oldest list in which it was dirtied (moving between lists or
 * removing altogether is cheap). Whenever the current period is advanced, any
 * elements older than the maxium age are expired. If an element is to be added
 * with a dirty age older than the maximum age, it is expired immediately.
 */
struct dirty_lists;

/**
 * typedef vdo_dirty_callback - Callback for processing dirty elements.
 * @expired: The list of expired elements.
 * @context: The context for the callback.
 *
 * A function which will be called with a ring of dirty elements which have
 * been expired. All of the expired elements must be removed from the ring
 * before this function returns.
 */
typedef void vdo_dirty_callback(struct list_head *expired, void *context);

int __must_check vdo_make_dirty_lists(block_count_t maximum_age,
				      vdo_dirty_callback *callback,
				      void *context,
				      struct dirty_lists **dirty_lists_ptr);

void vdo_set_dirty_lists_current_period(struct dirty_lists *dirty_lists,
					sequence_number_t period);

void vdo_add_to_dirty_lists(struct dirty_lists *dirty_lists,
			    struct list_head *entry,
			    sequence_number_t old_period,
			    sequence_number_t new_period);

void vdo_advance_dirty_lists_period(struct dirty_lists *dirty_lists,
				    sequence_number_t period);

void vdo_flush_dirty_lists(struct dirty_lists *dirty_lists);

#endif /* DIRTY_LISTS_H */
