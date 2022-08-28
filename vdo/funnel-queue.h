/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef FUNNEL_QUEUE_H
#define FUNNEL_QUEUE_H

#include <linux/atomic.h>

#include "compiler.h"
#include "cpu.h"
#include "type-defs.h"

/* This queue link structure must be embedded in client entries. */
struct funnel_queue_entry {
	/* The next (newer) entry in the queue. */
	struct funnel_queue_entry *volatile next;
};

/*
 * The dynamically allocated queue structure, which is allocated on a cache
 * line boundary so the producer and consumer fields in the structure will land
 * on separate cache lines. This should be consider opaque but it is exposed
 * here so funnel_queue_put() can be inlined.
 */
struct __attribute__((aligned(CACHE_LINE_BYTES))) funnel_queue {
	/*
	 * The producers' end of the queue, an atomically exchanged pointer
	 * that will never be NULL.
	 */
	struct funnel_queue_entry *volatile newest;

	/*
	 * The consumer's end of the queue, which is owned by the consumer and
	 * never NULL.
	 */
	struct funnel_queue_entry *oldest
		__attribute__((aligned(CACHE_LINE_BYTES)));

	/* A dummy entry used to provide the non-NULL invariants above. */
	struct funnel_queue_entry stub;
};

int __must_check make_funnel_queue(struct funnel_queue **queue_ptr);

void free_funnel_queue(struct funnel_queue *queue);

/*
 * Put an entry on the end of the queue.
 *
 * The entry pointer must be to the struct funnel_queue_entry embedded in the
 * caller's data structure. The caller must be able to derive the address of
 * the start of their data structure from the pointer that passed in here, so
 * every entry in the queue must have the struct funnel_queue_entry at the same
 * offset within the client's structure.
 */
static INLINE void funnel_queue_put(struct funnel_queue *queue,
				    struct funnel_queue_entry *entry)
{
	struct funnel_queue_entry *previous;

	/*
	 * Barrier requirements: All stores relating to the entry ("next"
	 * pointer, containing data structure fields) must happen before the
	 * previous->next store making it visible to the consumer. Also, the
	 * entry's "next" field initialization to NULL must happen before any
	 * other producer threads can see the entry (the xchg) and try to
	 * update the "next" field.
	 *
	 * xchg implements a full barrier.
	 */
	entry->next = NULL;
	/*
	 * The xchg macro in the PPC kernel calls a function that takes a void*
	 * argument, triggering a warning about dropping the volatile
	 * qualifier.
	 */
#pragma GCC diagnostic push
#if __GNUC__ >= 5
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#endif
	previous = xchg(&queue->newest, entry);
#pragma GCC diagnostic pop
	/*
	 * Preemptions between these two statements hide the rest of the queue
	 * from the consumer, preventing consumption until the following
	 * assignment runs.
	 */
	previous->next = entry;
}

struct funnel_queue_entry *__must_check
funnel_queue_poll(struct funnel_queue *queue);

bool __must_check is_funnel_queue_empty(struct funnel_queue *queue);

bool __must_check is_funnel_queue_idle(struct funnel_queue *queue);

#endif /* FUNNEL_QUEUE_H */
