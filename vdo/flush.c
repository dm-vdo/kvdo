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

#include "flush.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "completion.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "kernelVDO.h"
#include "logical-zone.h"
#include "num-utils.h"
#include "read-only-notifier.h"
#include "slab-depot.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

struct flusher {
	struct vdo_completion completion;
	/** The vdo to which this flusher belongs */
	struct vdo *vdo;
	/** The administrative state of the flusher */
	struct admin_state state;
	/** The current flush generation of the vdo */
	sequence_number_t flush_generation;
	/** The first unacknowledged flush generation */
	sequence_number_t first_unacknowledged_generation;
	/** The queue of flush requests waiting to notify other threads */
	struct wait_queue notifiers;
	/** The queue of flush requests waiting for VIOs to complete */
	struct wait_queue pending_flushes;
	/** The flush generation for which notifications are being sent */
	sequence_number_t notify_generation;
	/** The logical zone to notify next */
	struct logical_zone *logical_zone_to_notify;
	/** The ID of the thread on which flush requests should be made */
	thread_id_t thread_id;
	/** A flush request to ensure we always have at least one */
	struct vdo_flush *spare_flush;
	/** Bios waiting for a flush request to become available */
	struct bio_list waiting_flush_bios;
	/** The lock to protect the previous two fields */
	spinlock_t lock;
	/** The rotor for selecting the bio queue for submitting flush bios */
	zone_count_t bio_queue_rotor;
	/** The number of flushes submitted to the current bio queue */
	int flush_count;
};

/**
 * Check that we are on the flusher thread.
 *
 * @param flusher  The flusher
 * @param caller   The function which is asserting
 **/
static inline void assert_on_flusher_thread(struct flusher *flusher,
					    const char *caller)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == flusher->thread_id),
			"%s() called from flusher thread",
			caller);
}

/**
 * Convert a generic vdo_completion to a flusher.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a flusher
 **/
static struct flusher *as_flusher(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type,
				   VDO_FLUSH_NOTIFICATION_COMPLETION);
	return container_of(completion, struct flusher, completion);
}

/**
 * Convert a generic vdo_completion to a vdo_flush.
 *
 * @param completion The completion to convert
 *
 * @return The completion as a vdo_flush
 **/
static inline struct vdo_flush *
completion_as_vdo_flush(struct vdo_completion *completion)
{
	assert_vdo_completion_type(completion->type, VDO_FLUSH_COMPLETION);
	return container_of(completion, struct vdo_flush, completion);
}

/**
 * Convert a vdo_flush's generic wait queue entry back to the vdo_flush.
 *
 * @param waiter  The wait queue entry to convert
 *
 * @return The wait queue entry as a vdo_flush
 **/
static struct vdo_flush *waiter_as_flush(struct waiter *waiter)
{
	return container_of(waiter, struct vdo_flush, waiter);
}

/**
 * Make a flusher for a vdo.
 *
 * @param vdo  The vdo which owns the flusher
 *
 * @return VDO_SUCCESS or an error
 **/
int make_vdo_flusher(struct vdo *vdo)
{
	int result = UDS_ALLOCATE(1, struct flusher, __func__, &vdo->flusher);

	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo->flusher->vdo = vdo;
	vdo->flusher->thread_id = vdo->thread_config->packer_thread;
	vdo_set_admin_state_code(&vdo->flusher->state,
				 VDO_ADMIN_STATE_NORMAL_OPERATION);
	initialize_vdo_completion(&vdo->flusher->completion, vdo,
				  VDO_FLUSH_NOTIFICATION_COMPLETION);

	spin_lock_init(&vdo->flusher->lock);
	bio_list_init(&vdo->flusher->waiting_flush_bios);
	return UDS_ALLOCATE(1, struct vdo_flush, __func__,
			    &vdo->flusher->spare_flush);
}

/**
 * Free a flusher.
 *
 * @param flusher  The flusher to free
 **/
void free_vdo_flusher(struct flusher *flusher)
{
	if (flusher == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(flusher->spare_flush));
	UDS_FREE(flusher);
}

/**
 * Get the ID of the thread on which flusher functions should be called.
 *
 * @param flusher  The flusher to query
 *
 * @return The ID of the thread which handles the flusher
 **/
thread_id_t get_vdo_flusher_thread_id(struct flusher *flusher)
{
	return flusher->thread_id;
}

/**********************************************************************/
static void notify_flush(struct flusher *flusher);
static void vdo_complete_flush(struct vdo_flush *flush);

/**
 * Finish the notification process by checking if any flushes have completed
 * and then starting the notification of the next flush request if one came in
 * while the current notification was in progress. This callback is registered
 * in flush_packer_callback().
 *
 * @param completion  The flusher completion
 **/
static void finish_notification(struct vdo_completion *completion)
{
	struct waiter *waiter;
	int result;
	struct flusher *flusher = as_flusher(completion);

	assert_on_flusher_thread(flusher, __func__);

	waiter = dequeue_next_waiter(&flusher->notifiers);
	result = enqueue_waiter(&flusher->pending_flushes, waiter);
	if (result != VDO_SUCCESS) {
		struct vdo_flush *flush = waiter_as_flush(waiter);

		vdo_enter_read_only_mode(flusher->vdo->read_only_notifier,
					 result);
		vdo_complete_flush(flush);
		return;
	}

	complete_vdo_flushes(flusher);
	if (has_waiters(&flusher->notifiers)) {
		notify_flush(flusher);
	}
}

/**
 * Flush the packer now that all of the logical and physical zones have been
 * notified of the new flush request. This callback is registered in
 * increment_generation().
 *
 * @param completion  The flusher completion
 **/
static void flush_packer_callback(struct vdo_completion *completion)
{
	struct flusher *flusher = as_flusher(completion);

	increment_vdo_packer_flush_generation(flusher->vdo->packer);
	vdo_launch_completion_callback(completion, finish_notification,
				       flusher->thread_id);
}

/**
 * Increment the flush generation in a logical zone. If there are more logical
 * zones, go on to the next one, otherwise, prepare the physical zones. This
 * callback is registered both in notify_flush() and in itself.
 *
 * @param completion  The flusher as a completion
 **/
static void increment_generation(struct vdo_completion *completion)
{
	struct flusher *flusher = as_flusher(completion);
	struct logical_zone *zone = flusher->logical_zone_to_notify;

	increment_vdo_logical_zone_flush_generation(zone,
						    flusher->notify_generation);
	if (zone->next == NULL) {
		vdo_launch_completion_callback(completion,
					       flush_packer_callback,
					       flusher->thread_id);
		return;
	}

	flusher->logical_zone_to_notify = zone->next;
	vdo_launch_completion_callback(completion,
				       increment_generation,
				       flusher->logical_zone_to_notify->thread_id);
}

/**
 * Lauch a flush notification.
 *
 * @param flusher  The flusher doing the notification
 **/
static void notify_flush(struct flusher *flusher)
{
	struct vdo_flush *flush =
		waiter_as_flush(get_first_waiter(&flusher->notifiers));

	flusher->notify_generation = flush->flush_generation;
	flusher->logical_zone_to_notify
		= &flusher->vdo->logical_zones->zones[0];
	flusher->completion.requeue = true;
	vdo_launch_completion_callback(&flusher->completion,
				       increment_generation,
				       flusher->logical_zone_to_notify->thread_id);
}

/**
 * Start processing a flush request. This callback is registered in
 * launch_flush().
 *
 * @param completion  A flush request (as a vdo_completion)
 **/
static void flush_vdo(struct vdo_completion *completion)
{
	struct vdo_flush *flush = completion_as_vdo_flush(completion);
	struct flusher *flusher = completion->vdo->flusher;
	bool may_notify;
	int result;

	assert_on_flusher_thread(flusher, __func__);
	result = ASSERT(vdo_is_state_normal(&flusher->state),
			"flusher is in normal operation");
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(flusher->vdo->read_only_notifier,
					 result);
		vdo_complete_flush(flush);
		return;
	}

	flush->flush_generation = flusher->flush_generation++;
	may_notify = !has_waiters(&flusher->notifiers);

	result = enqueue_waiter(&flusher->notifiers, &flush->waiter);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(flusher->vdo->read_only_notifier,
					 result);
		vdo_complete_flush(flush);
		return;
	}

	if (may_notify) {
		notify_flush(flusher);
	}
}

/**
 * Check whether the flusher has drained.
 *
 * @param flusher  The flusher
 **/
static void check_for_drain_complete(struct flusher *flusher)
{
	if (vdo_is_state_draining(&flusher->state)
	    && !has_waiters(&flusher->pending_flushes)
	    && bio_list_empty(&flusher->waiting_flush_bios)) {
		finish_vdo_draining(&flusher->state);
	}
}

/**
 * Attempt to complete any flushes which might have finished.
 *
 * @param flusher  The flusher
 **/
void complete_vdo_flushes(struct flusher *flusher)
{
	sequence_number_t oldest_active_generation = UINT64_MAX;
	struct logical_zone *zone;

	assert_on_flusher_thread(flusher, __func__);

	for (zone = &flusher->vdo->logical_zones->zones[0];
	     zone != NULL;
	     zone = zone->next) {
		oldest_active_generation =
			min(oldest_active_generation,
			    READ_ONCE(zone->oldest_active_generation));
	}

	while (has_waiters(&flusher->pending_flushes)) {
		struct vdo_flush *flush =
			waiter_as_flush(get_first_waiter(&flusher->pending_flushes));
		if (flush->flush_generation >= oldest_active_generation) {
			return;
		}

		ASSERT_LOG_ONLY((flush->flush_generation
				 == flusher->first_unacknowledged_generation),
				"acknowledged next expected flush, %llu, was: %llu",
				(unsigned long long) flusher->first_unacknowledged_generation,
				(unsigned long long) flush->flush_generation);
		dequeue_next_waiter(&flusher->pending_flushes);
		vdo_complete_flush(flush);
		flusher->first_unacknowledged_generation++;
	}

	check_for_drain_complete(flusher);
}

/**
 * Dump the flusher, in a thread-unsafe fashion.
 *
 * @param flusher  The flusher
 **/
void dump_vdo_flusher(const struct flusher *flusher)
{
	uds_log_info("struct flusher");
	uds_log_info("  flush_generation=%llu first_unacknowledged_generation=%llu",
		     (unsigned long long) flusher->flush_generation,
		     (unsigned long long) flusher->first_unacknowledged_generation);
	uds_log_info("  notifiers queue is %s; pending_flushes queue is %s",
		     (has_waiters(&flusher->notifiers) ? "not empty" : "empty"),
		     (has_waiters(&flusher->pending_flushes) ? "not empty" : "empty"));
}


/**
 * Initialize a vdo_flush structure, transferring all the bios in the flusher's
 * waiting_flush_bios list to it. The caller MUST already hold the lock.
 *
 * @param flush  The flush to initialize
 * @param vdo    The vdo being flushed
 **/
static void initialize_flush(struct vdo_flush *flush, struct vdo *vdo)
{
	initialize_vdo_completion(&flush->completion,
				  vdo,
				  VDO_FLUSH_COMPLETION);
	bio_list_init(&flush->bios);
	bio_list_merge(&flush->bios, &vdo->flusher->waiting_flush_bios);
	bio_list_init(&vdo->flusher->waiting_flush_bios);
}

static void launch_flush(struct vdo_flush *flush)
{
	struct vdo_completion *completion = &flush->completion;

	vdo_prepare_completion(completion,
			       flush_vdo,
			       flush_vdo,
			       completion->vdo->thread_config->packer_thread,
			       NULL);
	vdo_enqueue_completion_with_priority(completion,
					     VDO_REQ_Q_FLUSH_PRIORITY);
}

/**
 * Function called to start processing a flush request. It is called when we
 * receive an empty flush bio from the block layer, and before acknowledging a
 * non-empty bio with the FUA flag set.
 *
 * @param vdo  The vdo
 * @param bio  The bio containing an empty flush request
 **/
void launch_vdo_flush(struct vdo *vdo, struct bio *bio)
{
	/*
	 * Try to allocate a vdo_flush to represent the flush request. If the
	 * allocation fails, we'll deal with it later.
	 */
	struct vdo_flush *flush
		= UDS_ALLOCATE_NOWAIT(struct vdo_flush, __func__);
	struct flusher *flusher = vdo->flusher;
	const struct admin_state_code *code =
		vdo_get_admin_state_code(&flusher->state);

	ASSERT_LOG_ONLY(!code->quiescent,
			"Flushing not allowed in state %s",
			code->name);

	spin_lock(&flusher->lock);

	/* We have a new bio to start. Add it to the list. */
	bio_list_add(&flusher->waiting_flush_bios, bio);

	if (flush == NULL) {
		/*
		 * The vdo_flush allocation failed. Try to use the spare
		 * vdo_flush structure.
		 */
		if (flusher->spare_flush == NULL) {
			/*
			 * The spare is already in use. This bio is on
			 * waiting_flush_bios and it will be handled by a flush
			 * completion or by a bio that can allocate.
			 */
			spin_unlock(&flusher->lock);
			return;
		}

		/* Take and use the spare flush request. */
		flush = flusher->spare_flush;
		flusher->spare_flush = NULL;
	}

	/* We have flushes to start. Capture them in the vdo_flush structure. */
	initialize_flush(flush, vdo);

	spin_unlock(&flusher->lock);

	/* Finish launching the flushes. */
	launch_flush(flush);
}

/**
 * Release a vdo_flush structure that has completed its work. If there are any
 * pending flush requests whose vdo_flush allocation failed, they will be
 * launched by immediately re-using the released vdo_flush. If there is no
 * spare vdo_flush, the released structure will become the spare. Otherwise,
 * the vdo_flush will be freed.
 *
 * @param flush  The completed flush structure to re-use or free
 **/
static void release_flush(struct vdo_flush *flush)
{
	bool relaunch_flush = false;
	struct flusher *flusher = flush->completion.vdo->flusher;

	spin_lock(&flusher->lock);
	if (bio_list_empty(&flusher->waiting_flush_bios)) {
		/* Nothing needs to be started.  Save one spare flush request. */
		if (flusher->spare_flush == NULL) {
			/*
			 * Make the new spare all zero, just like a newly
			 * allocated one.
			 */
			memset(flush, 0, sizeof(*flush));
			flusher->spare_flush = flush;
			flush = NULL;
		}
	} else {
		/* We have flushes to start. Capture them in a flush request. */
		initialize_flush(flush, flusher->vdo);
		relaunch_flush = true;
	}
	spin_unlock(&flusher->lock);

	if (relaunch_flush) {
		/* Finish launching the flushes. */
		launch_flush(flush);
		return;
	}

	if (flush != NULL) {
		UDS_FREE(flush);
	}
}

/**
 * Function called to complete and free a flush request, registered in
 * vdo_complete_flush().
 *
 * @param completion  The flush request
 **/
static void vdo_complete_flush_callback(struct vdo_completion *completion)
{
	struct vdo_flush *flush = completion_as_vdo_flush(completion);
	struct vdo *vdo = completion->vdo;
	struct bio *bio;

	while ((bio = bio_list_pop(&flush->bios)) != NULL) {
		/*
		 * We're not acknowledging this bio now, but we'll never touch
		 * it again, so this is the last chance to account for it.
		 */
		vdo_count_bios(&vdo->stats.bios_acknowledged, bio);

		/* Update the device, and send it on down... */
		bio_set_dev(bio, get_vdo_backing_device(vdo));
		atomic64_inc(&vdo->stats.flush_out);
		submit_bio_noacct(bio);
	}


	/*
	 * Release the flush structure, freeing it, re-using it as the spare,
	 * or using it to launch any flushes that had to wait when allocations
	 * failed.
	 */
	release_flush(flush);
}

/**
 * Select the bio queue on which to finish a flush request.
 *
 * @param flusher  The flusher finishing the request
 **/
static thread_id_t select_bio_queue(struct flusher *flusher)
{
	struct vdo *vdo = flusher->vdo;
	zone_count_t bio_threads
		= flusher->vdo->thread_config->bio_thread_count;
	int interval;

	if (bio_threads == 1) {
		return vdo->thread_config->bio_threads[0];
	}

	interval = vdo->device_config->thread_counts.bio_rotation_interval;
	if (flusher->flush_count == interval) {
		flusher->flush_count = 1;
		flusher->bio_queue_rotor = ((flusher->bio_queue_rotor + 1)
					    % bio_threads);
	} else {
		flusher->flush_count++;
	}

	return vdo->thread_config->bio_threads[flusher->bio_queue_rotor];
}

/**
 * Complete and free a vdo flush request.
 *
 * @param flush  The flush request
 **/
static void vdo_complete_flush(struct vdo_flush *flush)
{
	struct vdo_completion *completion = &flush->completion;

	vdo_prepare_completion(completion,
			       vdo_complete_flush_callback,
			       vdo_complete_flush_callback,
			       select_bio_queue(completion->vdo->flusher),
			       NULL);
	vdo_enqueue_completion_with_priority(completion, BIO_Q_FLUSH_PRIORITY);
}

/**
 * Initiate a drain.
 *
 * Implements vdo_admin_initiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	check_for_drain_complete(container_of(state, struct flusher, state));
}

/**
 * Drain the flusher by preventing any more VIOs from entering the flusher and
 * then flushing. The flusher will be left in the suspended state.
 *
 * @param flusher     The flusher to drain
 * @param completion  The completion to finish when the flusher has drained
 **/
void drain_vdo_flusher(struct flusher *flusher,
		       struct vdo_completion *completion)
{
	assert_on_flusher_thread(flusher, __func__);
	start_vdo_draining(&flusher->state,
			   VDO_ADMIN_STATE_SUSPENDING,
			   completion,
			   initiate_drain);
}

/**
 * Resume a flusher which has been suspended.
 *
 * @param flusher  The flusher to resume
 * @param parent   The completion to finish when the flusher has resumed
 **/
void resume_vdo_flusher(struct flusher *flusher, struct vdo_completion *parent)
{
	assert_on_flusher_thread(flusher, __func__);
	vdo_finish_completion(parent,
			      resume_vdo_if_quiescent(&flusher->state));
}
