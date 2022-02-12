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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/batchProcessor.c#19 $
 */

#include "batchProcessor.h"

#include <linux/atomic.h>

#include "memoryAlloc.h"

#include "constants.h"
#include "vdoInternal.h"

#include "kernelLayer.h"

/*
 * On memory ordering:
 *
 * The producer thread does: enqueue item on queue (xchg, which is
 * implicitly interlocked, then a store), memory barrier, then atomic
 * cmpxchg of the state field. The x86 architecture spec says the
 * xchg, store, lock-cmpxchg sequence cannot be reordered, but on
 * architectures using load-linked and store-conditional for the
 * cmpxchg, like AArch64, the LL can be reordered with the store, so
 * we add a barrier.
 *
 * The consumer thread, when it is running out of work, does: read
 * queue (find empty), set state, mfence, read queue again just to be
 * sure. The set-state and read-queue cannot be reordered with respect
 * to the mfence (but without the mfence, the read could be moved
 * before the set).
 *
 * The xchg and mfence impose a total order across processors, and
 * each processor sees the stores done by the other processor in the
 * required order. If the xchg happens before the mfence, the
 * consumer's "read queue again" operation will see the update. If the
 * mfence happens first, the producer's "cmpxchg state" will see its
 * updated value.
 *
 * These are the semantics implemented by memory set to WB (write-back
 * caching) mode on x86-64. So, the simple analysis is that no wakeups
 * should be missed.
 *
 * It's a little subtler with funnel queues, since one interrupted or
 * delayed enqueue operation (see the commentary in funnel_queue_put)
 * can cause another, concurrent enqueue operation to complete without
 * actually making the entry visible to the consumer. In essence, one
 * update makes no new work items visible to the consumer, and the
 * other (when it eventually completes) makes two (or more) work items
 * visible, and each one ensures that the consumer will process what
 * it has made visible.
 */

enum batch_processor_state {
	BATCH_PROCESSOR_IDLE,
	BATCH_PROCESSOR_ENQUEUED,
};

struct batch_processor {
	struct mutex consumer_mutex;
	struct funnel_queue *queue;
	struct vdo_work_item work_item;
	atomic_t state;
	batch_processor_callback callback;
	void *closure;
	struct vdo *vdo;
};

static void schedule_batch_processing(struct batch_processor *batch);

/**
 * Apply the batch processing function to the accumulated set of
 * objects.
 *
 * Runs in a "CPU queue".
 *
 * @param [in]  item  The work item embedded in the batch_processor
 **/
static void batch_processor_work(struct vdo_work_item *item)
{
	struct batch_processor *batch =
		container_of(item, struct batch_processor, work_item);
	bool need_reschedule;

	mutex_lock(&batch->consumer_mutex);
	while (!is_funnel_queue_empty(batch->queue)) {
		batch->callback(batch, batch->closure);
	}
	atomic_set(&batch->state, BATCH_PROCESSOR_IDLE);
	// Pairs with the barrier in schedule_batch_processing(); see header
	// comment on memory ordering.
	smp_mb();
	need_reschedule = !is_funnel_queue_empty(batch->queue);

	mutex_unlock(&batch->consumer_mutex);
	if (need_reschedule) {
		schedule_batch_processing(batch);
	}
}

/**
 * Ensure that the batch-processing function is scheduled to run.
 *
 * If we're the thread that switches the batch_processor state from
 * idle to enqueued, we're the thread responsible for actually
 * enqueueing it. If some other thread got there first, or it was
 * already enqueued, it's not our problem.
 *
 * @param [in]  batch  The batch_processor control data
 **/
static void schedule_batch_processing(struct batch_processor *batch)
{
	enum batch_processor_state old_state;
	bool do_schedule;

	/*
	 * We want this to be very fast in the common cases.
	 *
	 * In testing on our "mgh" class machines (HP ProLiant DL380p
	 * Gen8, Intel Xeon E5-2690, 2.9GHz), it appears that under
	 * some conditions it's a little faster to use a memory fence
	 * and then read the "state" field, skipping the cmpxchg if
	 * the state is already set to BATCH_PROCESSOR_ENQUEUED.
	 * (Sometimes slightly faster still if we prefetch the state
	 * field first.) Note that the read requires the fence,
	 * otherwise it could be executed before the preceding store
	 * by the funnel queue code to the "next" pointer, which can,
	 * very rarely, result in failing to issue a wakeup when
	 * needed.
	 *
	 * However, the gain is small, and in testing on our older
	 * "harvard" class machines (Intel Xeon X5680, 3.33GHz) it was
	 * a clear win to skip all of that and go right for the
	 * cmpxchg.
	 *
	 * Of course, the tradeoffs may be sensitive to the particular
	 * work going on, cache pressure, etc.
	 */

	// Pairs with the barrier in batch_processor_work(); see header
	// comment on memory ordering.
	smp_mb__before_atomic();
	old_state = atomic_cmpxchg(&batch->state, BATCH_PROCESSOR_IDLE,
				   BATCH_PROCESSOR_ENQUEUED);
	do_schedule = (old_state == BATCH_PROCESSOR_IDLE);

	if (do_schedule) {
		enqueue_work_queue(batch->vdo->cpu_queue, &batch->work_item);
	}
}

/**********************************************************************/
int make_batch_processor(struct vdo *vdo,
			 batch_processor_callback callback,
			 void *closure,
			 struct batch_processor **batch_ptr)
{
	struct batch_processor *batch;

	int result =
		UDS_ALLOCATE(1, struct batch_processor, "batch_processor", &batch);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = make_funnel_queue(&batch->queue);
	if (result != UDS_SUCCESS) {
		UDS_FREE(batch);
		return result;
	}

	mutex_init(&batch->consumer_mutex);
	setup_work_item(&batch->work_item,
			batch_processor_work,
			callback,
			CPU_Q_ACTION_COMPLETE_VIO);
	atomic_set(&batch->state, BATCH_PROCESSOR_IDLE);
	batch->callback = callback;
	batch->closure = closure;
	batch->vdo = vdo;

	*batch_ptr = batch;
	return UDS_SUCCESS;
}

/**********************************************************************/
void add_to_batch_processor(struct batch_processor *batch,
			    struct vdo_work_item *item)
{
	funnel_queue_put(batch->queue, &item->work_queue_entry_link);
	schedule_batch_processing(batch);
}

/**********************************************************************/
struct vdo_work_item *next_batch_item(struct batch_processor *batch)
{
	struct funnel_queue_entry *fq_entry = funnel_queue_poll(batch->queue);

	if (fq_entry == NULL) {
		return NULL;
	}
	return container_of(fq_entry,
			    struct vdo_work_item,
			    work_queue_entry_link);
}

void free_batch_processor(struct batch_processor *batch)
{
	if (batch == NULL) {
		return;
	}

	// Pairs with the barrier in schedule_batch_processing(). Possibly not
	// needed since it caters to an enqueue vs. free race.
	smp_mb();
	BUG_ON(atomic_read(&batch->state) == BATCH_PROCESSOR_ENQUEUED);

	free_funnel_queue(UDS_FORGET(batch->queue));
	UDS_FREE(batch);
}
