// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 *
 */

#include "data-vio-pool.h"

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device-mapper.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "data-vio.h"
#include "dump.h"
#include "vdo.h"
#include "types.h"

/**
 * DOC:
 *
 * The data_vio_pool maintains the pool of data_vios which a vdo uses to
 * service incoming bios. For correctness, and in order to avoid potentially
 * expensive or blocking memory allocations during normal operation, the number
 * of concurrently active data_vios is capped. Furthermore, in order to avoid
 * starvation of reads and writes, at most 75% of the data_vios may be used for
 * discards. The data_vio_pool is responsible for enforcing these
 * limits. Threads submitting bios for which a data_vio or discard permit are
 * not available will block until the necessary resources are available. The
 * pool is also responsible for distributing resources to blocked threads and
 * waking them. Finally, the pool attempts to batch the work of recycling
 * data_vios by performing the work of actually assigning resources to blocked
 * threads or placing data_vios back into the pool on a single cpu at a time.
 *
 * The pool contains two "limiters", one for tracking data_vios and one for
 * tracking discard permits. The limiters also provide safe cross-thread access
 * to pool statistics without the need to take the pool's lock. When a thread
 * submits a bio to a vdo device, it will first attempt to get a discard permit
 * if it is a discard, and then to get a data_vio. If the necessary resources
 * are available, the incoming bio will be assigned to the acquired data_vio,
 * and it will be launched. However, if either of these are unavailable, the
 * arrival time of the bio is recorded in the bio's bi_private field, the bio
 * and its submitter are both queued on the appropriate limiter and the
 * submitting thread will then put itself to sleep. (note that this mechanism
 * will break if jiffies are only 32 bits.)
 *
 * Whenever a data_vio has completed processing for the bio it was servicing,
 * release_data_vio() will be called on it. This function will add the data_vio
 * to a funnel queue, and then check the state of the pool. If the pool is not
 * currently processing released data_vios, the pool's completion will be
 * enqueued on a cpu queue. This obviates the need for the releasing threads to
 * hold the pool's lock, and also batches release work while avoiding
 * starvation of the cpu threads.
 *
 * Whenever the pool's completion is run on a cpu thread, it calls
 * process_release_callback() which processes a batch of returned data_vios
 * (currently at most 32) from the pool's funnel queue. For each data_vio, it
 * first checks whether that data_vio was processing a discard. If so, and
 * there is a blocked bio waiting for a discard permit, that permit is
 * notionally transfered to the eldest discard waiter, and that waiter is moved
 * to the end of the list of discard bios waiting for a data_vio. If there are
 * no discard waiters, the discard permit is returned to the pool. Next, the
 * data_vio is assigned to the oldest blocked bio which either has a discard
 * permit, or doesn't need one and relaunched. If neither of these exist, the
 * data_vio is returned to the pool. Finally, if any waiting bios were
 * launched, the threads which blocked trying to submit them are awakened.
 */

enum {
	DATA_VIO_RELEASE_BATCH_SIZE = 128,
};

static const unsigned int VDO_SECTORS_PER_BLOCK_MASK =
	VDO_SECTORS_PER_BLOCK - 1;

struct limiter;
typedef void assigner(struct limiter *limiter);

/*
 * Bookkeeping structure for a single type of resource.
 */
struct limiter {
	/* The data_vio_pool to which this limiter belongs */
	struct data_vio_pool *pool;
	/* The maximum number of data_vios available */
	vio_count_t limit;
	/* The number of resources in use */
	vio_count_t busy;
	/* The maximum number of resources ever simultaneously in use */
	vio_count_t max_busy;
	/* The number of resources to release */
	vio_count_t release_count;
	/* The number of waiters to wake */
	vio_count_t wake_count;
	/*
	 * The list of waiting bios which are known to
	 * process_release_callback()
	 */
	struct bio_list waiters;
	/*
	 * The list of waiting bios which are not yet known to
	 * process_release_callback()
	 */
	struct bio_list new_waiters;
	/* The list of waiters which have their permits */
	struct bio_list *permitted_waiters;
	/* The function for assigning a resource to a waiter */
	assigner *assigner;
	/* The queue of blocked threads */
	wait_queue_head_t blocked_threads;
	/* The arrival time of the eldest waiter */
	uint64_t arrival;
};

/*
 * A data_vio_pool is a collection of preallocated data_vios which may be
 * acquired from any thread, and are released in batches.
 */
struct data_vio_pool {
	/* Completion for scheduling releases */
	struct vdo_completion completion;
	/* The administrative state of the pool */
	struct admin_state state;
	/* Lock protecting the pool */
	spinlock_t lock;
	/* The main limiter controlling the total data_vios in the pool. */
	struct limiter limiter;
	/* The limiter controlling data_vios for discard */
	struct limiter discard_limiter;
	/*
	 * The list of bios which have discard permits but still need a
	 * data_vio
	 */
	struct bio_list permitted_discards;
	/* The list of available data_vios */
	struct list_head available;
	/* The queue of data_vios waiting to be returned to the pool */
	struct funnel_queue *queue;
	/* Whether the pool is processing, or scheduled to process releases */
	atomic_t processing;
	/* The data vios in the pool */
	struct data_vio data_vios[];
};

/**
 * as_data_vio_pool() - Convert a vdo_completion to a data_vio_pool.
 * @completion: The completion to convert.
 *
 * Return: The completion as a data_vio_pool.
 */
static inline struct data_vio_pool * __must_check
as_data_vio_pool(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type,
				   VDO_DATA_VIO_POOL_COMPLETION);
	return container_of(completion,
			    struct data_vio_pool,
			    completion);
}

static inline uint64_t get_arrival_time(struct bio *bio)
{
	return (uint64_t) bio->bi_private;
}

/**
 * check_for_drain_complete_locked() - Check whether a data_vio_pool
 *                                     has no outstanding data_vios or
 *                                     waiters while holding the
 *                                     pool's lock.
 * @pool: The pool to check.
 *
 * Return: true if the pool has no busy data_vios or waiters.
 */
static bool check_for_drain_complete_locked(struct data_vio_pool *pool)
{
	if (pool->limiter.busy > 0) {
		return false;
	}

	ASSERT_LOG_ONLY((pool->discard_limiter.busy == 0),
			"no outstanding discard permits");

	return (bio_list_empty(&pool->limiter.new_waiters)
		&& bio_list_empty(&pool->discard_limiter.new_waiters));
}

/*
 * Reset a data_vio which has just been acquired from the pool.
 */
static void reset_data_vio(struct data_vio *data_vio, struct vdo *vdo)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	/*
	 * FIXME: We save the bio out of the vio so that we don't forget it.
	 * Maybe we should just not zero that field somehow.
	 */
	struct bio *bio = vio->bio;

	/*
	 * Zero out the fields which don't need to be preserved (i.e. which
	 * are not pointers to separately allocated objects).
	 */
	memset(data_vio, 0, offsetof(struct data_vio, compression));
	memset(&data_vio->compression, 0, offsetof(struct compression_state,
						   block));
	initialize_vio(vio,
		       bio,
		       1,
		       VIO_TYPE_DATA,
		       VIO_PRIORITY_DATA,
		       vdo);
}

static void launch_bio(struct vdo *vdo,
		       struct data_vio *data_vio,
		       struct bio *bio)
{
	enum data_vio_operation operation = DATA_VIO_WRITE;
	logical_block_number_t lbn;
	reset_data_vio(data_vio, vdo);
	data_vio->user_bio = bio;
	data_vio->offset = to_bytes(bio->bi_iter.bi_sector
				    & VDO_SECTORS_PER_BLOCK_MASK);
	data_vio->is_partial = ((bio->bi_iter.bi_size < VDO_BLOCK_SIZE) ||
				(data_vio->offset != 0));

	/*
	 * Discards behave very differently than other requests when coming in
	 * from device-mapper. We have to be able to handle any size discards
	 * and various sector offsets within a block.
	 */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		data_vio->remaining_discard = bio->bi_iter.bi_size;
		if (data_vio->is_partial) {
			vdo_count_bios(&vdo->stats.bios_in_partial, bio);
			operation = DATA_VIO_READ_MODIFY_WRITE;
		}
	} else if (data_vio->is_partial) {
		vdo_count_bios(&vdo->stats.bios_in_partial, bio);
		operation = ((bio_data_dir(bio) == READ)
			     ? DATA_VIO_READ : DATA_VIO_READ_MODIFY_WRITE);
	} else if (bio_data_dir(bio) == READ) {
		operation = DATA_VIO_READ;
	} else {
		/*
		 * Copy the bio data to a char array so that we can continue to
		 * use the data after we acknowledge the bio.
		 */
		vdo_bio_copy_data_in(bio, data_vio->data_block);
		data_vio->is_zero_block = is_zero_block(data_vio->data_block);
	}

	if (data_vio->user_bio->bi_opf & REQ_FUA) {
		operation |= DATA_VIO_FUA;
	}

	lbn = ((bio->bi_iter.bi_sector - vdo->starting_sector_offset)
	       / VDO_SECTORS_PER_BLOCK);
	launch_data_vio(data_vio, lbn, operation);
}

static void assign_data_vio(struct limiter *limiter, struct data_vio *data_vio)
{
	struct bio *bio = bio_list_pop(limiter->permitted_waiters);

	launch_bio(limiter->pool->completion.vdo, data_vio, bio);
	limiter->wake_count++;

	bio = bio_list_peek(limiter->permitted_waiters);
	limiter->arrival = ((bio == NULL) ? UINT64_MAX : get_arrival_time(bio));
}

static void assign_discard_permit(struct limiter *limiter)
{
	struct bio *bio = bio_list_pop(&limiter->waiters);

	if (limiter->arrival == UINT64_MAX) {
		limiter->arrival = get_arrival_time(bio);
	}

	bio_list_add(limiter->permitted_waiters, bio);
}

static void get_waiters(struct limiter *limiter)
{
	bio_list_merge(&limiter->waiters, &limiter->new_waiters);
	bio_list_init(&limiter->new_waiters);
}

static inline
struct data_vio *get_available_data_vio(struct data_vio_pool *pool)
{
	struct data_vio *data_vio = list_first_entry(&pool->available,
						     struct data_vio,
						     pool_entry);
	list_del_init(&data_vio->pool_entry);
	return data_vio;
}

static void assign_data_vio_to_waiter(struct limiter *limiter)
{
	assign_data_vio(limiter, get_available_data_vio(limiter->pool));
}

static void update_limiter(struct limiter *limiter)
{
	struct bio_list *waiters = &limiter->waiters;
	vio_count_t available = limiter->limit - limiter->busy;

	ASSERT_LOG_ONLY((limiter->release_count <= limiter->busy),
			"Release count %u is not more than busy count %u",
			limiter->release_count,
			limiter->busy);

	get_waiters(limiter);
	for (; (limiter->release_count > 0) && !bio_list_empty(waiters);
	     limiter->release_count--) {
		limiter->assigner(limiter);
	}

	if (limiter->release_count > 0) {
		WRITE_ONCE(limiter->busy,
			   limiter->busy - limiter->release_count);
		limiter->release_count = 0;
		return;
	}

	for (; (available > 0) && !bio_list_empty(waiters); available--) {
		limiter->assigner(limiter);
	}

	WRITE_ONCE(limiter->busy, limiter->limit - available);
	if (limiter->max_busy < limiter->busy) {
		WRITE_ONCE(limiter->max_busy, limiter->busy);
	}
}

/**
 * schedule_releases() - Ensure that release processing is scheduled.
 * @pool: The data_vio_pool which has resources to release.
 *
 * If this call switches the state to processing, enqueue. Otherwise, some
 * other thread has already done so.
 */
static void schedule_releases(struct data_vio_pool *pool)
{
	/* Pairs with the barrier in process_release_callback(). */
	smp_mb__before_atomic();
	if (atomic_cmpxchg(&pool->processing, false, true)) {
		return;
	}

	pool->completion.requeue = true;
	vdo_invoke_completion_callback_with_priority(&pool->completion,
						     CPU_Q_COMPLETE_VIO_PRIORITY);
}

static void reuse_or_release_resources(struct data_vio_pool *pool,
				       struct data_vio *data_vio,
				       struct list_head *returned)
{
	if (data_vio->remaining_discard > 0) {
		if (bio_list_empty(&pool->discard_limiter.waiters)) {
			/* Return the data_vio's discard permit. */
			pool->discard_limiter.release_count++;
		} else {
			assign_discard_permit(&pool->discard_limiter);
		}
	}

	if (pool->limiter.arrival < pool->discard_limiter.arrival) {
		assign_data_vio(&pool->limiter, data_vio);
	} else if (pool->discard_limiter.arrival < UINT64_MAX) {
		assign_data_vio(&pool->discard_limiter, data_vio);
	} else {
		list_add(&data_vio->pool_entry, returned);
		pool->limiter.release_count++;
	}
}

/**
 * process_release_callback() - Process a batch of data_vio releases.
 * @completion: The pool with data_vios to release.
 */
static void process_release_callback(struct vdo_completion *completion)
{
	struct data_vio_pool *pool = as_data_vio_pool(completion);
	bool reschedule;
	bool drained;
	vio_count_t processed;
	vio_count_t to_wake;
	vio_count_t discards_to_wake;
	LIST_HEAD(returned);

	spin_lock(&pool->lock);
	get_waiters(&pool->discard_limiter);
	get_waiters(&pool->limiter);
	spin_unlock(&pool->lock);

	if (pool->limiter.arrival == UINT64_MAX) {
		struct bio *bio = bio_list_peek(&pool->limiter.waiters);

		if (bio != NULL) {
			pool->limiter.arrival = get_arrival_time(bio);
		}
	}

	for (processed = 0;
	     processed < DATA_VIO_RELEASE_BATCH_SIZE;
	     processed++) {
		struct data_vio *data_vio;
		struct funnel_queue_entry *entry
			= funnel_queue_poll(pool->queue);

		if (entry == NULL) {
			break;
		}

		data_vio = data_vio_from_funnel_queue_entry(entry);
		acknowledge_data_vio(data_vio);
		reuse_or_release_resources(pool, data_vio, &returned);
	}

	spin_lock(&pool->lock);
	/*
	 * There is a race where waiters could be added while we are in the
	 * unlocked section above. Those waiters could not see the resources we
	 * are now about to release, so we assign those resources now as we
	 * have no guarantee of being rescheduled. This is handled in
	 * update_limiter().
	 */
	update_limiter(&pool->discard_limiter);
	list_splice(&returned, &pool->available);
	update_limiter(&pool->limiter);
	to_wake = pool->limiter.wake_count;
	pool->limiter.wake_count = 0;
	discards_to_wake = pool->discard_limiter.wake_count;
	pool->discard_limiter.wake_count = 0;

	atomic_set(&pool->processing, false);
	/* Pairs with the barrier in schedule_releases(). */
	smp_mb();

	reschedule = !is_funnel_queue_empty(pool->queue);
	drained = (!reschedule
		   && vdo_is_state_draining(&pool->state)
		   && check_for_drain_complete_locked(pool));
	spin_unlock(&pool->lock);

	if (to_wake > 0) {
		wake_up_nr(&pool->limiter.blocked_threads, to_wake);
	}

	if (discards_to_wake > 0) {
		wake_up_nr(&pool->discard_limiter.blocked_threads,
			   discards_to_wake);
	}

	if (reschedule) {
		schedule_releases(pool);
	} else if (drained) {
		vdo_finish_draining(&pool->state);
	}
}

static void initialize_limiter(struct limiter *limiter,
			       struct data_vio_pool *pool,
			       assigner *assigner,
			       vio_count_t limit)
{
	limiter->pool = pool;
	limiter->assigner = assigner;
	limiter->limit = limit;
	limiter->arrival = UINT64_MAX;
	init_waitqueue_head(&limiter->blocked_threads);
}

/**
 * make_data_vio_pool() - Initialize a data_vio pool.
 * @vdo: The vdo to which the pool will belong.
 * @pool_size: The number of data_vios in the pool.
 * @discard_limit: The maximum number of data_vios which may be used for
 *                 discards.
 * @pool: A pointer to hold the newly allocated pool.
 */
int make_data_vio_pool(struct vdo *vdo,
		       vio_count_t pool_size,
		       vio_count_t discard_limit,
		       struct data_vio_pool **pool_ptr)
{
	int result;
	struct data_vio_pool *pool;
	vio_count_t i;

	result = UDS_ALLOCATE_EXTENDED(struct data_vio_pool,
				       pool_size,
				       struct data_vio,
				       __func__,
				       &pool);
	if (result != UDS_SUCCESS) {
		return result;
	}

	ASSERT_LOG_ONLY((discard_limit <= pool_size),
			"discard limit does not exceed pool size");
	initialize_limiter(&pool->discard_limiter,
			   pool,
			   assign_discard_permit,
			   discard_limit);
	pool->discard_limiter.permitted_waiters = &pool->permitted_discards;
	initialize_limiter(&pool->limiter,
			   pool,
			   assign_data_vio_to_waiter,
			   pool_size);
	pool->limiter.permitted_waiters = &pool->limiter.waiters;
	INIT_LIST_HEAD(&pool->available);
	spin_lock_init(&pool->lock);
	vdo_set_admin_state_code(&pool->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);
	vdo_initialize_completion(&pool->completion,
				  vdo,
				  VDO_DATA_VIO_POOL_COMPLETION);
	vdo_prepare_completion(&pool->completion,
			       process_release_callback,
			       process_release_callback,
			       vdo->thread_config->cpu_thread,
			       NULL);

	result = make_funnel_queue(&pool->queue);
	if (result != UDS_SUCCESS) {
		free_data_vio_pool(UDS_FORGET(pool));
		return result;
	}

	for (i = 0; i < pool_size; i++) {
		struct data_vio *data_vio = &pool->data_vios[i];

		result = initialize_data_vio(data_vio);
		if (result != VDO_SUCCESS) {
			free_data_vio_pool(pool);
			return result;
		}

		list_add(&data_vio->pool_entry, &pool->available);
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**
 * free_data_vio_pool() - Free a data_vio_pool and the data_vios in it.
 * @pool: The pool to free (may be NULL).
 *
 * All data_vios must be returned to the pool before calling this function.
 */
void free_data_vio_pool(struct data_vio_pool *pool)
{
	if (pool == NULL) {
		return;
	}

	/*
	 * Pairs with the barrier in process_release_callback(). Possibly not
	 * needed since it caters to an enqueue vs. free race.
	 */
	smp_mb();
	BUG_ON(atomic_read(&pool->processing));

	spin_lock(&pool->lock);
	ASSERT_LOG_ONLY((pool->limiter.busy == 0),
			"data_vio pool must not have %u busy entries when being freed",
			pool->limiter.busy);
	ASSERT_LOG_ONLY((bio_list_empty(&pool->limiter.waiters)
			 && bio_list_empty(&pool->limiter.new_waiters)),
			"data_vio pool must not have threads waiting to read or write when being freed");
	ASSERT_LOG_ONLY((bio_list_empty(&pool->discard_limiter.waiters)
			 && bio_list_empty(&pool->discard_limiter.new_waiters)),
			"data_vio pool must not have threads waiting to discard when being freed");
	spin_unlock(&pool->lock);

	while (!list_empty(&pool->available)) {
		struct data_vio *data_vio = list_first_entry(&pool->available,
							     struct data_vio,
							     pool_entry);

		list_del_init(pool->available.next);
		destroy_data_vio(data_vio);
	}

	free_funnel_queue(UDS_FORGET(pool->queue));
	UDS_FREE(pool);
}

static bool acquire_permit(struct limiter *limiter, struct bio *bio)
{
	if (limiter->busy >= limiter->limit) {
		DEFINE_WAIT(wait);

		bio_list_add(&limiter->new_waiters, bio);
		prepare_to_wait_exclusive(&limiter->blocked_threads,
					  &wait,
					  TASK_UNINTERRUPTIBLE);
		spin_unlock(&limiter->pool->lock);
		io_schedule();
		finish_wait(&limiter->blocked_threads, &wait);
		return false;
	}

	WRITE_ONCE(limiter->busy, limiter->busy + 1);
	if (limiter->max_busy < limiter->busy) {
		WRITE_ONCE(limiter->max_busy, limiter->busy);
	}

	return true;
}

/**
 * vdo_launch_bio() - Acquire a data_vio from the pool, assign the bio to it,
 *                    and send it on its way.
 * @pool: The pool from which to acquire a data_vio.
 * @bio: The bio to launch.
 *
 * This will block if data_vios or discard permits are not available.
 */
void vdo_launch_bio(struct data_vio_pool *pool, struct bio *bio)
{
	struct data_vio *data_vio;

	ASSERT_LOG_ONLY(!vdo_is_state_quiescent(&pool->state),
			"data_vio_pool not quiescent on acquire");

	bio->bi_private = (void *) jiffies;
	spin_lock(&pool->lock);
	if ((bio_op(bio) == REQ_OP_DISCARD) &&
	    !acquire_permit(&pool->discard_limiter, bio)) {
		return;
	}

	if (!acquire_permit(&pool->limiter, bio)) {
		return;
	}

	data_vio = get_available_data_vio(pool);
	spin_unlock(&pool->lock);
	launch_bio(pool->completion.vdo, data_vio, bio);
}

/**
 * release_data_vio() - Return a data_vio to the pool.
 * @data_vio: The data_vio to return.
 */
void release_data_vio(struct data_vio *data_vio)
{
	struct data_vio_pool *pool =
		vdo_from_data_vio(data_vio)->data_vio_pool;

	funnel_queue_put(pool->queue,
			 &(data_vio_as_completion(data_vio)->work_queue_entry_link));
	schedule_releases(pool);
}

/**
 * initiate_drain() - Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 */
static void initiate_drain(struct admin_state *state)
{
	bool drained;
	struct data_vio_pool *pool = container_of(state,
						  struct data_vio_pool,
						  state);

	spin_lock(&pool->lock);
	drained = check_for_drain_complete_locked(pool);
	spin_unlock(&pool->lock);

	if (drained) {
		vdo_finish_draining(state);
	}
}

/**
 * drain_data_vio_pool() - Wait asynchronously for all data_vios to be
 *                         returned to the pool.
 * @pool: The data_vio_pool to drain.
 * @completion: The completion to notify when the pool has drained.
 */
void drain_data_vio_pool(struct data_vio_pool *pool,
			 struct vdo_completion *completion)
{
	assert_on_vdo_cpu_thread(completion->vdo, __func__);
	vdo_start_draining(&pool->state,
			   VDO_ADMIN_STATE_SUSPENDING,
			   completion,
			   initiate_drain);
}

/**
 * resume_data_vio_pool() - Resume a data_vio pool.
 * @pool: The pool to resume.
 * @completion: The completion to notify when the pool has resumed.
 */
void resume_data_vio_pool(struct data_vio_pool *pool,
			  struct vdo_completion *completion)
{
	assert_on_vdo_cpu_thread(completion->vdo, __func__);
	vdo_finish_completion(completion,
			      vdo_resume_if_quiescent(&pool->state));
}

static void dump_limiter(const char *name, struct limiter *limiter)
{
	uds_log_info("%s: %u of %u busy (max %u), %s",
		     name,
		     limiter->busy,
		     limiter->limit,
		     limiter->max_busy,
		     ((bio_list_empty(&limiter->waiters)
		       && bio_list_empty(&limiter->new_waiters))
		      ? "no waiters"
		      : "has waiters"));
}

/**
 * dump_data_vio_pool() - Dump a data_vio pool to the log.
 * @pool: The pool to dump.
 * @dump_vios: Whether to dump the details of each busy data_vio as well.
 */
void dump_data_vio_pool(struct data_vio_pool *pool, bool dump_vios)
{
	/*
	 * In order that syslog can empty its buffer, sleep after 35 elements
	 * for 4ms (till the second clock tick).  These numbers were picked
	 * based on experiments with lab machines.
	 */
	enum { ELEMENTS_PER_BATCH = 35 };
	enum { SLEEP_FOR_SYSLOG = 4000 };

	if (pool == NULL) {
		return;
	}

	spin_lock(&pool->lock);
	dump_limiter("data_vios", &pool->limiter);
	dump_limiter("discard permits", &pool->discard_limiter);
	if (dump_vios) {
		int i;
		int dumped = 0;

		for (i = 0; i < pool->limiter.limit; i++) {
			struct data_vio *data_vio = &pool->data_vios[i];

			if (!list_empty(&data_vio->pool_entry)) {
				continue;
			}

			dump_data_vio(data_vio);
			if (++dumped >= ELEMENTS_PER_BATCH) {
				spin_unlock(&pool->lock);
				dumped = 0;
				fsleep(SLEEP_FOR_SYSLOG);
				spin_lock(&pool->lock);
			}
		}
	}

	spin_unlock(&pool->lock);
}

vio_count_t get_data_vio_pool_active_discards(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.busy);
}

vio_count_t get_data_vio_pool_discard_limit(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.limit);
}

vio_count_t get_data_vio_pool_maximum_discards(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->discard_limiter.max_busy);
}

int set_data_vio_pool_discard_limit(struct data_vio_pool *pool,
				    vio_count_t limit)
{
	if (get_data_vio_pool_request_limit(pool) < limit) {
		// The discard limit may not be higher than the data_vio limit.
		return -EINVAL;
	}

	spin_lock(&pool->lock);
	pool->discard_limiter.limit = limit;
	spin_unlock(&pool->lock);

	return VDO_SUCCESS;
}

vio_count_t get_data_vio_pool_active_requests(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.busy);
}

vio_count_t get_data_vio_pool_request_limit(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.limit);
}

vio_count_t get_data_vio_pool_maximum_requests(struct data_vio_pool *pool)
{
	return READ_ONCE(pool->limiter.max_busy);
}
