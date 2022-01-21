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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/ioSubmitter.c#32 $
 */

#include "ioSubmitter.h"

#include <linux/version.h>

#include "memoryAlloc.h"
#include "permassert.h"

#include "atomicStats.h"
#include "bio.h"
#include "dataKVIO.h"
#include "logger.h"
#include "vdoInternal.h"

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
	unsigned int bio_queue_rotor;
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
	.action_table = {

			{ .name = "bio_compressed_data",
			  .code = BIO_Q_ACTION_COMPRESSED_DATA,
			  .priority = 0 },
			{ .name = "bio_data",
			  .code = BIO_Q_ACTION_DATA,
			  .priority = 0 },
			{ .name = "bio_flush",
			  .code = BIO_Q_ACTION_FLUSH,
			  .priority = 2 },
			{ .name = "bio_high",
			  .code = BIO_Q_ACTION_HIGH,
			  .priority = 2 },
			{ .name = "bio_metadata",
			  .code = BIO_Q_ACTION_METADATA,
			  .priority = 1 },
			{ .name = "bio_verify",
			  .code = BIO_Q_ACTION_VERIFY,
			  .priority = 1 },
		},
};

/**
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an io_submitter bio thread.
 **/
static void assert_running_in_bio_queue(void)
{
	ASSERT_LOG_ONLY(!in_interrupt(), "not in interrupt context");
	ASSERT_LOG_ONLY(strnstr(current->comm, "bioQ", TASK_COMM_LEN) != NULL,
			"running in bio submission work queue thread");
}

/**
 * Returns the bio_queue_data pointer associated with the current thread.
 * Results are undefined if called from any other thread.
 *
 * @return the bio_queue_data pointer
 **/
static inline struct bio_queue_data *get_current_bio_queue_data(void)
{
	struct bio_queue_data *bio_queue_data =
		(struct bio_queue_data *) get_work_queue_private_data();
	// Does it look like a bio queue thread?
	BUG_ON(bio_queue_data == NULL);
	BUG_ON(bio_queue_data->queue != get_current_work_queue());
	return bio_queue_data;
}

/**********************************************************************/
static inline struct io_submitter *
bio_queue_to_submitter(struct bio_queue_data *bio_queue)
{
	struct bio_queue_data *first_bio_queue = bio_queue -
		bio_queue->queue_number;
	struct io_submitter *submitter = container_of(first_bio_queue,
						      struct io_submitter,
						      bio_queue_data[0]);
	return submitter;
}

/**
 * Return the bio thread number handling the specified physical block
 * number.
*
 * @param io_submitter  The I/O submitter data
 * @param pbn           The physical block number
 *
 * @return read cache zone number
 **/
static unsigned int bio_queue_number_for_pbn(struct io_submitter *io_submitter,
					     physical_block_number_t pbn)
{
	unsigned int bio_queue_index =
		((pbn % (io_submitter->num_bio_queues_used *
			 io_submitter->bio_queue_rotation_interval)) /
		 io_submitter->bio_queue_rotation_interval);

	return bio_queue_index;
}

/**
 * Check that we're running normally (i.e., not in an
 * interrupt-servicing context) in an io_submitter bio thread. Also
 * require that the thread we're running on is the correct one for the
 * supplied physical block number.
 *
 * @param pbn  The PBN that should have been used in thread selection
 **/
static void assert_running_in_bio_queue_for_pbn(physical_block_number_t pbn)
{
	struct bio_queue_data *this_queue;
	struct io_submitter *submitter;
	unsigned int computed_queue_number;

	assert_running_in_bio_queue();

	this_queue = get_current_bio_queue_data();
	submitter = bio_queue_to_submitter(this_queue);
	computed_queue_number = bio_queue_number_for_pbn(submitter, pbn);
	ASSERT_LOG_ONLY(this_queue->queue_number == computed_queue_number,
			"running in correct bio queue (%u vs %u) for PBN %llu",
			this_queue->queue_number,
			computed_queue_number,
			pbn);
}

/**
 * Determines which bio counter to use
 *
 * @param vio  the vio associated with the bio
 * @param bio  the bio to count
 */
static void count_all_bios(struct vio *vio, struct bio *bio)
{
	struct atomic_statistics *stats = &vio->vdo->stats;
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
 * Update stats and tracing info, then submit the supplied bio to the
 * OS for processing.
 *
 * @param vio       The vio associated with the bio
 * @param bio       The bio to submit to the OS
 **/
static void send_bio_to_device(struct vio *vio,
			       struct bio *bio)
{
	assert_running_in_bio_queue_for_pbn(vio->physical);
	atomic64_inc(&vio->vdo->stats.bios_submitted);
	count_all_bios(vio, bio);

	bio_set_dev(bio, get_vdo_backing_device(vio->vdo));
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
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
	assert_running_in_bio_queue();
	// XXX Should we call finish_bio_queue for the biomap case on old
	// kernels?
	if (is_data_vio(vio)) {
		// We need to make sure to do two things here:
		// 1. Use each bio's vio when submitting. Any other vio is
		// not safe
		// 2. Detach the bio list from the vio before submitting,
		// because it could get reused/free'd up before all bios
		// are submitted.
		struct bio_queue_data *bio_queue_data =
			get_work_queue_private_data();
		struct bio *bio = NULL;

		mutex_lock(&bio_queue_data->lock);
		if (!bio_list_empty(&vio->bios_merged)) {
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(vio->bios_merged.head));
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(vio->bios_merged.tail));
		}

		bio = vio->bios_merged.head;
		bio_list_init(&vio->bios_merged);
		mutex_unlock(&bio_queue_data->lock);
		// Somewhere in the list we'll be submitting the current
		// vio, so drop our handle on it now.
		vio = NULL;

		while (bio != NULL) {
			struct vio *vio_bio = bio->bi_private;
			struct bio *next = bio->bi_next;

			bio->bi_next = NULL;
			send_bio_to_device(vio_bio,
					   bio);
			bio = next;
		}
	} else {
		send_bio_to_device(vio,
				   vio->bio);
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
 * @param back_merge  Set to true for a back merge, false for a front merge
 *
 * @return the vio to merge to, NULL if no merging is possible
 */
static struct vio *get_mergeable_locked(struct int_map *map,
					struct vio *vio,
					bool back_merge)
{
	struct bio *bio = vio->bio;
	sector_t merge_sector = get_bio_sector(bio);
	struct vio *vio_merge;

	if (back_merge) {
		merge_sector -= VDO_SECTORS_PER_BLOCK;
        } else {
		merge_sector += VDO_SECTORS_PER_BLOCK;
	}

	vio_merge = int_map_get(map, merge_sector);

	if (vio_merge == NULL) {
		return NULL;
	}

	if (!are_work_item_actions_equal(work_item_from_vio(vio),
					 work_item_from_vio(vio_merge))) {
		return NULL;
	}

	if (bio_data_dir(bio) != bio_data_dir(vio_merge->bio)) {
		return NULL;
	}

	if (bio_list_empty(&vio_merge->bios_merged)) {
		return NULL;
	}

	if (back_merge) {
		if (get_bio_sector(vio_merge->bios_merged.tail) !=
		    merge_sector) {
			return NULL;
		}
        } else if (get_bio_sector(vio_merge->bios_merged.head) !=
		    merge_sector) {
			return NULL;
	}

	return vio_merge;
}

/**********************************************************************/
static inline unsigned int advance_bio_rotor(struct io_submitter *bio_data)
{
	unsigned int index = bio_data->bio_queue_rotor++ %
			     (bio_data->num_bio_queues_used *
			      bio_data->bio_queue_rotation_interval);
	index /= bio_data->bio_queue_rotation_interval;
	return index;
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

/**********************************************************************/
static bool try_bio_map_merge(struct bio_queue_data *bio_queue_data,
			      struct vio *vio,
			      struct bio *bio)
{
	int result;
	bool merged = false;
	struct vio *prev_vio, *next_vio;

	mutex_lock(&bio_queue_data->lock);
	prev_vio = get_mergeable_locked(bio_queue_data->map, vio, true);
	next_vio = get_mergeable_locked(bio_queue_data->map, vio, false);
	if (prev_vio == next_vio) {
		next_vio = NULL;
	}

	if ((prev_vio == NULL) && (next_vio == NULL)) {
		// no merge. just add to bio_queue
		result = int_map_put(bio_queue_data->map, get_bio_sector(bio),
				     vio, true, NULL);
		// We don't care about failure of int_map_put in this case.
		result = result;
		mutex_unlock(&bio_queue_data->lock);
	} else {
		if (next_vio == NULL) {
			// Only prev. merge to prev's tail
			result = merge_to_prev_tail(bio_queue_data->map,
						    vio, prev_vio);
		} else {
			// Only next. merge to next's head
			result = merge_to_next_head(bio_queue_data->map,
						    vio, next_vio);
		}

		// We don't care about failure of int_map_put in this case.
		result = result;
		mutex_unlock(&bio_queue_data->lock);
		merged = true;
	}

	return merged;
}

/**********************************************************************/
static struct bio_queue_data *
bio_queue_data_for_pbn(struct io_submitter *io_submitter,
		       physical_block_number_t pbn)
{
	unsigned int bio_queue_index =
		bio_queue_number_for_pbn(io_submitter, pbn);
	return &io_submitter->bio_queue_data[bio_queue_index];
}

/**********************************************************************/
void vdo_submit_bio(struct bio *bio, enum bio_q_action action)
{
	struct vio *vio = bio->bi_private;
	struct bio_queue_data *bio_queue_data =
		bio_queue_data_for_pbn(vio->vdo->io_submitter, vio->physical);
	bool merged = false;

	setup_vio_work(vio, process_bio_map, bio->bi_end_io, action);

	bio->bi_next = NULL;
	bio_list_init(&vio->bios_merged);
	bio_list_add(&vio->bios_merged, bio);

	/*
	 * Try to use the bio map to submit this bio earlier if we're already
	 * sending IO for an adjacent block. If we can't use an existing
	 * pending bio, enqueue an operation to run in a bio submission thread
	 * appropriate to the indicated physical block number.
	 */

	if (is_data_vio(vio)) {
		merged = try_bio_map_merge(bio_queue_data, vio, bio);
	}
	if (!merged) {
		enqueue_vio_work(bio_queue_data->queue, vio);
	}
}

/**********************************************************************/
static int initialize_bio_queue(struct bio_queue_data *bio_queue_data,
				const char *thread_name_prefix,
				const char *queue_name,
				unsigned int queue_number,
				struct vdo *vdo)
{
	bio_queue_data->queue_number = queue_number;

	return make_work_queue(thread_name_prefix,
			       queue_name,
			       vdo,
			       bio_queue_data,
			       &bio_queue_type,
			       1,
			       NULL,
			       &bio_queue_data->queue);
}

/**********************************************************************/
int make_vdo_io_submitter(const char *thread_name_prefix,
			  unsigned int thread_count,
			  unsigned int rotation_interval,
			  unsigned int max_requests_active,
			  struct vdo *vdo,
			  struct io_submitter **io_submitter_ptr)
{
	char queue_name[MAX_QUEUE_NAME_LEN];
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
		snprintf(queue_name, sizeof(queue_name), "bioQ%u", i);

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

		result = initialize_bio_queue(bio_queue_data,
					      thread_name_prefix,
					      queue_name,
					      i,
					      vdo);
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
		free_work_queue(UDS_FORGET(io_submitter->bio_queue_data[i].queue));
		free_int_map(UDS_FORGET(io_submitter->bio_queue_data[i].map));
	}
	UDS_FREE(io_submitter);
}

/**********************************************************************/
void vdo_dump_bio_work_queue(struct io_submitter *io_submitter)
{
	int i;

	for (i = 0; i < io_submitter->num_bio_queues_used; i++) {
		dump_work_queue(io_submitter->bio_queue_data[i].queue);
	}
}


/**********************************************************************/
void vdo_enqueue_bio_work_item(struct io_submitter *io_submitter,
			       struct vdo_work_item *work_item)
{
	unsigned int bio_queue_index = advance_bio_rotor(io_submitter);

	enqueue_work_queue(io_submitter->bio_queue_data[bio_queue_index].queue,
			   work_item);
}
