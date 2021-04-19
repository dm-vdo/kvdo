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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/flush.c#37 $
 */

#include "flush.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

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
	assert_completion_type(completion->type, FLUSH_NOTIFICATION_COMPLETION);
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
		= get_packer_zone_thread(get_thread_config(vdo));
	initialize_vdo_completion(&vdo->flusher->completion, vdo,
				  FLUSH_NOTIFICATION_COMPLETION);
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_flusher(struct flusher **flusher_ptr)
{
	struct flusher *flusher;

	if (*flusher_ptr == NULL) {
		return;
	}

	flusher = *flusher_ptr;
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
	ASSERT_LOG_ONLY((get_callback_thread_id() == flusher->thread_id),
			"finish_notification() called from flusher thread");

	waiter = dequeue_next_waiter(&flusher->notifiers);
	result = enqueue_waiter(&flusher->pending_flushes, waiter);
	if (result != VDO_SUCCESS) {
		struct vdo_flush *flush = waiter_as_flush(waiter);
		enter_read_only_mode(flusher->vdo->read_only_notifier, result);
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
	increment_packer_flush_generation(flusher->vdo->packer);
	launch_callback(completion, finish_notification, flusher->thread_id);
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
	increment_flush_generation(flusher->logical_zone_to_notify,
				   flusher->notify_generation);
	flusher->logical_zone_to_notify =
		get_next_logical_zone(flusher->logical_zone_to_notify);
	if (flusher->logical_zone_to_notify == NULL) {
		launch_callback(completion, flush_packer_callback,
			        flusher->thread_id);
		return;
	}

	launch_callback(completion, increment_generation,
		        get_logical_zone_thread_id(flusher->logical_zone_to_notify));
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
	flusher->logical_zone_to_notify =
		get_logical_zone(flusher->vdo->logical_zones, 0);
	flusher->completion.requeue = true;
	launch_callback(&flusher->completion, increment_generation,
		        get_logical_zone_thread_id(flusher->logical_zone_to_notify));
}

/**********************************************************************/
void flush_vdo(struct vdo *vdo, struct vdo_flush *flush)
{
	struct flusher *flusher = vdo->flusher;
	bool may_notify;
	int result;

	ASSERT_LOG_ONLY((get_callback_thread_id() == flusher->thread_id),
			"flush_vdo() called from flusher thread");

	flush->flush_generation = flusher->flush_generation++;
	may_notify = !has_waiters(&flusher->notifiers);

	result = enqueue_waiter(&flusher->notifiers, &flush->waiter);
	if (result != VDO_SUCCESS) {
		enter_read_only_mode(vdo->read_only_notifier, result);
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

	ASSERT_LOG_ONLY((get_callback_thread_id() == flusher->thread_id),
			"complete_vdo_flushes() called from flusher thread");

	for (zone = get_logical_zone(flusher->vdo->logical_zones, 0);
	     zone != NULL; zone = get_next_logical_zone(zone)) {
		sequence_number_t oldest_in_zone =
			get_oldest_locked_generation(zone);
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
	log_info("struct flusher");
	log_info("  flush_generation=%llu first_unacknowledged_generation=%llu",
		 flusher->flush_generation,
		 flusher->first_unacknowledged_generation);
	log_info("  notifiers queue is %s; pending_flushes queue is %s",
		 (has_waiters(&flusher->notifiers) ? "not empty" : "empty"),
		 (has_waiters(&flusher->pending_flushes) ? "not empty" : "empty"));
}
