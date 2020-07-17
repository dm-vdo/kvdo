/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvdoFlush.c#21 $
 */

#include "kvdoFlush.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "threadConfig.h"

#include "bio.h"
#include "ioSubmitter.h"

/**
 * A specific (concrete) encapsulation of flush requests.
 *
 * <p>We attempt to allocate a kvdo_flush structure for each incoming flush
 * bio. In case the allocate fails, a spare structure is pre-allocated by and
 * stored in the kernel layer. The first time an allocation fails, the spare
 * is used. If another allocation fails while the spare is in use, it will
 * merely be queued for later processing.
 *
 * <p>When a kvdo_flush is complete, it will either be freed, immediately
 * re-used for queued flushes, or stashed in the kernel layer as the new spare
 * structure. This ensures that we will always make forward progress.
 **/
struct kvdo_flush {
	struct kvdo_work_item work_item;
	struct kernel_layer *layer;
	struct bio_list bios;
	Jiffies arrival_time; // Time when earliest bio appeared
	struct vdo_flush vdo_flush;
};

/**********************************************************************/
int make_kvdo_flush(struct kvdo_flush **flush_ptr)
{
	return ALLOCATE(1, struct kvdo_flush, __func__, flush_ptr);
}

/**********************************************************************/
bool should_process_flush(struct kernel_layer *layer)
{
	return (get_kvdo_write_policy(&layer->kvdo) != WRITE_POLICY_SYNC);
}

/**
 * Function call to handle an empty flush request from the request queue.
 *
 * @param item  The work item representing the flush request
 **/
static void kvdo_flush_work(struct kvdo_work_item *item)
{
	struct kvdo_flush *kvdo_flush = container_of(item,
						     struct kvdo_flush,
						     work_item);
	flush(kvdo_flush->layer->kvdo.vdo, &kvdo_flush->vdo_flush);
}

/**
 * Initialize a kvdo_flush structure, transferring all the bios in the kernel
 * layer's waiting_flushes list to it. The caller MUST already hold the layer's
 * flush_lock.
 *
 * @param kvdo_flush  The flush to initialize
 * @param layer       The kernel layer on which the flush_lock is held
 **/
static void initialize_kvdo_flush(struct kvdo_flush *kvdo_flush,
				  struct kernel_layer *layer)
{
	kvdo_flush->layer = layer;
	bio_list_init(&kvdo_flush->bios);
	bio_list_merge(&kvdo_flush->bios, &layer->waiting_flushes);
	bio_list_init(&layer->waiting_flushes);
	kvdo_flush->arrival_time = layer->flush_arrival_time;
}

/**********************************************************************/
static void enqueue_kvdo_flush(struct kvdo_flush *kvdo_flush)
{
	setup_work_item(&kvdo_flush->work_item,
			kvdo_flush_work,
			NULL,
			REQ_Q_ACTION_FLUSH);
	struct kvdo *kvdo = &kvdo_flush->layer->kvdo;

	enqueue_kvdo_work(kvdo,
			  &kvdo_flush->work_item,
			  get_packer_zone_thread(get_thread_config(kvdo->vdo)));
}

/**********************************************************************/
void launch_kvdo_flush(struct kernel_layer *layer, struct bio *bio)
{
	// Try to allocate a kvdo_flush to represent the flush request.
	// If the allocation fails, we'll deal with it later.
	struct kvdo_flush *kvdo_flush = ALLOCATE_NOWAIT(struct kvdo_flush,
							__func__);

	spin_lock(&layer->flush_lock);

	// We have a new bio to start.  Add it to the list.  If it becomes the
	// only entry on the list, record the time.
	if (bio_list_empty(&layer->waiting_flushes)) {
		layer->flush_arrival_time = jiffies;
	}
	bio_list_add(&layer->waiting_flushes, bio);

	if (kvdo_flush == NULL) {
		// The kvdo_flush allocation failed. Try to use the spare
		// kvdo_flush structure.
		if (layer->spare_kvdo_flush == NULL) {
			// The spare is already in use. This bio is on
			// waiting_flushes and it will be handled by a flush
			// completion or by a bio that can allocate.
			spin_unlock(&layer->flush_lock);
			return;
		}

		// Take and use the spare kvdo_flush structure.
		kvdo_flush = layer->spare_kvdo_flush;
		layer->spare_kvdo_flush = NULL;
	}

	// We have flushes to start. Capture them in the kvdo_flush structure.
	initialize_kvdo_flush(kvdo_flush, layer);

	spin_unlock(&layer->flush_lock);

	// Finish launching the flushes.
	enqueue_kvdo_flush(kvdo_flush);
}

/**
 * Release a kvdo_flush structure that has completed its work. If there are
 * any pending flush requests whose kvdo_flush allocation failed, they will be
 * launched by immediately re-using the released kvdo_flush. If there is no
 * spare kvdo_flush, the released structure  will become the spare. Otherwise,
 * the kvdo_flush will be freed.
 *
 * @param kvdo_flush  The completed flush structure to re-use or free
 **/
static void release_kvdo_flush(struct kvdo_flush *kvdo_flush)
{
	struct kernel_layer *layer = kvdo_flush->layer;
	bool relaunch_flush = false;
	bool free_flush = false;

	spin_lock(&layer->flush_lock);
	if (bio_list_empty(&layer->waiting_flushes)) {
		// Nothing needs to be started.  Save one spare kvdo_flush
		// structure.
		if (layer->spare_kvdo_flush == NULL) {
			// Make the new spare all zero, just like a newly
			// allocated one.
			memset(kvdo_flush, 0, sizeof(*kvdo_flush));
			layer->spare_kvdo_flush = kvdo_flush;
		} else {
			free_flush = true;
		}
	} else {
		// We have flushes to start.  Capture them in the kvdo_flush
		// structure.
		initialize_kvdo_flush(kvdo_flush, layer);
		relaunch_flush = true;
	}
	spin_unlock(&layer->flush_lock);

	if (relaunch_flush) {
		// Finish launching the flushes.
		enqueue_kvdo_flush(kvdo_flush);
	} else if (free_flush) {
		FREE(kvdo_flush);
	}
}

/**
 * Function called to complete and free a flush request
 *
 * @param item    The flush-request work item
 **/
static void kvdo_complete_flush_work(struct kvdo_work_item *item)
{
	struct kvdo_flush *kvdo_flush = container_of(item,
						     struct kvdo_flush,
						     work_item);
	struct kernel_layer *layer = kvdo_flush->layer;

	struct bio *bio;

	while ((bio = bio_list_pop(&kvdo_flush->bios)) != NULL) {
		// We're not acknowledging this bio now, but we'll never touch
		// it again, so this is the last chance to account for it.
		count_bios(&layer->biosAcknowledged, bio);

		// Make sure the bio is an empty flush bio.
		prepare_flush_bio(bio,
				  bio->bi_private,
				  get_kernel_layer_bdev(layer),
				  bio->bi_end_io);
		atomic64_inc(&layer->flushOut);
		generic_make_request(bio);
	}


	// Release the kvdo_flush structure, freeing it, re-using it as the
	// spare, or using it to launch any flushes that had to wait when
	// allocations failed.
	release_kvdo_flush(kvdo_flush);
}

/**********************************************************************/
void kvdo_complete_flush(struct vdo_flush **kfp)
{
	if (*kfp != NULL) {
		struct kvdo_flush *kvdo_flush = container_of(*kfp,
							     struct kvdo_flush,
							     vdo_flush);
		setup_work_item(&kvdo_flush->work_item,
				kvdo_complete_flush_work,
				NULL,
				BIO_Q_ACTION_FLUSH);
		enqueue_bio_work_item(kvdo_flush->layer->io_submitter,
				      &kvdo_flush->work_item);
		*kfp = NULL;
	}
}

/**********************************************************************/
int synchronous_flush(struct kernel_layer *layer)
{
	struct bio bio;
	bio_init(&bio, 0, 0);
	prepare_flush_bio(&bio, layer, get_kernel_layer_bdev(layer), NULL);
	int result = submit_bio_and_wait(&bio);

	atomic64_inc(&layer->flushOut);
	if (result != 0) {
		logErrorWithStringError(result, "synchronous flush failed");
		result = -EIO;
	}

	bio_uninit(&bio);
	return result;
}
