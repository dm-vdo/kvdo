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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/dirtyLists.h#9 $
 */

#ifndef DIRTY_LISTS_H
#define DIRTY_LISTS_H

#include <linux/list.h>

#include "types.h"

/**
 * A collection of lists of dirty elements ordered by age. An element is always
 * placed on the oldest list in which it was dirtied (moving between lists or
 * removing altogether is cheap). Whenever the current period is advanced, any
 * elements older than the maxium age are expired. If an element is to be added
 * with a dirty age older than the maximum age, it is expired immediately.
 **/
struct dirty_lists;

/**
 * A function which will be called with a ring of dirty elements which have
 * been expired. All of the expired elements must be removed from the ring
 * before this function returns.
 *
 * @param expired  The list of expired elements
 * @param context  The context for the callback
 **/
typedef void dirty_callback(struct list_head *expired, void *context);

/**
 * Construct a new set of dirty lists.
 *
 * @param [in]  maximum_age      The age at which an element will be expired
 * @param [in]  callback         The function to call when a set of elements
 *                               have expired
 * @param [in]  context          The context for the callback
 * @param [out] dirty_lists_ptr  A pointer to hold the new dirty_lists structure
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_dirty_lists(block_count_t maximum_age,
				  dirty_callback *callback,
				  void *context,
				  struct dirty_lists **dirty_lists_ptr);

/**
 * Free a set of dirty lists and null out the pointer to them.
 *
 * @param dirty_lists_ptr A pointer to the dirty lists to be freed
 **/
void free_dirty_lists(struct dirty_lists **dirty_lists_ptr);

/**
 * Set the current period. This function should only be called once.
 *
 * @param dirty_lists  The dirty_lists
 * @param period       The current period
 **/
void set_current_period(struct dirty_lists *dirty_lists,
			sequence_number_t period);

/**
 * Add an element to the dirty lists.
 *
 * @param dirty_lists  The dirty_lists structure receiving the element
 * @param entry        The list entry of the element to add
 * @param old_period   The period in which the element was previous dirtied,
 *                     or 0 if it was not dirty
 * @param new_period   The period in which the element has now been dirtied,
 *                     or 0 if it does not hold a lock
 **/
void add_to_dirty_lists(struct dirty_lists *dirty_lists,
			struct list_head *entry,
			sequence_number_t old_period,
			sequence_number_t new_period);

/**
 * Advance the current period. If the current period is greater than the number
 * of lists, expire the oldest lists.
 *
 * @param dirty_lists  The dirty_lists to advance
 * @param period       The new current period
 **/
void advance_period(struct dirty_lists *dirty_lists, sequence_number_t period);

/**
 * Flush all dirty lists. This will cause the period to be advanced past the
 * current period.
 *
 * @param dirty_lists  The dirty_lists to flush
 **/
void flush_dirty_lists(struct dirty_lists *dirty_lists);

#endif // DIRTY_LISTS_H
