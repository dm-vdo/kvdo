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

#include "ioSubmitter.h"

#include <linux/version.h>

#include "memoryAlloc.h"
#include "permassert.h"

#include "atomic-stats.h"
#include "bio.h"
#include "dataKVIO.h"
#include "logger.h"
#include "types.h"
#include "vdo.h"

/*
 * Submission of bio operations to the underlying storage device will
 * go through a separate work queue thread (or more than one) to
 * prevent blocking in other threads if the storage device has a full
 * queue. The plug structure allows that thread to do better batching
 * of requests to make the I/O more efficient.
 *
 * When multiple worker threads are used, a thread is chosen for a
 * I/O operation submission based on the PBN, so a given PBN will
 * consistently wind up on the same thread. Flush operations are
 * assigned round-robin.
 *
 * The map (protected by the mutex) collects pending I/O operations so
 * that the worker thread can reorder them to try to encourage I/O
 * request merging in the request queue underneath.
 */
struct bio_queue_data {
	struct vdo_work_queue *queue;
	struct blk_plug plug;
	struct int_map *map;
	struct mutex lock;
	unsigned int queue_number;
};

struct io_submitter {
	unsigned int num_bio_queues_used;
	unsigned int bio_queue_rotation_interval;
	struct bio_queue_data bio_queue_data[];
};

/**********************************************************************/
static void start_bio_queue(void *ptr)
{
	struct bio_queue_data *bio_queue_data = (struct bio_queue_data *) ptr;

	blk_start_plug(&bio_queue_data->plug);
}

/**********************************************************************/
static void finish_bio_queue(void *ptr)
{
	struct bio_queue_data *bio_queue_data = (struct bio_queue_data *) ptr;

	blk_finish_plug(&bio_queue_data->plug);
}

static const struct vdo_work_queue_type bio_queue_type = {
	.start = start_bio_queue,
	.finish = finish_bio_queue,
	.max_priority = BIO_Q_MAX_PRIORITY,
	.default_priority = BIO_Q_DATA_PRIORITY,
};

/**
 * Determines which bio counter to use
 *
 * @param vio  the vio associated with the bio
 * @param bio  the bio to count
 */
static void count_all_bios(struct vio *vio, struct bio *bio)
{
	struct atomic_statistics *stats = &get_vdo_from_vio(vio)->stats;

	if (is_data_vio(vio)) {
		vdo_count_bios(&stats->bios_out, bio);
		return;
	}

	vdo_count_bios(&stats->bios_meta, bio);
	if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		vdo_count_bios(&stats->bios_journal, bio);
	} else if (vio->type == VIO_TYPE_BLOCK_MAP) {
		vdo_count_bios(&stats->bios_page_cache, bio);
	}
}

/**
 * Assert that a vio is in the correct bio zone and not in interrupt context.
 *
 * @param vio  The vio to check
 **/
static void assert_in_bio_zone(struct vio *vio)
{
	ASSERT_LOG_ONLY(!in_interrupt(), "not in interrupt context");
	assert_vio_in_bio_zone(vio);
}

/**
 * Update stats and tracing info, then submit the supplied bio to the
 * OS for processing.
 *
 * @param vio       The vio associated with the bio
 * @param bio       The bio to submit to the OS
 **/
static void send_bio_to_device(struct vio *vio,
			       struct bio *bio)
{
	struct vdo *vdo = get_vdo_from_vio(vio);
	assert_in_bio_zone(vio);
	atomic64_inc(&vdo->stats.bios_submitted);
	count_all_bios(vio, bio);

	/*
	 * vdo_get_callback_thread_id() can't be called from interrupt context,
	 * in which, bio_end_io functions are often called. By setting the
	 * requeue flag, vio launches from them will skip calling them to check
	 * if they need to requeue.
         */
	vio_as_completion(vio)->requeue = true;

	bio_set_dev(bio, get_vdo_backing_device(vdo));
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	generic_make_request(bio);
#else
	submit_bio_noacct(bio);
#endif
}

/**********************************************************************/
static sector_t get_bio_sector(struct bio *bio)
{
	return bio->bi_iter.bi_sector;
}

/**
 * Submits a bio to the underlying block device.  May block if the
 * device is busy.
 *
 * For normal data, vio->bios_merged is the list of all bios collected
 * together in this group; all of them get submitted.
 *
 * @param item  The work item in the vio "owning" the head of the
 *		bio_list to be submitted.
 **/
static void process_bio_map(struct vdo_work_item *item)
{
	struct vio *vio = work_item_as_vio(item);

	send_bio_to_device(vio, vio->bio);
}

/**
 * Extract the list of bios to submit from a vio. The list will always contain
 * at least one entry (the bio for the vio on which it is called), but other
 * bios may have been merged with it as well.
 *
 * @param vio  The vio submitting I/O
 *
 * @return bio  The head of the bio list to submit
 **/
static struct bio *get_bio_list(struct vio *vio)
{
	struct bio_queue_data *bio_queue_data = get_work_queue_private_data();
	struct bio *bio;

	assert_in_bio_zone(vio);

	mutex_lock(&bio_queue_data->lock);
	int_map_remove(bio_queue_data->map,
		       get_bio_sector(vio->bios_merged.head));
	int_map_remove(bio_queue_data->map,
		       get_bio_sector(vio->bios_merged.tail));
	bio = vio->bios_merged.head;
	bio_list_init(&vio->bios_merged);
	mutex_unlock(&bio_queue_data->lock);

	return bio;
}

/**********************************************************************/
void process_data_vio_io(struct vdo_completion *completion)
{
	struct bio *bio, *next;

	for (bio = get_bio_list(as_vio(completion)); bio != NULL; bio = next) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		send_bio_to_device((struct vio *) bio->bi_private, bio);
	}
}

/**
 * This function will attempt to find an already queued bio that the current
 * bio can be merged with. There are two types of merging possible, forward
 * and backward, which are distinguished by a flag that uses kernel
 * elevator terminology.
 *
 * @param map         The bio map to use for merging
 * @param vio         The vio we want to merge
 * @param merge_type  The type of merging we want to try
 *
 * @return the vio to merge to, NULL if no merging is possible
 */
static struct vio *get_mergeable_locked(struct int_map *map,
					struct vio *vio,
					unsigned int merge_type)
{
	struct bio *bio = vio->bio;
	sector_t merge_sector = get_bio_sector(bio);
	struct vio *vio_merge;

	switch (merge_type) {
	case ELEVATOR_BACK_MERGE:
		merge_sector -= VDO_SECTORS_PER_BLOCK;
		break;
	case ELEVATOR_FRONT_MERGE:
		merge_sector += VDO_SECTORS_PER_BLOCK;
		break;
	}

	vio_merge = int_map_get(map, merge_sector);

	if (vio_merge == NULL) {
		return NULL;
	}

	if (work_item_from_vio(vio)->priority
	    != work_item_from_vio(vio_merge)->priority) {
		return NULL;
	}

	if (bio_data_dir(bio) != bio_data_dir(vio_merge->bio)) {
		return NULL;
	}

	if (bio_list_empty(&vio_merge->bios_merged)) {
		return NULL;
	}

	switch (merge_type) {
	case ELEVATOR_BACK_MERGE:
		if (get_bio_sector(vio_merge->bios_merged.tail) !=
		    merge_sector) {
			return NULL;
		}
		break;

	case ELEVATOR_FRONT_MERGE:
		if (get_bio_sector(vio_merge->bios_merged.head) !=
		    merge_sector) {
			return NULL;
		}
		break;
	}

	return vio_merge;
}

/**********************************************************************/
static int merge_to_prev_tail(struct int_map *bio_map,
			      struct vio *vio,
			      struct vio *prev_vio)
{
	int result;

	int_map_remove(bio_map, get_bio_sector(prev_vio->bios_merged.tail));
	bio_list_merge(&prev_vio->bios_merged, &vio->bios_merged);
	result = int_map_put(bio_map,
			     get_bio_sector(prev_vio->bios_merged.head),
			     prev_vio, true, NULL);
	result = int_map_put(bio_map,
			     get_bio_sector(prev_vio->bios_merged.tail),
			     prev_vio, true, NULL);
	return result;
}

/**********************************************************************/
static int merge_to_next_head(struct int_map *bio_map,
			      struct vio *vio,
			      struct vio *next_vio)
{
	int result;

	// Handle "next merge" and "gap fill" cases the same way so as to
	// reorder bios in a way that's compatible with using funnel queues
	// in work queues.  This avoids removing an existing work item.
	int_map_remove(bio_map, get_bio_sector(next_vio->bios_merged.head));
	bio_list_merge_head(&next_vio->bios_merged, &vio->bios_merged);
	result = int_map_put(bio_map,
			     get_bio_sector(next_vio->bios_merged.head),
			     next_vio, true, NULL);
	result = int_map_put(bio_map,
			     get_bio_sector(next_vio->bios_merged.tail),
			     next_vio, true, NULL);
	return result;
}

/**
 * Attempt to merge a vio's bio with other pending I/Os. Currently this is only
 * used for data_vios, but is broken out for future use with metadata vios.
 *
 * @param vio  The vio to merge
 *
 * @return whether or not the vio was merged
 **/
static bool try_bio_map_merge(struct vio *vio)
{
	int result;
	bool merged = true;
	struct bio *bio = vio->bio;
	struct vio *prev_vio, *next_vio;
	struct vdo *vdo = get_vdo_from_vio(vio);
	struct bio_queue_data *bio_queue_data
		= &vdo->io_submitter->bio_queue_data[vio->bio_zone];

	bio->bi_next = NULL;
	bio_list_init(&vio->bios_merged);
	bio_list_add(&vio->bios_merged, bio);

	mutex_lock(&bio_queue_data->lock);
	prev_vio = get_mergeable_locked(bio_queue_data->map, vio,
					ELEVATOR_BACK_MERGE);
	next_vio = get_mergeable_locked(bio_queue_data->map, vio,
					ELEVATOR_FRONT_MERGE);
	if (prev_vio == next_vio) {
		next_vio = NULL;
	}

	if ((prev_vio == NULL) && (next_vio == NULL)) {
		// no merge. just add to bio_queue
		merged = false;
		result = int_map_put(bio_queue_data->map, get_bio_sector(bio),
				     vio, true, NULL);
	} else if (next_vio == NULL) {
		// Only prev. merge to prev's tail
		result = merge_to_prev_tail(bio_queue_data->map,
					    vio,
					    prev_vio);
	} else {
		// Only next. merge to next's head
		result = merge_to_next_head(bio_queue_data->map,
					    vio,
					    next_vio);
	}

	mutex_unlock(&bio_queue_data->lock);

	// We don't care about failure of int_map_put in this case.
	ASSERT_LOG_ONLY(result == UDS_SUCCESS, "bio map insertion succeeds");
	return merged;
}

/**********************************************************************/
void submit_data_vio_io(struct data_vio *data_vio)
{
	if (try_bio_map_merge(data_vio_as_vio(data_vio))) {
		return;
	}

	launch_data_vio_bio_zone_callback(data_vio,
					  process_data_vio_io);

}

/**********************************************************************/
void vdo_submit_bio(struct bio *bio, enum vdo_work_item_priority priority)
{
	struct vio *vio = bio->bi_private;
	struct io_submitter *submitter = get_vdo_from_vio(vio)->io_submitter;

	bio->bi_next = NULL;
	setup_vio_work(vio, process_bio_map, priority);
	enqueue_vio_work(submitter->bio_queue_data[vio->bio_zone].queue, vio);
}

/**********************************************************************/
int make_vdo_io_submitter(const char *thread_name_prefix,
			  unsigned int thread_count,
			  unsigned int rotation_interval,
			  unsigned int max_requests_active,
			  struct vdo *vdo,
			  struct io_submitter **io_submitter_ptr)
{
	unsigned int i;
	struct io_submitter *io_submitter;
	int result = UDS_ALLOCATE_EXTENDED(struct io_submitter,
					   thread_count,
					   struct bio_queue_data,
					   "bio submission data",
					   &io_submitter);
	if (result != UDS_SUCCESS) {
		return result;
	}


	io_submitter->bio_queue_rotation_interval = rotation_interval;

	// Setup for each bio-submission work queue
	for (i = 0; i < thread_count; i++) {
		struct bio_queue_data *bio_queue_data =
			&io_submitter->bio_queue_data[i];

		mutex_init(&bio_queue_data->lock);
		/*
		 * One I/O operation per request, but both first &
		 * last sector numbers.
		 *
		 * If requests are assigned to threads round-robin,
		 * they should be distributed quite evenly. But if
		 * they're assigned based on PBN, things can sometimes
		 * be very uneven. So for now, we'll assume that all
		 * requests *may* wind up on one thread, and thus all
		 * in the same map.
		 */
		result = make_int_map(max_requests_active * 2, 0,
				      &bio_queue_data->map);
		if (result != 0) {
			// Clean up the partially initialized bio-queue
			// entirely and indicate that initialization failed.
			uds_log_error("bio map initialization failed %d",
				      result);
			cleanup_vdo_io_submitter(io_submitter);
			free_vdo_io_submitter(io_submitter);
			return result;
		}

		bio_queue_data->queue_number = i;
		result = make_vdo_thread(vdo,
					 thread_name_prefix,
					 vdo->thread_config->bio_threads[i],
					 &bio_queue_type,
					 1,
					 (void **) &bio_queue_data);
		if (result != VDO_SUCCESS) {
			// Clean up the partially initialized bio-queue
			// entirely and indicate that initialization failed.
			free_int_map(UDS_FORGET(bio_queue_data->map));
			uds_log_error("bio queue initialization failed %d",
				      result);
			cleanup_vdo_io_submitter(io_submitter);
			free_vdo_io_submitter(io_submitter);
			return result;
		}

		bio_queue_data->queue
			= vdo->threads[vdo->thread_config->bio_threads[i]].queue;
		io_submitter->num_bio_queues_used++;
	}

	*io_submitter_ptr = io_submitter;

	return VDO_SUCCESS;
}

/**********************************************************************/
void cleanup_vdo_io_submitter(struct io_submitter *io_submitter)
{
	int i;

	if (io_submitter == NULL) {
		return;
	}

	for (i = io_submitter->num_bio_queues_used - 1; i >= 0; i--) {
		finish_work_queue(io_submitter->bio_queue_data[i].queue);
	}
}

/**********************************************************************/
void free_vdo_io_submitter(struct io_submitter *io_submitter)
{
	int i;

	if (io_submitter == NULL) {
		return;
	}

	for (i = io_submitter->num_bio_queues_used - 1; i >= 0; i--) {
		io_submitter->num_bio_queues_used--;
		// destroy_vdo() will free the work queue, so just give up our
		// reference to it.
		UDS_FORGET(io_submitter->bio_queue_data[i].queue);
		free_int_map(UDS_FORGET(io_submitter->bio_queue_data[i].map));
	}
	UDS_FREE(io_submitter);
}
