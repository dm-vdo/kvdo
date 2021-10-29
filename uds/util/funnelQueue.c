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

#include "funnelQueue.h"

#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/**********************************************************************/
int make_funnel_queue(struct funnel_queue **queue_ptr)
{
	// Allocate the queue on a cache line boundary so the producer and
	// consumer fields in the structure will land on separate cache lines.
	struct funnel_queue *queue;
	int result = UDS_ALLOCATE(1, struct funnel_queue, "funnel queue",
				  &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Initialize the stub entry and put it in the queue, establishing the
	// invariant that queue->newest and queue->oldest are never null.
	queue->stub.next = NULL;
	queue->newest = &queue->stub;
	queue->oldest = &queue->stub;

	*queue_ptr = queue;
	return UDS_SUCCESS;
}

/**********************************************************************/
void free_funnel_queue(struct funnel_queue *queue)
{
	UDS_FREE(queue);
}

/**********************************************************************/
static struct funnel_queue_entry *get_oldest(struct funnel_queue *queue)
{
	/*
	 * Barrier requirements: We need a read barrier between reading a
	 * "next" field pointer value and reading anything it points to.
	 * There's an accompanying barrier in funnel_queue_put between its
	 * caller setting up the entry and making it visible.
	 */
	struct funnel_queue_entry *oldest = queue->oldest;
	struct funnel_queue_entry *next = oldest->next;

	if (oldest == &queue->stub) {
		// When the oldest entry is the stub and it has no successor,
		// the queue is logically empty.
		if (next == NULL) {
			return NULL;
		}
		// The stub entry has a successor, so the stub can be dequeued
		// and ignored without breaking the queue invariants.
		oldest = next;
		queue->oldest = oldest;
		// XXX Some platforms such as Alpha may require an
		// additional barrier here.  See
		// https://lkml.org/lkml/2019/11/8/1021
		next = oldest->next;
	}

	// We have a non-stub candidate to dequeue. If it lacks a successor,
	// we'll need to put the stub entry back on the queue first.
	if (next == NULL) {
		struct funnel_queue_entry *newest = queue->newest;
		if (oldest != newest) {
			// Another thread has already swung queue->newest
			// atomically, but not yet assigned previous->next. The
			// queue is really still empty.
			return NULL;
		}

		// Put the stub entry back on the queue, ensuring a successor
		// will eventually be seen.
		funnel_queue_put(queue, &queue->stub);

		// Check again for a successor.
		next = oldest->next;
		if (next == NULL) {
			// We lost a race with a producer who swapped
			// queue->newest before we did, but who hasn't yet
			// updated previous->next. Try again later.
			return NULL;
		}
	}
	return oldest;
}

/**********************************************************************/
struct funnel_queue_entry *funnel_queue_poll(struct funnel_queue *queue)
{
	struct funnel_queue_entry *oldest = get_oldest(queue);
	if (oldest == NULL) {
		return oldest;
	}

	/*
	 * Dequeue the oldest entry and return it. Only one consumer thread may
	 * call this function, so no locking, atomic operations, or fences are
	 * needed; queue->oldest is owned by the consumer and oldest->next is
	 * never used by a producer thread after it is swung from NULL to
	 * non-NULL.
	 */
	queue->oldest = oldest->next;
	/*
	 * Make sure the caller sees the proper stored data for this entry.
	 *
	 * Since we've already fetched the entry pointer we stored in
	 * "queue->oldest", this also ensures that on entry to the next call
	 * we'll properly see the dependent data.
	 */
	smp_rmb();
	/*
	 * If "oldest" is a very light-weight work item, we'll be looking
	 * for the next one very soon, so prefetch it now.
	 */
	prefetch_address(queue->oldest, true);
	oldest->next = NULL;
	return oldest;
}

/**********************************************************************/
bool is_funnel_queue_empty(struct funnel_queue *queue)
{
	return get_oldest(queue) == NULL;
}

/**********************************************************************/
bool is_funnel_queue_idle(struct funnel_queue *queue)
{
	/*
	 * Oldest is not the stub, so there's another entry, though if next is
	 * NULL we can't retrieve it yet.
	 */
	if (queue->oldest != &queue->stub) {
		return false;
	}

	/*
	 * Oldest is the stub, but newest has been updated by _put(); either
	 * there's another, retrievable entry in the list, or the list is
	 * officially empty but in the intermediate state of having an entry
	 * added.
	 *
	 * Whether anything is retrievable depends on whether stub.next has
	 * been updated and become visible to us, but for idleness we don't
	 * care. And due to memory ordering in _put(), the update to newest
	 * would be visible to us at the same time or sooner.
	 */
	if (queue->newest != &queue->stub) {
		return false;
	}

	// Otherwise, we're idle.
	return true;
}
