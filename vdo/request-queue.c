// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "request-queue.h"

#include <linux/atomic.h>
#include <linux/wait.h>

#include "compiler.h"
#include "funnel-queue.h"
#include "logger.h"
#include "request.h"
#include "memory-alloc.h"
#include "uds-threads.h"

/*
 * Ordering:
 *
 * Multiple retry requests or multiple non-retry requests enqueued from
 * a single producer thread will be processed in the order enqueued.
 *
 * Retry requests will generally be processed before normal requests.
 *
 * HOWEVER, a producer thread can enqueue a retry request (generally given
 * higher priority) and then enqueue a normal request, and they can get
 * processed in the reverse order.  The checking of the two internal queues is
 * very simple and there's a potential race with the producer regarding the
 * "priority" handling.  If an ordering guarantee is needed, it can be added
 * without much difficulty, it just makes the code a bit more complicated.
 *
 * If requests are enqueued while the processing of another request is
 * happening, and the enqueuing operations complete while the request
 * processing is still in progress, then the retry request(s) *will*
 * get processed next.  (This is used for testing.)
 */

/**
 * Time constants, all in units of nanoseconds.
 **/
enum {
	ONE_NANOSECOND = 1,
	ONE_MICROSECOND = 1000 * ONE_NANOSECOND,
	ONE_MILLISECOND = 1000 * ONE_MICROSECOND,
	ONE_SECOND = 1000 * ONE_MILLISECOND,

	/** The initial time to wait after waiting with no timeout */
	DEFAULT_WAIT_TIME = 20 * ONE_MICROSECOND,

	/** The minimum time to wait when waiting with a timeout */
	MINIMUM_WAIT_TIME = DEFAULT_WAIT_TIME / 2,

	/** The maximimum time to wait when waiting with a timeout */
	MAXIMUM_WAIT_TIME = ONE_MILLISECOND
};

/**
 * Batch size tuning constants. These are compared to the number of requests
 * that have been processed since the worker thread last woke up.
 **/
enum {
	MINIMUM_BATCH = 32, /* wait time increases if batch smaller than this */
	MAXIMUM_BATCH = 64  /* wait time decreases if batch larger than this */
};

struct uds_request_queue {
	/* Wait queue for synchronizing producers and consumer */
	struct wait_queue_head wqhead;
	/* function to process 1 request */
	uds_request_queue_processor_t *process_one;
	/* new incoming requests */
	struct funnel_queue *main_queue;
	/* old requests to retry first */
	struct funnel_queue *retry_queue;
	/* thread id of the worker thread */
	struct thread *thread;
	/* true if the worker was started */
	bool started;
	/* when true, requests can be enqueued */
	bool alive;
	/* A flag set when the worker is waiting without a timeout */
	atomic_t dormant;
};

/**
 * Poll the underlying lock-free queues for a request to process.  Must only be
 * called by the worker thread.
 *
 * @param queue  the request queue being serviced
 *
 * @return a dequeued request, or NULL if no request was available
 **/
static INLINE struct uds_request *poll_queues(struct uds_request_queue *queue)
{
	/* The retry queue has higher priority. */
	struct funnel_queue_entry *entry =
		funnel_queue_poll(queue->retry_queue);
	if (entry != NULL) {
		return container_of(entry, struct uds_request, request_queue_link);
	}

	/* The main queue has lower priority. */
	entry = funnel_queue_poll(queue->main_queue);
	if (entry != NULL) {
		return container_of(entry, struct uds_request, request_queue_link);
	}

	/* No entry found. */
	return NULL;
}

/**
 * Check if the underlying lock-free queues appear not just not to have any
 * requests available right now, but also not to be in the intermediate state
 * of getting requests added. Must only be called by the worker thread.
 *
 * @param queue  the uds_request_queue being serviced
 *
 * @return true iff both funnel queues are idle
 **/
static INLINE bool are_queues_idle(struct uds_request_queue *queue)
{
	return (is_funnel_queue_idle(queue->retry_queue) &&
		is_funnel_queue_idle(queue->main_queue));
}

/**
 * Remove the next request to be processed from the queue.  Must only be called
 * by the worker thread.
 *
 * @param queue        the queue from which to remove an entry
 * @param request_ptr  the next request is returned here, or a NULL pointer to
 *                     indicate that there will be no more requests
 * @param waited_ptr   return a boolean to indicate that we need to wait
 *
 * @return True when there is a next request, or when we know that there will
 *         never be another request.  False when we must wait for a request.
 **/
static INLINE bool dequeue_request(struct uds_request_queue *queue,
				   struct uds_request **request_ptr,
				   bool *waited_ptr)
{
	/* Because of batching, we expect this to be the most common code path. */
	struct uds_request *request = poll_queues(queue);

	if (request != NULL) {
		/* Return because we found a request */
		*request_ptr = request;
		return true;
	}

	if (!READ_ONCE(queue->alive)) {
		/* Return because we see that shutdown is happening */
		*request_ptr = NULL;
		return true;
	}

	/* Return indicating that we need to wait. */
	*request_ptr = NULL;
	*waited_ptr = true;
	return false;
}

static void request_queue_worker(void *arg)
{
	struct uds_request_queue *queue = (struct uds_request_queue *) arg;
	unsigned long time_batch = DEFAULT_WAIT_TIME;
	bool dormant = atomic_read(&queue->dormant);
	long current_batch = 0;

	for (;;) {
		struct uds_request *request;
		bool waited = false;

		if (dormant) {
			/*
			 * Sleep/wakeup protocol:
			 *
			 * The enqueue operation updates "newest" in the
			 * funnel queue via xchg which is a memory barrier,
			 * and later checks "dormant" to decide whether to do
			 * a wakeup of the worker thread.
			 *
			 * The worker thread, when deciding to go to sleep,
			 * sets "dormant" and then examines "newest" to decide
			 * if the funnel queue is idle. In dormant mode, the
			 * last examination of "newest" before going to sleep
			 * is done inside the wait_event_interruptible macro,
			 * after a point where (one or more) memory barriers
			 * have been issued. (Preparing to sleep uses spin
			 * locks.) Even if the "next" field update isn't
			 * visible yet to make the entry accessible, its
			 * existence will kick the worker thread out of
			 * dormant mode and back into timer-based mode.
			 *
			 * So the two threads should agree on the ordering of
			 * the updating of the two fields.
			 */
			wait_event_interruptible(queue->wqhead,
						 dequeue_request(queue,
								 &request,
								 &waited) ||
						 !are_queues_idle(queue));
		} else {
			wait_event_interruptible_hrtimeout(queue->wqhead,
							   dequeue_request(queue,
									   &request,
									   &waited),
							   ns_to_ktime(time_batch));
		}

		if (likely(request != NULL)) {
			/* We got a request. */
			current_batch++;
			queue->process_one(request);
		} else if (!READ_ONCE(queue->alive)) {
			/* We got no request and we know we are shutting down. */
			break;
		}

		if (dormant) {
			/*
			 * We've been roused from dormancy. Clear the flag so
			 * enqueuers can stop broadcasting (no fence needed for
			 * this transition).
			 */
			atomic_set(&queue->dormant, false);
			dormant = false;
			/*
			 * Reset the timeout back to the default since we don't
			 * know how long we've been asleep and we also want to
			 * be responsive to a new burst.
			 */
			time_batch = DEFAULT_WAIT_TIME;
		} else if (waited) {
			/*
			 * We waited for this request to show up.  Adjust the
			 * wait time if the last batch of requests was too
			 * small or too large..
			 */
			if (current_batch < MINIMUM_BATCH) {
				/*
				 * Adjust the wait time if the last batch of
				 * requests was too small.
				 */
				time_batch += time_batch / 4;
				if (time_batch >= MAXIMUM_WAIT_TIME) {
					/*
					 * The timeout is getting long enough
					 * that we need to switch into dormant
					 * mode.
					 */
					atomic_set(&queue->dormant, true);
					dormant = true;
				}
			} else if (current_batch > MAXIMUM_BATCH) {
				/*
				 * Adjust the wait time if the last batch of
				 * requests was too large.
				 */
				time_batch -= time_batch / 4;
				if (time_batch < MINIMUM_WAIT_TIME) {
					/*
					 * But if the producer is very fast or
					 * the scheduler doesn't wake up
					 * promptly, waiting for very short
					 * times won't make the batches
					 * smaller.
					 */
					time_batch = MINIMUM_WAIT_TIME;
				}
			}
			/* And we must now start a new batch count */
			current_batch = 0;
		}
	}

	/*
	 * Ensure that we see any requests that were guaranteed to have been
	 * fully enqueued before shutdown was flagged.  The corresponding write
	 * barrier is in uds_request_queue_finish.
	 */
	smp_rmb();

	/*
	 * Process every request that is still in the queue, and never wait for
	 * any new requests to show up.
	 */
	for (;;) {
		struct uds_request *request = poll_queues(queue);

		if (request == NULL) {
			break;
		}
		queue->process_one(request);
	}
}

int make_uds_request_queue(const char *queue_name,
			   uds_request_queue_processor_t *process_one,
			   struct uds_request_queue **queue_ptr)
{
	struct uds_request_queue *queue;
	int result = UDS_ALLOCATE(1, struct uds_request_queue, __func__,
				  &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}
	queue->process_one = process_one;
	queue->alive = true;
	atomic_set(&queue->dormant, false);
	init_waitqueue_head(&queue->wqhead);

	result = make_funnel_queue(&queue->main_queue);
	if (result != UDS_SUCCESS) {
		uds_request_queue_finish(queue);
		return result;
	}

	result = make_funnel_queue(&queue->retry_queue);
	if (result != UDS_SUCCESS) {
		uds_request_queue_finish(queue);
		return result;
	}

	result = uds_create_thread(request_queue_worker, queue, queue_name,
				   &queue->thread);
	if (result != UDS_SUCCESS) {
		uds_request_queue_finish(queue);
		return result;
	}

	queue->started = true;
	smp_mb();
	*queue_ptr = queue;
	return UDS_SUCCESS;
}

static INLINE void wake_up_worker(struct uds_request_queue *queue)
{
	/* This is the code sequence recommended in <linux/wait.h> */
	smp_mb();
	if (waitqueue_active(&queue->wqhead)) {
		wake_up(&queue->wqhead);
	}
}

void uds_request_queue_enqueue(struct uds_request_queue *queue,
			       struct uds_request *request)
{
	bool unbatched = request->unbatched;

	funnel_queue_put(request->requeued ? queue->retry_queue :
					     queue->main_queue,
			 &request->request_queue_link);

	/*
	 * We must wake the worker thread when it is dormant (waiting with no
	 * timeout).  An atomic load (read fence) isn't needed here since we
	 * know the queue operation acts as one.
	 */
	if (atomic_read(&queue->dormant) || unbatched) {
		wake_up_worker(queue);
	}
}

void uds_request_queue_finish(struct uds_request_queue *queue)
{
	if (queue == NULL) {
		return;
	}

	/*
	 * This memory barrier ensures that any requests we queued will be
	 * seen.  The point is that when dequeue_request sees the following
	 * update to the alive flag, it will also be able to see any change we
	 * made to a next field in the struct funnel_queue entry.  The
	 * corresponding read barrier is in request_queue_worker.
	 */
	smp_wmb();

	/* Mark the queue as dead. */
	WRITE_ONCE(queue->alive, false);

	if (queue->started) {
		int result;
		/* Wake the worker so it notices that it should exit. */
		wake_up_worker(queue);

		/*
		 * Wait for the worker thread to finish processing any
		 * additional pending work and exit.
		 */
		result = uds_join_threads(queue->thread);
		if (result != UDS_SUCCESS) {
			uds_log_warning_strerror(result,
						 "Failed to join worker thread");
		}
	}

	free_funnel_queue(queue->main_queue);
	free_funnel_queue(queue->retry_queue);
	UDS_FREE(queue);
}
