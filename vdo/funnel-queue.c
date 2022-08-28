// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "funnel-queue.h"

#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

/**
 * A funnel queue is a simple (almost) lock-free queue that accepts entries
 * from multiple threads (multi-producer) and delivers them to a single thread
 * (single-consumer). "Funnel" is an attempt to evoke the image of requests
 * from more than one producer being "funneled down" to a single consumer.
 *
 * This is an unsynchronized but thread-safe data structure when used as
 * intended. There is no mechanism to ensure that only one thread is consuming
 * from the queue. If more than one thread attempts to comsume from the queue,
 * the resulting behavior is undefined. Clients must not directly access or
 * manipulate the internals of the queue, which are only exposed for the
 * purpose of allowing the very simple enqueue operation to be inlined.
 *
 * The implementation requires that a funnel_queue_entry structure (a link
 * pointer) is embedded in the queue entries, and pointers to those structures
 * are used exclusively by the queue. No macros are defined to template the
 * queue, so the offset of the funnel_queue_entry in the records placed in the
 * queue must all be the same so the client can derive their structure pointer
 * from the entry pointer returned by funnel_queue_poll().
 *
 * Callers are wholly responsible for allocating and freeing the entries.
 * Entries may be freed as soon as they are returned since this queue is not
 * susceptible to the "ABA problem" present in many lock-free data structures.
 * The queue is dynamically allocated to ensure cache-line alignment, but no
 * other dynamic allocation is used.
 *
 * The algorithm is not actually 100% lock-free. There is a single point in
 * funnel_queue_put() at which a preempted producer will prevent the consumers
 * from seeing items added to the queue by later producers, and only if the
 * queue is short enough or the consumer fast enough for it to reach what was
 * the end of the queue at the time of the preemption.
 *
 * The consumer function, funnel_queue_poll(), will return NULL when the queue
 * is empty. To wait for data to consume, spin (if safe) or combine the queue
 * with a struct event_count to signal the presence of new entries.
 **/

int make_funnel_queue(struct funnel_queue **queue_ptr)
{
	int result;
	struct funnel_queue *queue;

	result = UDS_ALLOCATE(1, struct funnel_queue, "funnel queue", &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Initialize the stub entry and put it in the queue, establishing the
	 * invariant that queue->newest and queue->oldest are never null.
	 */
	queue->stub.next = NULL;
	queue->newest = &queue->stub;
	queue->oldest = &queue->stub;

	*queue_ptr = queue;
	return UDS_SUCCESS;
}

void free_funnel_queue(struct funnel_queue *queue)
{
	UDS_FREE(queue);
}

static struct funnel_queue_entry *get_oldest(struct funnel_queue *queue)
{
	/*
	 * Barrier requirements: We need a read barrier between reading a
	 * "next" field pointer value and reading anything it points to.
	 * There's an accompanying barrier in funnel_queue_put() between its
	 * caller setting up the entry and making it visible.
	 */
	struct funnel_queue_entry *oldest = queue->oldest;
	struct funnel_queue_entry *next = oldest->next;

	if (oldest == &queue->stub) {
		/*
		 * When the oldest entry is the stub and it has no successor,
		 * the queue is logically empty.
		 */
		if (next == NULL) {
			return NULL;
		}
		/*
		 * The stub entry has a successor, so the stub can be dequeued
		 * and ignored without breaking the queue invariants.
		 */
		oldest = next;
		queue->oldest = oldest;
		/*
		 * FIXME: Some platforms such as Alpha may require an
		 * additional barrier here.  See
		 * https://lkml.org/lkml/2019/11/8/1021
		 */
		next = oldest->next;
	}

	/*
	 * We have a non-stub candidate to dequeue. If it lacks a successor,
	 * we'll need to put the stub entry back on the queue first.
	 */
	if (next == NULL) {
		struct funnel_queue_entry *newest = queue->newest;

		if (oldest != newest) {
			/*
			 * Another thread has already swung queue->newest
			 * atomically, but not yet assigned previous->next. The
			 * queue is really still empty.
			 */
			return NULL;
		}

		/*
		 * Put the stub entry back on the queue, ensuring a successor
		 * will eventually be seen.
		 */
		funnel_queue_put(queue, &queue->stub);

		/* Check again for a successor. */
		next = oldest->next;
		if (next == NULL) {
			/*
			 * We lost a race with a producer who swapped
			 * queue->newest before we did, but who hasn't yet
			 * updated previous->next. Try again later.
			 */
			return NULL;
		}
	}

	return oldest;
}

/*
 * Poll a queue, removing the oldest entry if the queue is not empty. This
 * function must only be called from a single consumer thread.
 */
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

/*
 * Check whether the funnel queue is empty or not. If the queue is in a
 * transition state with one or more entries being added such that the list
 * view is incomplete, this function will report the queue as empty.
 */
bool is_funnel_queue_empty(struct funnel_queue *queue)
{
	return get_oldest(queue) == NULL;
}

/*
 * Check whether the funnel queue is idle or not.  If the queue has entries
 * available to be retrieved, it is not idle. If the queue is in a transition
 * state with one or more entries being added such that the list view is
 * incomplete, it may not be possible to retrieve an entry with the
 * funnel_queue_poll() function, but the queue will not be considered idle.
 */
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

	return true;
}
