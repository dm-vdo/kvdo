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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dirty-lists.c#2 $
 */

#include "dirty-lists.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "status-codes.h"
#include "types.h"

struct dirty_lists {
	/** The number of periods after which an element will be expired */
	block_count_t maximum_age;
	/** The oldest period which has unexpired elements */
	sequence_number_t oldest_period;
	/** One more than the current period */
	sequence_number_t next_period;
	/** The function to call on expired elements */
	vdo_dirty_callback *callback;
	/** The callback context */
	void *context;
	/** The offset in the array of lists of the oldest period */
	block_count_t offset;
	/** The list of elements which are being expired */
	struct list_head expired;
	/** The lists of dirty elements */
	struct list_head lists[];
};

/**********************************************************************/
int make_vdo_dirty_lists(block_count_t maximum_age,
			 vdo_dirty_callback *callback,
			 void *context,
			 struct dirty_lists **dirty_lists_ptr)
{
	block_count_t i;
	struct dirty_lists *dirty_lists;
	int result = UDS_ALLOCATE_EXTENDED(struct dirty_lists, maximum_age,
					   struct list_head, __func__,
					   &dirty_lists);
	if (result != VDO_SUCCESS) {
		return result;
	}

	dirty_lists->maximum_age = maximum_age;
	dirty_lists->callback = callback;
	dirty_lists->context = context;

	INIT_LIST_HEAD(&dirty_lists->expired);
	for (i = 0; i < maximum_age; i++) {
		INIT_LIST_HEAD(&dirty_lists->lists[i]);
	}

	*dirty_lists_ptr = dirty_lists;
	return VDO_SUCCESS;
}

/**********************************************************************/
void set_vdo_dirty_lists_current_period(struct dirty_lists *dirty_lists,
					sequence_number_t period)
{
	ASSERT_LOG_ONLY(dirty_lists->next_period == 0, "current period not set");
	dirty_lists->oldest_period = period;
	dirty_lists->next_period = period + 1;
	dirty_lists->offset = period % dirty_lists->maximum_age;
}

/**
 * Expire the oldest list.
 *
 * @param dirty_lists  The dirty_lists to expire
 **/
static void expire_oldest_list(struct dirty_lists *dirty_lists)
{
	struct list_head *dirty_list =
		&(dirty_lists->lists[dirty_lists->offset++]);
	dirty_lists->oldest_period++;

	if (!list_empty(dirty_list)) {
		list_splice_tail(dirty_list, &dirty_lists->expired);
		INIT_LIST_HEAD(dirty_list);
	}

	if (dirty_lists->offset == dirty_lists->maximum_age) {
		dirty_lists->offset = 0;
	}
}

/**
 * Update the period if necessary.
 *
 * @param dirty_lists  The dirty_lists structure
 * @param period      The new period
 **/
static void update_period(struct dirty_lists *dirty_lists,
			  sequence_number_t period)
{
	while (dirty_lists->next_period <= period) {
		if ((dirty_lists->next_period - dirty_lists->oldest_period)
		    == dirty_lists->maximum_age) {
			expire_oldest_list(dirty_lists);
		}
		dirty_lists->next_period++;
	}
}

/**
 * Write out the expired list.
 *
 * @param dirty_lists  The dirty_lists
 **/
static void write_expired_elements(struct dirty_lists *dirty_lists)
{
	if (list_empty(&dirty_lists->expired)) {
		return;
	}

	dirty_lists->callback(&dirty_lists->expired, dirty_lists->context);
	ASSERT_LOG_ONLY(list_empty(&dirty_lists->expired),
			"no expired elements remain");
}

/**********************************************************************/
void add_to_vdo_dirty_lists(struct dirty_lists *dirty_lists,
			    struct list_head *entry,
			    sequence_number_t old_period,
			    sequence_number_t new_period)
{
	if ((old_period == new_period)
	    || ((old_period != 0) && (old_period < new_period))) {
		return;
	}

	if (new_period < dirty_lists->oldest_period) {
		list_move_tail(entry, &dirty_lists->expired);
	} else {
		update_period(dirty_lists, new_period);
		list_move_tail(entry,
			       &dirty_lists->lists[new_period %
						   dirty_lists->maximum_age]);
	}

	write_expired_elements(dirty_lists);
}

/**********************************************************************/
void advance_vdo_dirty_lists_period(struct dirty_lists *dirty_lists,
				    sequence_number_t period)
{
	update_period(dirty_lists, period);
	write_expired_elements(dirty_lists);
}

/**********************************************************************/
void flush_vdo_dirty_lists(struct dirty_lists *dirty_lists)
{
	while (dirty_lists->oldest_period < dirty_lists->next_period) {
		expire_oldest_list(dirty_lists);
	}
	write_expired_elements(dirty_lists);
}
