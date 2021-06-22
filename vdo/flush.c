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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/flush.c#49 $
 */

#include "flush.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "ioSubmitter.h"
#include "kernelLayer.h"
#include "kernelVDO.h"

#include "blockAllocator.h"
#include "completion.h"
#include "logicalZone.h"
#include "numUtils.h"
#include "readOnlyNotifier.h"
#include "slabDepot.h"
#include "vdoInternal.h"

struct flusher {
	struct vdo_completion completion;
	/** The vdo to which this flusher belongs */
	struct vdo *vdo;
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
	/** When the longest waiting flush bio arrived */
	uint64_t flush_arrival_jiffies;
};

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
				   FLUSH_NOTIFICATION_COMPLETION);
	return container_of(completion, struct flusher, completion);
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

/**********************************************************************/
int make_vdo_flusher(struct vdo *vdo)
{
	int result = ALLOCATE(1, struct flusher, __func__, &vdo->flusher);
	if (result != VDO_SUCCESS) {
		return result;
	}

	vdo->flusher->vdo = vdo;
	vdo->flusher->thread_id
		= vdo_get_packer_zone_thread(get_vdo_thread_config(vdo));
	initialize_vdo_completion(&vdo->flusher->completion, vdo,
				  FLUSH_NOTIFICATION_COMPLETION);

	spin_lock_init(&vdo->flusher->lock);
	bio_list_init(&vdo->flusher->waiting_flush_bios);
	result = ALLOCATE(1, struct vdo_flush, __func__,
			  &vdo->flusher->spare_flush);
	return result;
}

/**********************************************************************/
void free_vdo_flusher(struct flusher **flusher_ptr)
{
	struct flusher *flusher = *flusher_ptr;
	if (flusher == NULL) {
		return;
	}

	FREE(flusher->spare_flush);
	FREE(flusher);
	*flusher_ptr = NULL;
}

/**********************************************************************/
thread_id_t get_vdo_flusher_thread_id(struct flusher *flusher)
{
	return flusher->thread_id;
}

/**********************************************************************/
static void notify_flush(struct flusher *flusher);

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
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == flusher->thread_id),
			"finish_notification() called from flusher thread");

	waiter = dequeue_next_waiter(&flusher->notifiers);
	result = enqueue_waiter(&flusher->pending_flushes, waiter);
	if (result != VDO_SUCCESS) {
		struct vdo_flush *flush = waiter_as_flush(waiter);
		vdo_enter_read_only_mode(flusher->vdo->read_only_notifier,
					 result);
		vdo_complete_flush(&flush);
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
	launch_vdo_completion_callback(completion, finish_notification,
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
	thread_id_t thread_id;
	struct flusher *flusher = as_flusher(completion);
	increment_vdo_logical_zone_flush_generation(flusher->logical_zone_to_notify,
				   		    flusher->notify_generation);
	flusher->logical_zone_to_notify =
		get_next_vdo_logical_zone(flusher->logical_zone_to_notify);
	if (flusher->logical_zone_to_notify == NULL) {
		launch_vdo_completion_callback(completion,
					       flush_packer_callback,
					       flusher->thread_id);
		return;
	}

	thread_id =
		get_vdo_logical_zone_thread_id(flusher->logical_zone_to_notify);
	launch_vdo_completion_callback(completion,
				       increment_generation,
				       thread_id);
}

/**
 * Lauch a flush notification.
 *
 * @param flusher  The flusher doing the notification
 **/
static void notify_flush(struct flusher *flusher)
{
	thread_id_t thread_id;
	struct vdo_flush *flush =
		waiter_as_flush(get_first_waiter(&flusher->notifiers));
	flusher->notify_generation = flush->flush_generation;
	flusher->logical_zone_to_notify =
		get_vdo_logical_zone(flusher->vdo->logical_zones, 0);
	flusher->completion.requeue = true;

	thread_id =
		get_vdo_logical_zone_thread_id(flusher->logical_zone_to_notify);
	launch_vdo_completion_callback(&flusher->completion,
				       increment_generation,
				       thread_id);
}

/**********************************************************************/
void flush_vdo(struct vdo_work_item *item)
{
	struct vdo_flush *flush = container_of(item,
					       struct vdo_flush,
					       work_item);
	struct flusher *flusher = flush->vdo->flusher;
	bool may_notify;
	int result;

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == flusher->thread_id),
			"flush_vdo() called from flusher thread");

	flush->flush_generation = flusher->flush_generation++;
	may_notify = !has_waiters(&flusher->notifiers);

	result = enqueue_waiter(&flusher->notifiers, &flush->waiter);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(flush->vdo->read_only_notifier,
					 result);
		vdo_complete_flush(&flush);
		return;
	}

	if (may_notify) {
		notify_flush(flusher);
	}
}

/**********************************************************************/
void complete_vdo_flushes(struct flusher *flusher)
{
	sequence_number_t oldest_active_generation = UINT64_MAX;
	struct logical_zone *zone;

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == flusher->thread_id),
			"complete_vdo_flushes() called from flusher thread");

	for (zone = get_vdo_logical_zone(flusher->vdo->logical_zones, 0);
	     zone != NULL; zone = get_next_vdo_logical_zone(zone)) {
		sequence_number_t oldest_in_zone =
			get_vdo_logical_zone_oldest_locked_generation(zone);
		oldest_active_generation =
			min(oldest_active_generation, oldest_in_zone);
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
				flusher->first_unacknowledged_generation,
				flush->flush_generation);
		dequeue_next_waiter(&flusher->pending_flushes);
		vdo_complete_flush(&flush);
		flusher->first_unacknowledged_generation++;
	}
}

/**********************************************************************/
void dump_vdo_flusher(const struct flusher *flusher)
{
	uds_log_info("struct flusher");
	uds_log_info("  flush_generation=%llu first_unacknowledged_generation=%llu",
		     flusher->flush_generation,
		     flusher->first_unacknowledged_generation);
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
	flush->vdo = vdo;
	bio_list_init(&flush->bios);
	bio_list_merge(&flush->bios, &vdo->flusher->waiting_flush_bios);
	bio_list_init(&vdo->flusher->waiting_flush_bios);
	flush->arrival_jiffies = vdo->flusher->flush_arrival_jiffies;
}

/**********************************************************************/
static void enqueue_flush(struct vdo_flush *flush)
{
	struct vdo *vdo = flush->vdo;
	setup_work_item(&flush->work_item,
			flush_vdo,
			NULL,
			REQ_Q_ACTION_FLUSH);
	enqueue_vdo_work(vdo,
			 &flush->work_item,
			 vdo_get_packer_zone_thread(get_vdo_thread_config(vdo)));
}

/**********************************************************************/
void launch_vdo_flush(struct vdo *vdo, struct bio *bio)
{
	// Try to allocate a vdo_flush to represent the flush request. If the
	// allocation fails, we'll deal with it later.
	struct vdo_flush *flush = ALLOCATE_NOWAIT(struct vdo_flush, __func__);
	struct flusher *flusher = vdo->flusher;
	spin_lock(&flusher->lock);

	// We have a new bio to start. Add it to the list. If it becomes the
	// only entry on the list, record the time.
	if (bio_list_empty(&flusher->waiting_flush_bios)) {
		flusher->flush_arrival_jiffies = jiffies;
	}

	bio_list_add(&flusher->waiting_flush_bios, bio);

	if (flush == NULL) {
		// The vdo_flush allocation failed. Try to use the spare
		// vdo_flush structure.
		if (flusher->spare_flush == NULL) {
			// The spare is already in use. This bio is on
			// waiting_flush_bios and it will be handled by a flush
			// completion or by a bio that can allocate.
			spin_unlock(&flusher->lock);
			return;
		}

		// Take and use the spare flush request.
		flush = flusher->spare_flush;
		flusher->spare_flush = NULL;
	}

	// We have flushes to start. Capture them in the vdo_flush structure.
	initialize_flush(flush, vdo);

	spin_unlock(&flusher->lock);

	// Finish launching the flushes.
	enqueue_flush(flush);
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
	struct flusher *flusher = flush->vdo->flusher;
	bool relaunch_flush = false;

	spin_lock(&flusher->lock);
	if (bio_list_empty(&flusher->waiting_flush_bios)) {
		// Nothing needs to be started.  Save one spare flush request.
		if (flusher->spare_flush == NULL) {
			// Make the new spare all zero, just like a newly
			// allocated one.
			memset(flush, 0, sizeof(*flush));
			flusher->spare_flush = flush;
			flush = NULL;
		}
	} else {
		// We have flushes to start. Capture them in a flush request.
		initialize_flush(flush, flusher->vdo);
		relaunch_flush = true;
	}
	spin_unlock(&flusher->lock);

	if (relaunch_flush) {
		// Finish launching the flushes.
		enqueue_flush(flush);
		return;
	}

	if (flush != NULL) {
		FREE(flush);
	}
}

/**
 * Function called to complete and free a flush request
 *
 * @param item    The flush-request work item
 **/
static void vdo_complete_flush_work(struct vdo_work_item *item)
{
	struct vdo_flush *flush = container_of(item,
					       struct vdo_flush,
					       work_item);
	struct vdo *vdo = flush->vdo;
	struct bio *bio;

	while ((bio = bio_list_pop(&flush->bios)) != NULL) {
		// We're not acknowledging this bio now, but we'll never touch
		// it again, so this is the last chance to account for it.
		vdo_count_bios(&vdo->stats.bios_acknowledged, bio);

		// Update the device, and send it on down...
		bio_set_dev(bio, get_vdo_backing_device(flush->vdo));
		atomic64_inc(&vdo->stats.flush_out);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
		generic_make_request(bio);
#else
		submit_bio_noacct(bio);
#endif
	}


	// Release the flush structure, freeing it, re-using it as the spare,
	// or using it to launch any flushes that had to wait when allocations
	// failed.
	release_flush(flush);
}

/**********************************************************************/
void vdo_complete_flush(struct vdo_flush **flush_ptr)
{
	struct vdo_flush *flush = *flush_ptr;
	setup_work_item(&flush->work_item,
			vdo_complete_flush_work,
			NULL,
			BIO_Q_ACTION_FLUSH);
	vdo_enqueue_bio_work_item(flush->vdo->io_submitter, &flush->work_item);
}
