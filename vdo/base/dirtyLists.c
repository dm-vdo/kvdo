/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dirtyLists.c#5 $
 */

#include "dirtyLists.h"
#include "dirtyListsInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "types.h"

struct dirty_lists {
	/** The number of periods after which an element will be expired */
	block_count_t maximum_age;
	/** The oldest period which has unexpired elements */
	SequenceNumber oldest_period;
	/** One more than the current period */
	SequenceNumber next_period;
	/** The function to call on expired elements */
	dirty_callback *callback;
	/** The callback context */
	void *context;
	/** The offset in the array of lists of the oldest period */
	block_count_t offset;
	/** The list of elements which are being expired */
	RingNode expired;
	/** The lists of dirty elements */
	RingNode lists[];
};

/**********************************************************************/
int make_dirty_lists(block_count_t maximum_age, dirty_callback *callback,
		     void *context, struct dirty_lists **dirty_lists_ptr)
{
	struct dirty_lists *dirty_lists;
	int result = ALLOCATE_EXTENDED(struct dirty_lists, maximum_age,
				       RingNode, __func__, &dirty_lists);
	if (result != VDO_SUCCESS) {
		return result;
	}

	dirty_lists->maximum_age = maximum_age;
	dirty_lists->callback = callback;
	dirty_lists->context = context;

	initializeRing(&dirty_lists->expired);
	block_count_t i;
	for (i = 0; i < maximum_age; i++) {
		initializeRing(&dirty_lists->lists[i]);
	}

	*dirty_lists_ptr = dirty_lists;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_dirty_lists(struct dirty_lists **dirty_lists_ptr)
{
	struct dirty_lists *lists = *dirty_lists_ptr;
	if (lists == NULL) {
		return;
	}

	FREE(lists);
	*dirty_lists_ptr = NULL;
}

/**********************************************************************/
void set_current_period(struct dirty_lists *dirty_lists, SequenceNumber period)
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
	dirty_lists->oldest_period++;
	RingNode *ring = &(dirty_lists->lists[dirty_lists->offset++]);
	if (!isRingEmpty(ring)) {
		spliceRingChainBefore(ring->next, ring->prev,
				      &dirty_lists->expired);
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
			  SequenceNumber period)
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
	if (isRingEmpty(&dirty_lists->expired)) {
		return;
	}

	dirty_lists->callback(&dirty_lists->expired, dirty_lists->context);
	ASSERT_LOG_ONLY(isRingEmpty(&dirty_lists->expired),
			"no expired elements remain");
}

/**********************************************************************/
void add_to_dirty_lists(struct dirty_lists *dirty_lists, RingNode *node,
			SequenceNumber old_period, SequenceNumber new_period)
{
	if ((old_period == new_period)
	    || ((old_period != 0) && (old_period < new_period))) {
		return;
	}

	if (new_period < dirty_lists->oldest_period) {
		pushRingNode(&dirty_lists->expired, node);
	} else {
		update_period(dirty_lists, new_period);
		pushRingNode(
			&dirty_lists->lists[new_period %
					    dirty_lists->maximum_age],
			node);
	}

	write_expired_elements(dirty_lists);
}

/**********************************************************************/
void advance_period(struct dirty_lists *dirty_lists, SequenceNumber period)
{
	update_period(dirty_lists, period);
	write_expired_elements(dirty_lists);
}

/**********************************************************************/
void flush_dirty_lists(struct dirty_lists *dirty_lists)
{
	while (dirty_lists->oldest_period < dirty_lists->next_period) {
		expire_oldest_list(dirty_lists);
	}
	write_expired_elements(dirty_lists);
}

/**********************************************************************/
SequenceNumber get_dirty_lists_next_period(struct dirty_lists *dirty_lists)
{
	return dirty_lists->next_period;
}
