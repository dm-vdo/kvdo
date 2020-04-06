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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/workQueueInternals.h#9 $
 */

#ifndef WORK_QUEUE_INTERNALS_H
#define WORK_QUEUE_INTERNALS_H

#include <linux/completion.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "workItemStats.h"
#include "workQueueStats.h"

struct kvdo_work_item_list {
	struct kvdo_work_item *tail;
};

/**
 * Work queue definition.
 *
 * There are two types of work queues: simple, with one worker thread, and
 * round-robin, which uses a group of the former to do the work, and assigns
 * work to them in round-robin fashion (roughly). Externally, both are
 * represented via the same common sub-structure, though there's actually not a
 * great deal of overlap between the two types internally.
 **/
struct kvdo_work_queue {
	/** Name of just the work queue (e.g., "cpuQ12") */
	char *name;
	/**
	 * Whether this is a round-robin work queue or a simple (one-thread)
	 * work queue.
	 **/
	bool round_robin_mode;
	/** A handle to a sysfs tree for reporting stats and other info */
	struct kobject kobj;
	/** The kernel layer owning this work queue */
	struct kernel_layer *owner;
};

struct simple_work_queue {
	/** Common work queue bits */
	struct kvdo_work_queue common;
	/** A copy of .thread->pid, for safety in the sysfs support */
	atomic_t thread_id;
	/**
	 * Number of priorities actually used, so we don't keep re-checking
	 * unused funnel queues.
	 **/
	unsigned int num_priority_lists;
	/**
	 * Map from action codes to priorities.
	 *
	 * This mapping can be changed at run time in internal builds, for
	 * tuning purposes.
	 **/
	uint8_t priority_map[WORK_QUEUE_ACTION_COUNT];
	/** The funnel queues */
	FunnelQueue *priority_lists[WORK_QUEUE_PRIORITY_COUNT];
	/** The kernel thread */
	struct task_struct *thread;
	/** Life cycle functions, etc */
	const struct kvdo_work_queue_type *type;
	/** Opaque private data pointer, defined by higher level code */
	void *private;
	/** In a subordinate work queue, a link back to the round-robin parent
	 */
	struct kvdo_work_queue *parent_queue;
	/** Padding for cache line separation */
	char pad[CACHE_LINE_BYTES - sizeof(struct kvdo_work_queue *)];
	/**
	 * Lock protecting priority_map, num_priority_lists, started
	 */
	spinlock_t lock;
	/** Any worker threads (zero or one) waiting for new work to do */
	wait_queue_head_t waiting_worker_threads;
	/**
	 * Hack to reduce wakeup calls if the worker thread is running. See
	 * comments in workQueue.c.
	 *
	 * There is a lot of redundancy with "first_wakeup", though, and the
	 * pair should be re-examined.
	 **/
	atomic_t idle;
	/** Wait list for synchronization during worker thread startup */
	wait_queue_head_t start_waiters;
	/** Worker thread status (boolean) */
	bool started;

	/**
	 * Timestamp (ns) from the submitting thread that decided to wake us
	 * up; also used as a flag to indicate whether a wakeup is needed.
	 *
	 * Written by submitting threads with atomic64_cmpxchg, and by the
	 * worker thread setting to 0.
	 *
	 * If the value is 0, the worker is probably asleep; the submitting
	 * thread stores a non-zero value and becomes responsible for calling
	 * wake_up on the worker thread. If the value is non-zero, either the
	 * worker is running or another thread has the responsibility for
	 * issuing the wakeup.
	 *
	 * The "sleep" mode has periodic wakeups and the worker thread may
	 * happen to wake up while a work item is being enqueued. If that
	 * happens, the wakeup may be unneeded but will be attempted anyway.
	 *
	 * So the return value from cmpxchg(first_wakeup,0,nonzero) can always
	 * be done, and will tell the submitting thread whether to issue the
	 * wakeup or not; cmpxchg is atomic, so no other synchronization is
	 * needed.
	 *
	 * A timestamp is used rather than, say, 1, so that the worker thread
	 * can record stats on how long it takes to actually get the worker
	 * thread running.
	 *
	 * There is some redundancy between this and "idle" above.
	 **/
	atomic64_t first_wakeup;
	/** Padding for cache line separation */
	char pad2[CACHE_LINE_BYTES - sizeof(atomic64_t)];
	/** Scheduling and work-function statistics */
	struct kvdo_work_queue_stats stats;
	/** Last time (ns) the scheduler actually woke us up */
	uint64_t most_recent_wakeup;
};

struct round_robin_work_queue {
	/** Common work queue bits */
	struct kvdo_work_queue common;
	/** Simple work queues, for actually getting stuff done */
	struct simple_work_queue **service_queues;
	/** Number of subordinate work queues */
	unsigned int num_service_queues;
};

static inline struct simple_work_queue *
as_simple_work_queue(struct kvdo_work_queue *queue)
{
	return ((queue == NULL) ?
		 NULL :
		 container_of(queue, struct simple_work_queue, common));
}

static inline const struct simple_work_queue *
as_const_simple_work_queue(const struct kvdo_work_queue *queue)
{
	return ((queue == NULL) ?
		 NULL :
		 container_of(queue, struct simple_work_queue, common));
}

static inline struct round_robin_work_queue *
as_round_robin_work_queue(struct kvdo_work_queue *queue)
{
	return ((queue == NULL) ?
		 NULL :
		 container_of(queue, struct round_robin_work_queue, common));
}

#endif // WORK_QUEUE_INTERNALS_H
