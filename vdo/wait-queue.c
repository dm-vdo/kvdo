// SPDX-License-Identifier: GPL-2.0-only
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

#include "wait-queue.h"

#include "permassert.h"

#include "status-codes.h"

/**
 * Add a waiter to the tail end of a wait queue. The waiter must not already
 * be waiting in a queue.
 *
 * @param queue     The queue to which to add the waiter
 * @param waiter    The waiter to add to the queue
 *
 * @return VDO_SUCCESS or an error code
 **/
int enqueue_waiter(struct wait_queue *queue, struct waiter *waiter)
{
	int result = ASSERT((waiter->next_waiter == NULL),
			    "new waiter must not already be in a waiter queue");
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (queue->last_waiter == NULL) {
		/*
		 * The queue is empty, so form the initial circular list by
		 * self-linking the initial waiter.
		 */
		waiter->next_waiter = waiter;
	} else {
		/* Splice the new waiter in at the end of the queue. */
		waiter->next_waiter = queue->last_waiter->next_waiter;
		queue->last_waiter->next_waiter = waiter;
	}
	/*
	 * In both cases, the waiter we added to the ring becomes the last
	 * waiter.
	 */
	queue->last_waiter = waiter;
	queue->queue_length += 1;
	return VDO_SUCCESS;
}

/**
 * Transfer all waiters from one wait queue to a second queue, emptying the
 * first queue.
 *
 * @param from_queue  The queue containing the waiters to move
 * @param to_queue    The queue that will receive the waiters from the
 *                    first queue
 **/
void transfer_all_waiters(struct wait_queue *from_queue,
			  struct wait_queue *to_queue)
{
	/* If the source queue is empty, there's nothing to do. */
	if (!has_waiters(from_queue)) {
		return;
	}

	if (has_waiters(to_queue)) {
		/*
		 * Both queues are non-empty. Splice the two circular lists
		 * together by swapping the next (head) pointers in the list
		 * tails.
		 */
		struct waiter *from_head = from_queue->last_waiter->next_waiter;
		struct waiter *to_head = to_queue->last_waiter->next_waiter;

		to_queue->last_waiter->next_waiter = from_head;
		from_queue->last_waiter->next_waiter = to_head;
	}

	to_queue->last_waiter = from_queue->last_waiter;
	to_queue->queue_length += from_queue->queue_length;
	initialize_wait_queue(from_queue);
}

/**
 * Notify all the entries waiting in a queue to continue execution by invoking
 * a callback function on each of them in turn. The queue is copied and
 * emptied before invoking any callbacks, and only the waiters that were in
 * the queue at the start of the call will be notified.
 *
 * @param queue     The wait queue containing the waiters to notify
 * @param callback  The function to call to notify each waiter, or NULL
 *                  to invoke the callback field registered in each waiter
 * @param context   The context to pass to the callback function
 **/
void notify_all_waiters(struct wait_queue *queue, waiter_callback *callback,
			void *context)
{
	/*
	 * Copy and empty the queue first, avoiding the possibility of an
	 * infinite loop if entries are returned to the queue by the callback
	 * function.
	 */
	struct wait_queue waiters;

	initialize_wait_queue(&waiters);
	transfer_all_waiters(queue, &waiters);

	/* Drain the copied queue, invoking the callback on every entry. */
	while (notify_next_waiter(&waiters, callback, context)) {
		/* All the work is done by the loop condition. */
	}
}

/**
 * Return the waiter that is at the head end of a wait queue.
 *
 * @param queue  The queue from which to get the first waiter
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
struct waiter *get_first_waiter(const struct wait_queue *queue)
{
	struct waiter *last_waiter = queue->last_waiter;

	if (last_waiter == NULL) {
		/* There are no waiters, so we're done. */
		return NULL;
	}

	/*
	 * The queue is circular, so the last entry links to the head of the
	 * queue.
	 */
	return last_waiter->next_waiter;
}

/**
 * Remove all waiters that match based on the specified matching method and
 * append them to a wait_queue.
 *
 * @param queue          The wait queue to process
 * @param match_method   The method to determine matching
 * @param match_context  Contextual info for the match method
 * @param matched_queue  A wait_queue to store matches
 *
 * @return VDO_SUCCESS or an error code
 **/
int dequeue_matching_waiters(struct wait_queue *queue,
			     waiter_match *match_method,
			     void *match_context,
			     struct wait_queue *matched_queue)
{
	struct wait_queue matched_waiters, iteration_queue;

	initialize_wait_queue(&matched_waiters);

	initialize_wait_queue(&iteration_queue);
	transfer_all_waiters(queue, &iteration_queue);
	while (has_waiters(&iteration_queue)) {
		struct waiter *waiter = dequeue_next_waiter(&iteration_queue);
		int result = VDO_SUCCESS;

		if (!match_method(waiter, match_context)) {
			result = enqueue_waiter(queue, waiter);
		} else {
			result = enqueue_waiter(&matched_waiters, waiter);
		}
		if (result != VDO_SUCCESS) {
			transfer_all_waiters(&matched_waiters, queue);
			transfer_all_waiters(&iteration_queue, queue);
			return result;
		}
	}

	transfer_all_waiters(&matched_waiters, matched_queue);
	return VDO_SUCCESS;
}

/**
 * Remove the first waiter from the head end of a wait queue. The caller will
 * be responsible for waking the waiter by invoking the correct callback
 * function to resume its execution.
 *
 * @param queue  The wait queue from which to remove the first entry
 *
 * @return The first (oldest) waiter in the queue, or <code>NULL</code> if
 *         the queue is empty
 **/
struct waiter *dequeue_next_waiter(struct wait_queue *queue)
{
	struct waiter *first_waiter = get_first_waiter(queue);
	struct waiter *last_waiter = queue->last_waiter;

	if (first_waiter == NULL) {
		return NULL;
	}

	if (first_waiter == last_waiter) {
		/*
		 * The queue has a single entry, so just empty it out by nulling
		 * the tail.
		 */
		queue->last_waiter = NULL;
	} else {
		/*
		 * The queue has more than one entry, so splice the first waiter
		 * out of the circular queue.
		 */
		last_waiter->next_waiter = first_waiter->next_waiter;
	}

	/* The waiter is no longer in a wait queue. */
	first_waiter->next_waiter = NULL;
	queue->queue_length -= 1;
	return first_waiter;
}

/**
 * Notify the next entry waiting in a queue to continue execution by invoking
 * a callback function on it after removing it from the queue.
 *
 * @param queue     The wait queue containing the waiter to notify
 * @param callback  The function to call to notify the waiter, or NULL
 *                  to invoke the callback field registered in the waiter
 * @param context   The context to pass to the callback function
 *
 * @return <code>true</code> if there was a waiter in the queue
 **/
bool notify_next_waiter(struct wait_queue *queue, waiter_callback *callback,
			void *context)
{
	struct waiter *waiter = dequeue_next_waiter(queue);

	if (waiter == NULL) {
		return false;
	}

	if (callback == NULL) {
		callback = waiter->callback;
	}
	(*callback)(waiter, context);
	return true;
}

/**
 * Get the waiter after this one, for debug iteration.
 *
 * @param queue   The wait queue
 * @param waiter  A waiter
 *
 * @return the next waiter, or NULL
 **/
const struct waiter *get_next_waiter(const struct wait_queue *queue,
				     const struct waiter *waiter)
{
	struct waiter *first_waiter = get_first_waiter(queue);

	if (waiter == NULL) {
		return first_waiter;
	}
	return ((waiter->next_waiter != first_waiter) ? waiter->next_waiter
						    : NULL);
}
