/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/batchProcessor.c#7 $
 */

#include "batchProcessor.h"

#include "memoryAlloc.h"

#include "constants.h"

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
 * delayed enqueue operation (see the commentary in funnelQueuePut)
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
	spinlock_t consumerLock;
	FunnelQueue *queue;
	struct kvdo_work_item workItem;
	atomic_t state;
	BatchProcessorCallback callback;
	void *closure;
	KernelLayer *layer;
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
static void batchProcessorWork(struct kvdo_work_item *item)
{
	struct batch_processor *batch =
		container_of(item, struct batch_processor, workItem);

	spin_lock(&batch->consumerLock);
	while (!isFunnelQueueEmpty(batch->queue)) {
		batch->callback(batch, batch->closure);
	}
	atomic_set(&batch->state, BATCH_PROCESSOR_IDLE);
	memoryFence();
	bool need_reschedule = !isFunnelQueueEmpty(batch->queue);
	spin_unlock(&batch->consumerLock);
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
	 * by the FunnelQueue code to the "next" pointer, which can,
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

	smp_mb();
	enum batch_processor_state old_state = atomic_cmpxchg(
		&batch->state, BATCH_PROCESSOR_IDLE, BATCH_PROCESSOR_ENQUEUED);
	bool do_schedule = (old_state == BATCH_PROCESSOR_IDLE);
	if (do_schedule) {
		enqueueCPUWorkQueue(batch->layer, &batch->workItem);
	}
}

/**********************************************************************/
int make_batch_processor(KernelLayer *layer,
			 BatchProcessorCallback callback,
			 void *closure,
			 struct batch_processor **batch_ptr)
{
	struct batch_processor *batch;

	int result =
		ALLOCATE(1, struct batch_processor, "batchProcessor", &batch);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = makeFunnelQueue(&batch->queue);
	if (result != UDS_SUCCESS) {
		FREE(batch);
		return result;
	}

	spin_lock_init(&batch->consumerLock);
	setup_work_item(&batch->workItem,
			batchProcessorWork,
			(KvdoWorkFunction)callback,
			CPU_Q_ACTION_COMPLETE_KVIO);
	atomic_set(&batch->state, BATCH_PROCESSOR_IDLE);
	batch->callback = callback;
	batch->closure  = closure;
	batch->layer    = layer;

	*batch_ptr = batch;
	return UDS_SUCCESS;
}

/**********************************************************************/
void add_to_batch_processor(struct batch_processor *batch,
                            struct kvdo_work_item *item)
{
	funnelQueuePut(batch->queue, &item->work_queue_entry_link);
	schedule_batch_processing(batch);
}

/**********************************************************************/
struct kvdo_work_item *next_batch_item(struct batch_processor *batch)
{
	FunnelQueueEntry *fq_entry = funnelQueuePoll(batch->queue);

	if (fq_entry == NULL) {
		return NULL;
	}
	return container_of(fq_entry,
			    struct kvdo_work_item,
			    work_queue_entry_link);
}

/**********************************************************************/
void cond_resched_batch_processor(struct batch_processor *batch)
{
	cond_resched_lock(&batch->consumerLock);
}

/**********************************************************************/
void free_batch_processor(struct batch_processor **batch_ptr)
{
	struct batch_processor *batch = *batch_ptr;
	if (batch) {
		memoryFence();
		BUG_ON(atomic_read(&batch->state) == BATCH_PROCESSOR_ENQUEUED);
		freeFunnelQueue(batch->queue);
		FREE(batch);
		*batch_ptr = NULL;
	}
}
