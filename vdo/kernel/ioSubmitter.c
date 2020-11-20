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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/ioSubmitter.c#55 $
 */

#include "ioSubmitter.h"

#include <linux/version.h>

#include "memoryAlloc.h"

#include "bio.h"
#include "dataKVIO.h"
#include "kernelLayer.h"
#include "logger.h"

enum {
	/*
	 * Whether to use bio merging code.
	 *
	 * Merging I/O requests in the request queue below us is helpful for
	 * many devices, and VDO does a good job sometimes of shuffling up
	 * the I/O order (too much for some simple I/O schedulers to sort
	 * out) as we deal with dedupe advice etc. The bio map tracks the
	 * yet-to-be-submitted I/O requests by block number so that we can
	 * collect together and submit sequential I/O operations that should
	 * be easy to merge. (So we don't actually *merge* them here, we
	 * just arrange them so that merging can happen.)
	 *
	 * For some devices, merging may not help, and we may want to turn
	 * off this code and save compute/spinlock cycles.
	 */
	USE_BIOMAP = 1,
};

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
	struct kvdo_work_queue *queue;
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

static const struct kvdo_work_queue_type bio_queue_type = {
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
 * @param io_submitter      The I/O submitter data
 * @param pbn               The physical block number
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
	assert_running_in_bio_queue();

	struct bio_queue_data *this_queue = get_current_bio_queue_data();
	struct io_submitter *submitter = bio_queue_to_submitter(this_queue);
	unsigned int computed_queue_number =
		bio_queue_number_for_pbn(submitter, pbn);
	ASSERT_LOG_ONLY(this_queue->queue_number == computed_queue_number,
			"running in correct bio queue (%u vs %u) for PBN %llu",
			this_queue->queue_number,
			computed_queue_number,
			pbn);
}

/**
 * Determines which bio counter to use
 *
 * @param kvio the kvio associated with the bio
 * @param bio  the bio to count
 */
static void count_all_bios(struct kvio *kvio, struct bio *bio)
{
	struct kernel_layer *layer = kvio->layer;

	if (is_data(kvio)) {
		count_bios(&layer->biosOut, bio);
		return;
	}

	count_bios(&layer->biosMeta, bio);
	if (kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		count_bios(&layer->biosJournal, bio);
	} else if (kvio->vio->type == VIO_TYPE_BLOCK_MAP) {
		count_bios(&layer->biosPageCache, bio);
	}
}

/**
 * Update stats and tracing info, then submit the supplied bio to the
 * OS for processing.
 *
 * @param kvio      The kvio associated with the bio
 * @param bio       The bio to submit to the OS
 * @param location  Call site location for tracing
 **/
static void send_bio_to_device(struct kvio *kvio,
			       struct bio *bio,
			       const struct trace_location *location)
{
	assert_running_in_bio_queue_for_pbn(kvio->vio->physical);

	atomic64_inc(&kvio->layer->bios_submitted);
	count_all_bios(kvio, bio);
	kvio_add_trace_record(kvio, location);

	bio_set_dev(bio, get_kernel_layer_bdev(kvio->layer));
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
 * For metadata or if USE_BIOMAP is disabled, kvio->bio_to_submit holds
 * the struct bio pointer to submit to the target device. For normal
 * data when USE_BIOMAP is enabled, kvio->bios_merged is the list of
 * all bios collected together in this group; all of them get
 * submitted. In both cases, the bi_end_io callback is invoked when
 * each I/O operation completes.
 *
 * @param item  The work item in the kvio "owning" either the bio to
 *              submit, or the head of the bio_list to be submitted.
 **/
static void process_bio_map(struct kvdo_work_item *item)
{
	assert_running_in_bio_queue();
	struct kvio *kvio = work_item_as_kvio(item);
	// XXX Should we call finish_bio_queue for the biomap case on old
	// kernels?
	if (USE_BIOMAP && is_data(kvio)) {
		// We need to make sure to do two things here:
		// 1. Use each bio's kvio when submitting. Any other kvio is
		// not safe
		// 2. Detach the bio list from the kvio before submitting,
		// because it could get reused/free'd up before all bios
		// are submitted.
		struct bio_queue_data *bio_queue_data =
			get_work_queue_private_data();
		struct bio *bio = NULL;

		mutex_lock(&bio_queue_data->lock);
		if (!bio_list_empty(&kvio->bios_merged)) {
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(kvio->bios_merged.head));
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(kvio->bios_merged.tail));
		}
		bio = kvio->bios_merged.head;
		bio_list_init(&kvio->bios_merged);
		mutex_unlock(&bio_queue_data->lock);
		// Somewhere in the list we'll be submitting the current
		// "kvio", so drop our handle on it now.
		kvio = NULL;

		while (bio != NULL) {
			struct kvio *kvio_bio = bio->bi_private;
			struct bio *next = bio->bi_next;

			bio->bi_next = NULL;
			send_bio_to_device(kvio_bio,
					   bio,
					   THIS_LOCATION("$F($io)"));
			bio = next;
		}
	} else {
		send_bio_to_device(kvio,
				   kvio->bio_to_submit,
				   THIS_LOCATION("$F($io)"));
	}
}

/**
 * This function will attempt to find an already queued bio that the current
 * bio can be merged with. There are two types of merging possible, forward
 * and backward, which are distinguished by a flag that uses kernel
 * elevator terminology.
 *
 * @param map         The bio map to use for merging
 * @param kvio        The kvio we want to merge
 * @param merge_type  The type of merging we want to try
 *
 * @return the kvio to merge to, NULL if no merging is possible
 */
static struct kvio *get_mergeable_locked(struct int_map *map,
				  struct kvio *kvio,
				  unsigned int merge_type)
{
	struct bio *bio = kvio->bio_to_submit;
	sector_t merge_sector = get_bio_sector(bio);

	switch (merge_type) {
	case ELEVATOR_BACK_MERGE:
		merge_sector -= VDO_SECTORS_PER_BLOCK;
		break;
	case ELEVATOR_FRONT_MERGE:
		merge_sector += VDO_SECTORS_PER_BLOCK;
		break;
	}

	struct kvio *kvio_merge = int_map_get(map, merge_sector);

	if (kvio_merge == NULL) {
		return NULL;
	}

	if (!are_work_item_actions_equal(work_item_from_kvio(kvio),
					 work_item_from_kvio(kvio_merge))) {
		return NULL;
	}

	if (bio_data_dir(bio) != bio_data_dir(kvio_merge->bio_to_submit)) {
		return NULL;
	}

	if (bio_list_empty(&kvio_merge->bios_merged)) {
		return NULL;
	}

	switch (merge_type) {
	case ELEVATOR_BACK_MERGE:
		if (get_bio_sector(kvio_merge->bios_merged.tail)
		    != merge_sector) {
			return NULL;
		}
		break;

	case ELEVATOR_FRONT_MERGE:
		if (get_bio_sector(kvio_merge->bios_merged.head) !=
		    merge_sector) {
			return NULL;
		}
		break;
	}

	return kvio_merge;
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
static bool try_bio_map_merge(struct bio_queue_data *bio_queue_data,
			      struct kvio *kvio,
			      struct bio *bio)
{
	bool merged = false;

	mutex_lock(&bio_queue_data->lock);
	struct kvio *prev_kvio = get_mergeable_locked(bio_queue_data->map,
						      kvio,
						      ELEVATOR_BACK_MERGE);
	struct kvio *next_kvio = get_mergeable_locked(bio_queue_data->map,
						      kvio,
						      ELEVATOR_FRONT_MERGE);
	if (prev_kvio == next_kvio) {
		next_kvio = NULL;
	}
	int result;

	if ((prev_kvio == NULL) && (next_kvio == NULL)) {
		// no merge. just add to bio_queue
		result = int_map_put(bio_queue_data->map, get_bio_sector(bio),
				     kvio, true, NULL);
		// We don't care about failure of int_map_put in this case.
		result = result;
		mutex_unlock(&bio_queue_data->lock);
	} else {
		if (next_kvio == NULL) {
			// Only prev. merge to  prev's tail
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(prev_kvio->bios_merged.tail));
			bio_list_merge(&prev_kvio->bios_merged,
				       &kvio->bios_merged);
			result = int_map_put(bio_queue_data->map,
					     get_bio_sector(prev_kvio->bios_merged.head),
					     prev_kvio, true, NULL);
			result = int_map_put(bio_queue_data->map,
					     get_bio_sector(prev_kvio->bios_merged.tail),
					     prev_kvio, true, NULL);
		} else {
			// Only next. merge to next's head
			//
			// Handle "next merge" and "gap fill" cases the same way
			// so as to reorder bios in a way that's compatible with
			// using funnel queues in work queues.  This avoids
			// removing an existing work item.
			int_map_remove(bio_queue_data->map,
				       get_bio_sector(next_kvio->bios_merged.head));
			bio_list_merge_head(&next_kvio->bios_merged,
					    &kvio->bios_merged);
			result = int_map_put(bio_queue_data->map,
					     get_bio_sector(next_kvio->bios_merged.head),
					     next_kvio, true, NULL);
			result = int_map_put(bio_queue_data->map,
					     get_bio_sector(next_kvio->bios_merged.tail),
					     next_kvio, true, NULL);
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
void vdo_submit_bio(struct bio *bio, bio_q_action action)
{
	struct kvio *kvio = bio->bi_private;

	kvio->bio_to_submit = bio;
	setup_kvio_work(kvio, process_bio_map, (KvdoWorkFunction) bio->bi_end_io,
		      action);

	struct kernel_layer *layer = kvio->layer;
	struct bio_queue_data *bio_queue_data =
		bio_queue_data_for_pbn(layer->io_submitter,
				       kvio->vio->physical);

	kvio_add_trace_record(kvio, THIS_LOCATION("$F($io)"));

	bio->bi_next = NULL;
	bio_list_init(&kvio->bios_merged);
	bio_list_add(&kvio->bios_merged, bio);

	/*
	 * Enabling of MD RAID5 mode optimizes performance for MD RAID5
	 * storage configurations. It clears the bits for sync I/O RW flags on
	 * data block bios and sets the bits for sync I/O RW flags on all
	 * journal-related bios.
	 *
	 * This increases the frequency of full-stripe writes by altering
	 * flags of submitted bios. For workloads with write requests this
	 * increases the likelihood that the MD RAID5 device will update a
	 * full stripe instead of a partial stripe, thereby avoiding making
	 * read requests to the underlying physical storage for purposes of
	 * parity chunk calculations.
	 *
	 * Setting the sync-flag on journal-related bios is expected to reduce
	 * latency on journal updates submitted to an MD RAID5 device.
	 */
	if (layer->device_config->md_raid5_mode_enabled) {
		if (is_data(kvio)) {
			// Clear the bits for sync I/O RW flags on data block
			// bios.
			bio->bi_opf &= ~REQ_SYNC;
		} else if ((kvio->vio->type == VIO_TYPE_RECOVERY_JOURNAL) ||
			   (kvio->vio->type == VIO_TYPE_SLAB_JOURNAL)) {
			// Set the bits for sync I/O RW flags on all
			// journal-related and slab-journal-related bios.
			bio->bi_opf |= REQ_SYNC;
		}
	}

	/*
	 * Try to use the bio map to submit this bio earlier if we're already
	 * sending IO for an adjacent block. If we can't use an existing
	 * pending bio, enqueue an operation to run in a bio submission thread
	 * appropriate to the indicated physical block number.
	 */

	bool merged = false;

	if (USE_BIOMAP && is_data(kvio)) {
		merged = try_bio_map_merge(bio_queue_data, kvio, bio);
	}
	if (!merged) {
		enqueue_kvio_work(bio_queue_data->queue, kvio);
	}
}

/**********************************************************************/
static int initialize_bio_queue(struct bio_queue_data *bio_queue_data,
				const char *thread_name_prefix,
				const char *queue_name,
				unsigned int queue_number,
				struct kernel_layer *layer)
{
	bio_queue_data->queue_number = queue_number;

	return make_work_queue(thread_name_prefix, queue_name,
			       &layer->wq_directory, layer, bio_queue_data,
			       &bio_queue_type, 1, NULL,
			       &bio_queue_data->queue);
}

/**********************************************************************/
int make_io_submitter(const char *thread_name_prefix,
		      unsigned int thread_count,
		      unsigned int rotation_interval,
		      unsigned int max_requests_active,
		      struct kernel_layer *layer,
		      struct io_submitter **io_submitter_ptr)
{
	struct io_submitter *io_submitter;
	int result = ALLOCATE_EXTENDED(struct io_submitter,
				       thread_count,
				       struct bio_queue_data,
				       "bio submission data",
				       &io_submitter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// Setup for each bio-submission work queue
	char queue_name[MAX_QUEUE_NAME_LEN];

	io_submitter->bio_queue_rotation_interval = rotation_interval;
	unsigned int i;

	for (i = 0; i < thread_count; i++) {
		struct bio_queue_data *bio_queue_data =
			&io_submitter->bio_queue_data[i];
		snprintf(queue_name, sizeof(queue_name), "bioQ%u", i);

		if (USE_BIOMAP) {
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
				// entirely and indicate that initialization
				// failed.
				uds_log_error("bio map initialization failed %d",
					      result);
				cleanup_io_submitter(io_submitter);
				free_io_submitter(io_submitter);
				return result;
			}
		}

		result = initialize_bio_queue(bio_queue_data,
					      thread_name_prefix,
					      queue_name,
					      i,
					      layer);
		if (result != VDO_SUCCESS) {
			// Clean up the partially initialized bio-queue entirely
			// and indicate that initialization failed.
			if (USE_BIOMAP) {
				free_int_map(&io_submitter->bio_queue_data[i].map);
			}
			uds_log_error("bio queue initialization failed %d",
				      result);
			cleanup_io_submitter(io_submitter);
			free_io_submitter(io_submitter);
			return result;
		}

		io_submitter->num_bio_queues_used++;
	}

	*io_submitter_ptr = io_submitter;

	return VDO_SUCCESS;
}

/**********************************************************************/
void cleanup_io_submitter(struct io_submitter *io_submitter)
{
	int i;

	for (i = io_submitter->num_bio_queues_used - 1; i >= 0; i--) {
		finish_work_queue(io_submitter->bio_queue_data[i].queue);
	}
}

/**********************************************************************/
void free_io_submitter(struct io_submitter *io_submitter)
{
	int i;

	for (i = io_submitter->num_bio_queues_used - 1; i >= 0; i--) {
		io_submitter->num_bio_queues_used--;
		free_work_queue(&io_submitter->bio_queue_data[i].queue);
		if (USE_BIOMAP) {
			free_int_map(&io_submitter->bio_queue_data[i].map);
		}
	}
	FREE(io_submitter);
}

/**********************************************************************/
void dump_bio_work_queue(struct io_submitter *io_submitter)
{
	int i;

	for (i = 0; i < io_submitter->num_bio_queues_used; i++) {
		dump_work_queue(io_submitter->bio_queue_data[i].queue);
	}
}


/**********************************************************************/
void enqueue_bio_work_item(struct io_submitter *io_submitter,
			   struct kvdo_work_item *work_item)
{
	unsigned int bio_queue_index = advance_bio_rotor(io_submitter);

	enqueue_work_queue(io_submitter->bio_queue_data[bio_queue_index].queue,
			   work_item);
}
