// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "workQueue.h"

#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/percpu.h>

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "string-utils.h"

#include "completion.h"
#include "status-codes.h"

static DEFINE_PER_CPU(unsigned int, service_queue_rotor);

/**
 * DOC: Work queue definition.
 *
 * There are two types of work queues: simple, with one worker thread, and
 * round-robin, which uses a group of the former to do the work, and assigns
 * work to them in round-robin fashion (roughly). Externally, both are
 * represented via the same common sub-structure, though there's actually not a
 * great deal of overlap between the two types internally.
 */
struct vdo_work_queue {
	/* Name of just the work queue (e.g., "cpuQ12") */
	char *name;
	bool round_robin_mode;
	struct vdo_thread *owner;
	/* Life cycle functions, etc */
	const struct vdo_work_queue_type *type;
};

struct simple_work_queue {
	struct vdo_work_queue common;
	/* A copy of .thread->pid, for safety in the sysfs support */
	pid_t thread_pid;
	/*
	 * Number of priorities actually used, so we don't keep re-checking
	 * unused funnel queues.
	 */
	unsigned int num_priority_lists;

	struct funnel_queue *priority_lists[VDO_WORK_Q_MAX_PRIORITY + 1];
	struct task_struct *thread;
	void *private;
	/* In a subordinate work queue, a link back to the round-robin parent */
	struct vdo_work_queue *parent_queue;
	/* Padding for cache line separation */
	char pad[CACHE_LINE_BYTES - sizeof(struct vdo_work_queue *)];
	/* Lock protecting priority_map, num_priority_lists, started */
	spinlock_t lock;
	/* Any (0 or 1) worker threads waiting for new work to do */
	wait_queue_head_t waiting_worker_threads;
	/*
	 * Hack to reduce wakeup calls if the worker thread is running. See
	 * comments in workQueue.c.
	 *
	 * FIXME: There is a lot of redundancy with "first_wakeup", though, and
	 * the pair should be re-examined.
	 */
	atomic_t idle;
	/* Wait list for synchronization during worker thread startup */
	wait_queue_head_t start_waiters;
	bool started;

	/*
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
	 * happen to wake up while a completion is being enqueued. If that
	 * happens, the wakeup may be unneeded but will be attempted anyway.
	 *
	 * So the return value from cmpxchg(first_wakeup,0,nonzero) can always
	 * be done, and will tell the submitting thread whether to issue the
	 * wakeup or not; cmpxchg is atomic, so no other synchronization is
	 * needed.
	 *
	 * A timestamp is used rather than, say, 1, so that the worker thread
	 * could record stats on how long it takes to actually get the worker
	 * thread running.
	 *
	 * There is some redundancy between this and "idle" above.
	 */
	atomic64_t first_wakeup;
	/* More padding for cache line separation */
	char pad2[CACHE_LINE_BYTES - sizeof(atomic64_t)];
	/* Last wakeup, in ns. */
	uint64_t most_recent_wakeup;
};

struct round_robin_work_queue {
	struct vdo_work_queue common;
	struct simple_work_queue **service_queues;
	unsigned int num_service_queues;
};

static inline struct simple_work_queue *
as_simple_work_queue(struct vdo_work_queue *queue)
{
	return ((queue == NULL) ?
		 NULL :
		 container_of(queue, struct simple_work_queue, common));
}

static inline struct round_robin_work_queue *
as_round_robin_work_queue(struct vdo_work_queue *queue)
{
	return ((queue == NULL) ?
		 NULL :
		 container_of(queue, struct round_robin_work_queue, common));
}

/* Processing normal completions. */

/*
 * Dequeue and return the next waiting completion, if any.
 *
 * We scan the funnel queues from highest priority to lowest, once; there is
 * therefore a race condition where a high-priority completion can be enqueued
 * followed by a lower-priority one, and we'll grab the latter (but we'll catch
 * the high-priority item on the next call). If strict enforcement of
 * priorities becomes necessary, this function will need fixing.
 */
static struct vdo_completion *
poll_for_completion(struct simple_work_queue *queue)
{
	struct vdo_completion *completion = NULL;
	int i;

	for (i = queue->num_priority_lists - 1; i >= 0; i--) {
		struct funnel_queue_entry *link =
			funnel_queue_poll(queue->priority_lists[i]);
		if (link != NULL) {
			completion = container_of(link,
						  struct vdo_completion,
						  work_queue_entry_link);
			break;
		}
	}

	return completion;
}

static void enqueue_work_queue_completion(struct simple_work_queue *queue,
					  struct vdo_completion *completion)
{
	ASSERT_LOG_ONLY(completion->my_queue == NULL,
			"completion %px (fn %px) to enqueue (%px) is not already queued (%px)",
			completion,
			completion->callback,
			queue,
			completion->my_queue);
	if (completion->priority == VDO_WORK_Q_DEFAULT_PRIORITY) {
		completion->priority = queue->common.type->default_priority;
	}

	if (ASSERT(completion->priority < queue->num_priority_lists,
		   "priority is in range for queue") != VDO_SUCCESS) {
		completion->priority = 0;
	}

	completion->my_queue = &queue->common;

	/* Funnel queue handles the synchronization for the put. */
	funnel_queue_put(queue->priority_lists[completion->priority],
			 &completion->work_queue_entry_link);

	/*
	 * Due to how funnel queue synchronization is handled (just atomic
	 * operations), the simplest safe implementation here would be to
	 * wake-up any waiting threads after enqueueing each item. Even if the
	 * funnel queue is not empty at the time of adding an item to the queue,
	 * the consumer thread may not see this since it is not guaranteed to
	 * have the same view of the queue as a producer thread.
	 *
	 * However, the above is wasteful so instead we attempt to minimize the
	 * number of thread wakeups. Using an idle flag, and careful ordering
	 * using memory barriers, we should be able to determine when the
	 * worker thread might be asleep or going to sleep. We use cmpxchg to
	 * try to take ownership (vs other producer threads) of the
	 * responsibility for waking the worker thread, so multiple wakeups
	 * aren't tried at once.
	 *
	 * This was tuned for some x86 boxes that were handy; it's untested
	 * whether doing the read first is any better or worse for other
	 * platforms, even other x86 configurations.
	 */
	smp_mb();
	if ((atomic_read(&queue->idle) != 1) ||
	    (atomic_cmpxchg(&queue->idle, 1, 0) != 1)) {
		return;
	}

	atomic64_cmpxchg(&queue->first_wakeup, 0, ktime_get_ns());

	/* There's a maximum of one thread in this list. */
	wake_up(&queue->waiting_worker_threads);
}

static void run_start_hook(struct simple_work_queue *queue)
{
	if (queue->common.type->start != NULL) {
		queue->common.type->start(queue->private);
	}
}

static void run_finish_hook(struct simple_work_queue *queue)
{
	if (queue->common.type->finish != NULL) {
		queue->common.type->finish(queue->private);
	}
}

/*
 * Wait for the next completion to process, or until kthread_should_stop
 * indicates that it's time for us to shut down.
 *
 * If kthread_should_stop says it's time to stop but we have pending
 * completions return a completion.
 *
 * Also update statistics relating to scheduler interactions.
 */
static struct vdo_completion *
wait_for_next_completion(struct simple_work_queue *queue)
{
	struct vdo_completion *completion;
	DEFINE_WAIT(wait);

	while (true) {
		atomic64_set(&queue->first_wakeup, 0);
		prepare_to_wait(&queue->waiting_worker_threads,
				&wait,
				TASK_INTERRUPTIBLE);
		/*
		 * Don't set the idle flag until a wakeup will not be lost.
		 *
		 * Force synchronization between setting the idle flag and
		 * checking the funnel queue; the producer side will do them in
		 * the reverse order. (There's still a race condition we've
		 * chosen to allow, because we've got a timeout below that
		 * unwedges us if we hit it, but this may narrow the window a
		 * little.)
		 */
		atomic_set(&queue->idle, 1);
		smp_mb(); /* store-load barrier between "idle" and funnel queue */

		completion = poll_for_completion(queue);
		if (completion != NULL) {
			break;
		}

		/*
		 * We need to check for thread-stop after setting
		 * TASK_INTERRUPTIBLE state up above. Otherwise, schedule()
		 * will put the thread to sleep and might miss a wakeup from
		 * kthread_stop() call in finish_work_queue().
		 */
		if (kthread_should_stop()) {
			break;
		}

		schedule();

		/*
		 * Check again before resetting first_wakeup for more accurate
		 * stats. If it was a spurious wakeup, continue looping.
		 */
		completion = poll_for_completion(queue);
		if (completion != NULL) {
			break;
		}
	}

	finish_wait(&queue->waiting_worker_threads, &wait);
	atomic_set(&queue->idle, 0);

	return completion;
}

static void process_completion(struct simple_work_queue *queue,
			       struct vdo_completion *completion)
{
	if (ASSERT(completion->my_queue == &queue->common,
		   "completion %px from queue %px marked as being in this queue (%px)",
		   completion,
		   queue,
		   completion->my_queue) == UDS_SUCCESS) {
		completion->my_queue = NULL;
	}

	vdo_run_completion_callback(completion);
}

static void yield_to_scheduler(struct simple_work_queue *queue)
{
	cond_resched();
	queue->most_recent_wakeup = ktime_get_ns();
}

static void service_work_queue(struct simple_work_queue *queue)
{
	run_start_hook(queue);

	while (true) {
		struct vdo_completion *completion = poll_for_completion(queue);

		if (completion == NULL) {
			completion = wait_for_next_completion(queue);
		}

		if (completion == NULL) {
			/*
			 * No completions but kthread_should_stop() was
			 * triggered.
			 */
			break;
		}

		process_completion(queue, completion);

		/*
		 * Be friendly to a CPU that has other work to do, if the
		 * kernel has told us to. This speeds up some performance
		 * tests; that "other work" might include other VDO threads.
		 */
		if (need_resched()) {
			yield_to_scheduler(queue);
		}
	}

	run_finish_hook(queue);
}

static int work_queue_runner(void *ptr)
{
	struct simple_work_queue *queue = ptr;
	unsigned long flags;

	queue->most_recent_wakeup = ktime_get_ns();

	spin_lock_irqsave(&queue->lock, flags);
	queue->started = true;
	spin_unlock_irqrestore(&queue->lock, flags);

	wake_up(&queue->start_waiters);
	service_work_queue(queue);

	return 0;
}

/* Creation & teardown */

static void free_simple_work_queue(struct simple_work_queue *queue)
{
	unsigned int i;

	for (i = 0; i <= VDO_WORK_Q_MAX_PRIORITY; i++) {
		free_funnel_queue(queue->priority_lists[i]);
	}
	UDS_FREE(queue->common.name);
	UDS_FREE(queue);
}

static void free_round_robin_work_queue(struct round_robin_work_queue *queue)
{
	struct simple_work_queue **queue_table = queue->service_queues;
	unsigned int count = queue->num_service_queues;
	unsigned int i;

	queue->service_queues = NULL;

	for (i = 0; i < count; i++) {
		free_simple_work_queue(queue_table[i]);
	}
	UDS_FREE(queue_table);
	UDS_FREE(queue->common.name);
	UDS_FREE(queue);
}

void free_work_queue(struct vdo_work_queue *queue)
{
	if (queue == NULL) {
		return;
	}

	finish_work_queue(queue);

	if (queue->round_robin_mode) {
		free_round_robin_work_queue(as_round_robin_work_queue(queue));
	} else {
		free_simple_work_queue(as_simple_work_queue(queue));
	}
}

static bool queue_started(struct simple_work_queue *queue)
{
	unsigned long flags;
	bool started;

	spin_lock_irqsave(&queue->lock, flags);
	started = queue->started;
	spin_unlock_irqrestore(&queue->lock, flags);

	return started;
}

static int make_simple_work_queue(const char *thread_name_prefix,
				  const char *name,
				  struct vdo_thread *owner,
				  void *private,
				  const struct vdo_work_queue_type *type,
				  struct simple_work_queue **queue_ptr)
{
	struct simple_work_queue *queue;
	int i;
	struct task_struct *thread = NULL;
	int result;

	ASSERT_LOG_ONLY((type->max_priority <= VDO_WORK_Q_MAX_PRIORITY),
			"queue priority count %u within limit %u",
			type->max_priority,
			VDO_WORK_Q_MAX_PRIORITY);

	result = UDS_ALLOCATE(1,
			      struct simple_work_queue,
			      "simple work queue",
			      &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	queue->private = private;
	queue->common.type = type;
	queue->common.owner = owner;

	result = uds_duplicate_string(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		UDS_FREE(queue);
		return -ENOMEM;
	}

	init_waitqueue_head(&queue->waiting_worker_threads);
	init_waitqueue_head(&queue->start_waiters);
	spin_lock_init(&queue->lock);

	queue->num_priority_lists = type->max_priority + 1;
	for (i = 0; i < queue->num_priority_lists; i++) {
		result = make_funnel_queue(&queue->priority_lists[i]);
		if (result != UDS_SUCCESS) {
			free_simple_work_queue(queue);
			return result;
		}
	}

	queue->started = false;
	thread = kthread_run(work_queue_runner,
			     queue,
			     "%s:%s",
			     thread_name_prefix,
			     queue->common.name);
	if (IS_ERR(thread)) {
		free_simple_work_queue(queue);
		return (int) PTR_ERR(thread);
	}

	queue->thread = thread;
	WRITE_ONCE(queue->thread_pid, thread->pid);

	/*
	 * If we don't wait to ensure the thread is running VDO code, a
	 * quick kthread_stop (due to errors elsewhere) could cause it to
	 * never get as far as running VDO, skipping the cleanup code.
	 *
	 * Eventually we should just make that path safe too, and then we
	 * won't need this synchronization.
	 */
	wait_event(queue->start_waiters, queue_started(queue) == true);

	*queue_ptr = queue;
	return UDS_SUCCESS;
}

/**
 * Create a work queue; if multiple threads are requested, completions will be
 * distributed to them in round-robin fashion.
 *
 * Each queue is associated with a struct vdo_thread which has a single vdo
 * thread id. Regardless of the actual number of queues and threads allocated
 * here, code outside of the queue implementation will treat this as a single
 * zone.
 */
int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct vdo_thread *owner,
		    const struct vdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct vdo_work_queue **queue_ptr)
{
	struct round_robin_work_queue *queue;
	int result;
	char thread_name[TASK_COMM_LEN];
	unsigned int i;

	if (thread_count == 1) {
		struct simple_work_queue *simple_queue;
		void *context = ((thread_privates != NULL)
				 ? thread_privates[0]
				 : NULL);
		result = make_simple_work_queue(thread_name_prefix,
						name,
						owner,
						context,
						type,
						&simple_queue);
		if (result == VDO_SUCCESS) {
			*queue_ptr = &simple_queue->common;
		}
		return result;
	}

	result = UDS_ALLOCATE(1, struct round_robin_work_queue,
			      "round-robin work queue", &queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(thread_count,
			      struct simple_work_queue *,
			      "subordinate work queues",
			      &queue->service_queues);
	if (result != UDS_SUCCESS) {
		UDS_FREE(queue);
		return result;
	}

	queue->num_service_queues = thread_count;
	queue->common.round_robin_mode = true;
	queue->common.owner = owner;

	result = uds_duplicate_string(name, "queue name", &queue->common.name);
	if (result != VDO_SUCCESS) {
		UDS_FREE(queue->service_queues);
		UDS_FREE(queue);
		return -ENOMEM;
	}

	*queue_ptr = &queue->common;

	for (i = 0; i < thread_count; i++) {
		void *context = ((thread_privates != NULL)
				 ? thread_privates[i]
				 : NULL);
		snprintf(thread_name, sizeof(thread_name), "%s%u", name, i);
		result = make_simple_work_queue(thread_name_prefix,
						thread_name,
						owner,
						context,
						type,
						&queue->service_queues[i]);
		if (result != VDO_SUCCESS) {
			queue->num_service_queues = i;
			/* Destroy previously created subordinates. */
			free_work_queue(UDS_FORGET(*queue_ptr));
			return result;
		}
		queue->service_queues[i]->parent_queue = *queue_ptr;
	}

	return VDO_SUCCESS;
}

static void finish_simple_work_queue(struct simple_work_queue *queue)
{
	if (queue->thread == NULL) {
		return;
	}

	/*
	 * Reduces (but does not eliminate) the chance of the sysfs support
	 * reporting the pid even after the thread is gone.
	 */
	WRITE_ONCE(queue->thread_pid, 0);

	/* Tells the worker thread to shut down and waits for it to exit. */
	kthread_stop(queue->thread);
	queue->thread = NULL;
}

static void finish_round_robin_work_queue(struct round_robin_work_queue *queue)
{
	struct simple_work_queue **queue_table = queue->service_queues;
	unsigned int count = queue->num_service_queues;
	unsigned int i;

	for (i = 0; i < count; i++) {
		finish_simple_work_queue(queue_table[i]);
	}
}

/*
 * No enqueueing of completions should be done once this function is called.
 */
void finish_work_queue(struct vdo_work_queue *queue)
{
	if (queue == NULL) {
		return;
	}

	if (queue->round_robin_mode) {
		struct round_robin_work_queue *rrqueue
			= as_round_robin_work_queue(queue);
		finish_round_robin_work_queue(rrqueue);
	} else {
		finish_simple_work_queue(as_simple_work_queue(queue));
	}
}

/* Debugging dumps */

static void dump_simple_work_queue(struct simple_work_queue *queue)
{
	const char *thread_status = "no threads";
	char task_state_report = '-';

	if (queue->thread != NULL) {
		task_state_report = task_state_to_char(queue->thread);
		thread_status = atomic_read(&queue->idle) ? "idle" : "running";
	}

	uds_log_info("workQ %px (%s) %s (%c)",
		     &queue->common,
		     queue->common.name,
		     thread_status,
		     task_state_report);

	/*
	 * ->lock spin lock status?
	 * ->waiting_worker_threads wait queue status? anyone waiting?
	 */
}

/**
 * Write to the buffer some info about the completion, for logging.  Since the
 * common use case is dumping info about a lot of completions to syslog all at
 * once, the format favors brevity over readability.
 */
void dump_work_queue(struct vdo_work_queue *queue)
{
	if (queue->round_robin_mode) {
		struct round_robin_work_queue *round_robin_queue =
			as_round_robin_work_queue(queue);
		unsigned int i;

		for (i = 0;
		     i < round_robin_queue->num_service_queues; i++) {
			dump_simple_work_queue(round_robin_queue->service_queues[i]);
		}
	} else {
		dump_simple_work_queue(as_simple_work_queue(queue));
	}
}

static void get_function_name(void *pointer,
			      char *buffer,
			      size_t buffer_length)
{
	if (pointer == NULL) {
                /*
                 * Format "%ps" logs a null pointer as "(null)" with a bunch of
                 * leading spaces. We sometimes use this when logging lots of
                 * data; don't be so verbose.
                 */
		strncpy(buffer, "-", buffer_length);
	} else {
                /*
                 * Use a non-const array instead of a string literal below to
                 * defeat gcc's format checking, which doesn't understand that
                 * "%ps" actually does support a precision spec in Linux kernel
                 * code.
                 */
		static char truncated_function_name_format_string[] = "%.*ps";
		char *space;

		snprintf(buffer,
			 buffer_length,
			 truncated_function_name_format_string,
			 buffer_length - 1,
			 pointer);

		space = strchr(buffer, ' ');

		if (space != NULL) {
			*space = '\0';
		}
	}
}

void dump_completion_to_buffer(struct vdo_completion *completion,
			       char *buffer,
			       size_t length)
{
	size_t current_length =
		scnprintf(buffer,
			  length,
			  "%.*s/",
			  TASK_COMM_LEN,
			  (completion->my_queue == NULL ?
			   "-" :
			   completion->my_queue->name));
	if (current_length < length) {
		get_function_name((void *) completion->callback,
				  buffer + current_length,
				  length - current_length);
	}
}

/* Completion submission */
/*
 * If the completion has a timeout that has already passed, the timeout handler
 * function may be invoked by this function.
 */
void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_completion *completion)
{
	/*
	 * Convert the provided generic vdo_work_queue to the simple_work_queue
	 * to actually queue on.
	 */
	struct simple_work_queue *simple_queue = NULL;

	if (!queue->round_robin_mode) {
		simple_queue = as_simple_work_queue(queue);
	} else {
		struct round_robin_work_queue *round_robin
			= as_round_robin_work_queue(queue);

		/*
		 * It shouldn't be a big deal if the same rotor gets used for
		 * multiple work queues. Any patterns that might develop are
		 * likely to be disrupted by random ordering of multiple
		 * completions and migration between cores, unless the load is
		 * so light as to be regular in ordering of tasks and the
		 * threads are confined to individual cores; with a load that
		 * light we won't care.
		 */
		unsigned int rotor = this_cpu_inc_return(service_queue_rotor);
		unsigned int index = rotor % round_robin->num_service_queues;

		simple_queue = round_robin->service_queues[index];
	}

	enqueue_work_queue_completion(simple_queue, completion);
}

/* Misc */

/*
 * Return the work queue pointer recorded at initialization time in the
 * work-queue stack handle initialized on the stack of the current thread, if
 * any.
 */
static struct simple_work_queue *get_current_thread_work_queue(void)
{
	/*
	 * In interrupt context, if a vdo thread is what got interrupted, the
	 * calls below will find the queue for the thread which was
	 * interrupted. However, the interrupted thread may have been
	 * processing a completion, in which case starting to process another
	 * would violate our concurrency assumptions.
	 */
	if (in_interrupt()) {
		return NULL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,13,0)
	/*
	 * The kthreadd process has the PF_KTHREAD flag set but a null "struct
	 * kthread" pointer, which breaks the (initial) implementation of
	 * kthread_func, which assumes the pointer is always non-null. This
	 * matters if memory reclamation is triggered and causes calls into VDO
	 * that get here. [VDO-5194]
	 *
	 * There might also be a similar reclamation issue in the
	 * usermodehelper code path before exec is called, and/or kthread setup
	 * when allocating the kthread struct itself.
	 *
	 * So we check for the null pointer first, on older kernels. The
	 * kthread code initially overloaded the set_child_tid field to use for
	 * its pointer in PF_KTHREAD processes. (If PF_KTHREAD is clear,
	 * kthread_func will return null anyway so we needn't worry about that
	 * case.)
	 *
	 * This bug was fixed in the 5.13 kernel release, but the 5.17 kernel
	 * release changed the task structure field used such that this
	 * workaround will break things on newer kernels. It shows up as a null
	 * pointer returned for the current work queue even when running in the
	 * work queue thread.
	 *
	 * Any backports of the 5.13 fix to custom pre-5.13 kernels should have
	 * no problem with this. Backports of the 5.17 change to 5.13 and later
	 * should be okay with this #if check; backports to pre-5.13 will need
	 * further protection.
	 */
	if (current->set_child_tid == NULL) {
		return NULL;
	}
#endif

	if (kthread_func(current) != work_queue_runner) {
		/* Not a VDO work queue thread. */
		return NULL;
	}
	return kthread_data(current);
}

struct vdo_work_queue *get_current_work_queue(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue == NULL) ? NULL : &queue->common;
}

struct vdo_thread *get_work_queue_owner(struct vdo_work_queue *queue)
{
	return queue->owner;
}

/**
 * Returns the private data for the current thread's work queue, or NULL if
 * none or if the current thread is not a work queue thread.
 */
void *get_work_queue_private_data(void)
{
	struct simple_work_queue *queue = get_current_thread_work_queue();

	return (queue != NULL) ? queue->private : NULL;
}

bool vdo_work_queue_type_is(struct vdo_work_queue *queue,
			    const struct vdo_work_queue_type *type) {
	return (queue->type == type);
}
