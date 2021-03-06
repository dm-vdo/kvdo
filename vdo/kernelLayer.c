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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/kernel/kernelLayer.c#1 $
 */

#include "kernelLayer.h"

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/lz4.h>
#include <linux/ratelimit.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"
#include "permassert.h"

#include "adminCompletion.h"
#include "flush.h"
#include "releaseVersions.h"
#include "statistics.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "volumeGeometry.h"

#include "bio.h"
#include "dataKVIO.h"
#include "dedupeIndex.h"
#include "deviceConfig.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "ioSubmitter.h"
#include "kvio.h"
#include "poolSysfs.h"
#include "stringUtils.h"
#include "vdoInit.h"

static const struct vdo_work_queue_type bio_ack_q_type = {
	.action_table = {
		{
			.name = "bio_ack",
			.code = BIO_ACK_Q_ACTION_ACK,
			.priority = 0
		},
	},
};

static const struct vdo_work_queue_type cpu_q_type = {
	.action_table = {
		{
			.name = "cpu_complete_vio",
			.code = CPU_Q_ACTION_COMPLETE_VIO,
			.priority = 0
		},
		{
			.name = "cpu_compress_block",
			.code = CPU_Q_ACTION_COMPRESS_BLOCK,
			.priority = 0
		},
		{
			.name = "cpu_hash_block",
			.code = CPU_Q_ACTION_HASH_BLOCK,
			.priority = 0
		},
		{
			.name = "cpu_event_reporter",
			.code = CPU_Q_ACTION_EVENT_REPORTER,
			.priority = 0
		},
	},
};

/**********************************************************************/
int map_to_system_error(int error)
{
	char error_name[80], error_message[ERRBUF_SIZE];

	// 0 is success, negative a system error code
	if (likely(error <= 0)) {
		return error;
	}
	if (error < 1024) {
		// errno macro used without negating - may be a minor bug
		return -error;
	}

	// VDO or UDS error
	switch (sans_unrecoverable(error)) {
	case VDO_NO_SPACE:
		return -ENOSPC;
	case VDO_READ_ONLY:
		return -EIO;
	default:
		log_info("%s: mapping internal status code %d (%s: %s) to EIO",
			 __func__,
			 error,
			 string_error_name(error,
					   error_name,
					   sizeof(error_name)),
			 uds_string_error(error,
					  error_message,
					  sizeof(error_message)));
		return -EIO;
	}
}

/**********************************************************************/
static void set_kernel_layer_state(struct kernel_layer *layer,
				   enum kernel_layer_state new_state)
{
	smp_wmb();
	WRITE_ONCE(layer->state, new_state);
}

/**
 * Start processing a new data vio based on the supplied bio, but from within
 * a VDO thread context, when we're not allowed to block. Using this path at
 * all suggests a bug or erroneous usage, but we special-case it to avoid a
 * deadlock that can apparently result. Message will be logged to alert the
 * administrator that something has gone wrong, while we attempt to continue
 * processing other requests.
 *
 * If a request permit can be acquired immediately,
 * vdo_launch_data_vio_from_bio will be called. (If the bio is a discard
 * operation, a permit from the discard limiter will be requested but the call
 * will be made with or without it.) If the request permit is not available,
 * the bio will be saved on a list to be launched later. Either way, this
 * function will not block, and will take responsibility for processing the
 * bio.
 *
 * @param vdo              The vdo
 * @param bio              The bio to launch
 * @param arrival_jiffies  The arrival time of the bio
 *
 * @return DM_MAPIO_SUBMITTED or a system error code
 **/
static int launch_data_vio_from_vdo_thread(struct vdo *vdo,
					   struct bio *bio,
					   uint64_t arrival_jiffies)
{
	bool has_discard_permit;
	int result;

	uds_log_warning("kvdo_map_bio called from within a VDO thread!");
	/*
	 * We're not yet entirely sure what circumstances are causing this
	 * situation in [ESC-638], but it does appear to be happening and
	 * causing VDO to deadlock.
	 *
	 * Somehow kvdo_map_bio is being called from generic_make_request
	 * which is being called from the VDO code to pass a flush on down to
	 * the underlying storage system; we've got 2000 requests in progress,
	 * so we have to wait for one to complete, but none can complete while
	 * the bio thread is blocked from passing more I/O requests down. Near
	 * as we can tell, the flush bio should always have gotten updated to
	 * point to the storage system, so we shouldn't be calling back into
	 * VDO unless something's gotten messed up somewhere.
	 *
	 * To side-step this case, if the limiter says we're busy *and* we're
	 * running on one of VDO's own threads, we'll drop the I/O request in a
	 * special queue for processing as soon as vios become free.
	 *
	 * We don't want to do this in general because it leads to unbounded
	 * buffering, arbitrarily high latencies, inability to push back in a
	 * way the caller can take advantage of, etc. If someone wants huge
	 * amounts of buffering on top of VDO, they're welcome to access it
	 * through the kernel page cache or roll their own.
	 */
	if (!limiter_poll(&vdo->request_limiter)) {
		add_to_deadlock_queue(&vdo->deadlock_queue,
				      bio,
				      arrival_jiffies);
		uds_log_warning("queued an I/O request to avoid deadlock!");

		return DM_MAPIO_SUBMITTED;
	}

	has_discard_permit =
		((bio_op(bio) == REQ_OP_DISCARD) &&
		 limiter_poll(&vdo->discard_limiter));
	result = vdo_launch_data_vio_from_bio(vdo,
					      bio,
					      arrival_jiffies,
					      has_discard_permit);
	// Succeed or fail, vdo_launch_data_vio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}


/**********************************************************************/
int kvdo_map_bio(struct kernel_layer *layer, struct bio *bio)
{
	int result;
	uint64_t arrival_jiffies = jiffies;
	enum kernel_layer_state state = get_kernel_layer_state(layer);
	struct vdo_work_queue *current_work_queue;
	bool has_discard_permit = false;

	ASSERT_LOG_ONLY(state == LAYER_RUNNING,
			"kvdo_map_bio should not be called while in state %d",
			state);

	// Count all incoming bios.
	count_bios(&layer->bios_in, bio);


	// Handle empty bios.  Empty flush bios are not associated with a vio.
	if ((bio_op(bio) == REQ_OP_FLUSH) ||
	    ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		launch_vdo_flush(&layer->vdo, bio);
		return DM_MAPIO_SUBMITTED;
	}

	current_work_queue = get_current_work_queue();

	if ((current_work_queue != NULL) &&
	    (layer == get_work_queue_owner(current_work_queue))) {
		/*
		 * This prohibits sleeping during I/O submission to VDO from
		 * its own thread.
		 */
		return launch_data_vio_from_vdo_thread(&layer->vdo,
						       bio,
						       arrival_jiffies);
	}

	if (bio_op(bio) == REQ_OP_DISCARD) {
		limiter_wait_for_one_free(&layer->vdo.discard_limiter);
		has_discard_permit = true;
	}
	limiter_wait_for_one_free(&layer->vdo.request_limiter);

	result = vdo_launch_data_vio_from_bio(&layer->vdo,
					      bio,
					      arrival_jiffies,
					      has_discard_permit);
	// Succeed or fail, vdo_launch_data_vio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
void complete_many_requests(struct vdo *vdo, uint32_t count)
{
	// If we had to buffer some requests to avoid deadlock, release them
	// now.
	while (count > 0) {
		bool has_discard_permit;
		int result;
		uint64_t arrival_jiffies = 0;
		struct bio *bio = poll_deadlock_queue(&vdo->deadlock_queue,
						      &arrival_jiffies);
		if (likely(bio == NULL)) {
			break;
		}

		has_discard_permit =
			((bio_op(bio) == REQ_OP_DISCARD) &&
			 limiter_poll(&vdo->discard_limiter));
		result = vdo_launch_data_vio_from_bio(vdo,
						      bio,
						      arrival_jiffies,
						      has_discard_permit);
		if (result != VDO_SUCCESS) {
			complete_bio(bio, result);
		}
		// Succeed or fail, vdo_launch_data_vio_from_bio owns the
		// permit(s) now.
		count--;
	}
	// Notify the limiter, so it can wake any blocked processes.
	if (count > 0) {
		limiter_release_many(&vdo->request_limiter, count);
	}
}

/**********************************************************************/
int make_kernel_layer(unsigned int instance,
		      struct device_config *config,
		      char **reason,
		      struct kernel_layer **layer_ptr)
{
	int result, i;
	struct kernel_layer *layer;

	// VDO-3769 - Set a generic reason so we don't ever return garbage.
	*reason = "Unspecified error";

	/*
	 * Part 1 - Allocate the kernel layer, its essential parts, and set
	 * up the sysfs node. These must come first so that the sysfs node
	 * works correctly through the freeing of the kernel layer. After this
	 * part you must use free_kernel_layer.
	 */
	result = ALLOCATE(1, struct kernel_layer, "VDO configuration", &layer);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO configuration";
		release_vdo_instance(instance);
		return result;
	}

	result = initialize_vdo(&layer->vdo,
				&layer->common,
				config,
				instance,
				reason);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/*
	 * After this point, calling kobject_put on vdo->vdo_directory will
	 * decrement its reference count, and when the count goes to 0 the
	 * struct kernel_layer will be freed.
	 *
	 * Any error in this method from here on requires calling
	 * free_kernel_layer() before returning.
	 */

	/*
	 * Part 2 - Do all the simple initialization.  These initializations
	 * have no order dependencies and can be done in any order, but
	 * free_kernel_layer() cannot be called until all the simple layer
	 * properties are set.
	 *
	 * The kernel_layer structure starts as all zeros.  Pointer
	 * initializations consist of replacing a NULL pointer with a non-NULL
	 * pointer, which can be easily undone by freeing all of the non-NULL
	 * pointers (using the proper free routine).
	 */
	set_kernel_layer_state(layer, LAYER_SIMPLE_THINGS_INITIALIZED);

	mutex_init(&layer->stats_mutex);

	result = register_vdo(&layer->vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot add layer to device registry";
		free_kernel_layer(layer);
		return result;
	}

	snprintf(layer->thread_name_prefix,
		 sizeof(layer->thread_name_prefix),
		 "%s%u",
		 THIS_MODULE->name,
		 instance);

	result = make_batch_processor(layer,
				      return_data_vio_batch_to_pool,
				      layer,
				      &layer->data_vio_releaser);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate vio-freeing batch processor";
		free_kernel_layer(layer);
		return result;
	}

	// Spare kvdo_flush, so that we will always have at least one available

	// Dedupe Index
	BUG_ON(layer->thread_name_prefix[0] == '\0');
	result = make_dedupe_index(&layer->dedupe_index, &layer->vdo);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot initialize dedupe index";
		free_kernel_layer(layer);
		return result;
	}

	// Compression context storage
	result = ALLOCATE(config->thread_counts.cpu_threads,
			  char *,
			  "LZ4 context",
			  &layer->compression_context);
	if (result != VDO_SUCCESS) {
		*reason = "cannot allocate LZ4 context";
		free_kernel_layer(layer);
		return result;
	}

	for (i = 0; i < config->thread_counts.cpu_threads; i++) {
		result = ALLOCATE(LZ4_MEM_COMPRESS,
				  char,
				  "LZ4 context",
				  &layer->compression_context[i]);
		if (result != VDO_SUCCESS) {
			*reason = "cannot allocate LZ4 context";
			free_kernel_layer(layer);
			return result;
		}
	}


	/*
	 * Part 3 - Do initializations that depend upon other previous
	 * initializations, but have no order dependencies at freeing time.
	 * Order dependencies for initialization are identified using BUG_ON.
	 */
	set_kernel_layer_state(layer, LAYER_BUFFER_POOLS_INITIALIZED);


	// Data vio pool
	BUG_ON(layer->vdo.device_config->logical_block_size <= 0);
	BUG_ON(layer->vdo.request_limiter.limit <= 0);
	BUG_ON(layer->vdo.device_config->owned_device == NULL);
	result = make_data_vio_buffer_pool(layer->vdo.request_limiter.limit,
					   &layer->data_vio_pool);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate vio data";
		free_kernel_layer(layer);
		return result;
	}

	/*
	 * Part 4 - Do initializations that depend upon other previous
	 * initialization, that may have order dependencies at freeing time.
	 * These are mostly starting up the workqueue threads.
	 */

	// Base-code thread, etc
	result = make_vdo_threads(&layer->vdo, reason);
	if (result != VDO_SUCCESS) {
		free_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_REQUEST_QUEUE_INITIALIZED);

	// Bio queue
	result = make_io_submitter(layer->thread_name_prefix,
				   config->thread_counts.bio_threads,
				   config->thread_counts.bio_rotation_interval,
				   layer->vdo.request_limiter.limit,
				   layer,
				   &layer->vdo.io_submitter);
	if (result != VDO_SUCCESS) {
		// If initialization of the bio-queues failed, they are cleaned
		// up already, so just free the rest of the kernel layer.
		free_kernel_layer(layer);
		*reason = "bio submission initialization failed";
		return result;
	}
	set_kernel_layer_state(layer, LAYER_BIO_DATA_INITIALIZED);

	// Bio ack queue
	if (use_bio_ack_queue(&layer->vdo)) {
		result = make_work_queue(layer->thread_name_prefix,
					 "ackQ",
					 &layer->vdo.work_queue_directory,
					 layer,
					 layer,
					 &bio_ack_q_type,
					 config->thread_counts.bio_ack_threads,
					 NULL,
					 &layer->bio_ack_queue);
		if (result != VDO_SUCCESS) {
			*reason = "bio ack queue initialization failed";
			free_kernel_layer(layer);
			return result;
		}
	}

	set_kernel_layer_state(layer, LAYER_BIO_ACK_QUEUE_INITIALIZED);

	// CPU Queues
	result = make_work_queue(layer->thread_name_prefix,
				 "cpuQ",
				 &layer->vdo.work_queue_directory,
				 layer,
				 layer,
				 &cpu_q_type,
				 config->thread_counts.cpu_threads,
				 (void **) layer->compression_context,
				 &layer->cpu_queue);
	if (result != VDO_SUCCESS) {
		*reason = "CPU queue initialization failed";
		free_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_CPU_QUEUE_INITIALIZED);

	*layer_ptr = layer;
	return VDO_SUCCESS;
}

/**********************************************************************/
int prepare_to_modify_kernel_layer(struct kernel_layer *layer,
				   struct device_config *config,
				   char **error_ptr)
{
	struct device_config *extant_config = layer->vdo.device_config;

	if (config->owning_target->begin !=
	    extant_config->owning_target->begin) {
		*error_ptr = "Starting sector cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (config->logical_block_size != extant_config->logical_block_size) {
		*error_ptr = "Logical block size cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (config->cache_size != extant_config->cache_size) {
		*error_ptr = "Block map cache size cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (config->block_map_maximum_age !=
	    extant_config->block_map_maximum_age) {
		*error_ptr = "Block map maximum age cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (memcmp(&config->thread_counts, &extant_config->thread_counts,
		   sizeof(struct thread_count_config)) != 0) {
		*error_ptr = "Thread configuration cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	// Below here are the actions to take when a non-immutable property
	// changes.

	if (config->owning_target->len != extant_config->owning_target->len) {
		int result;
		size_t logical_bytes = to_bytes(config->owning_target->len);

		if ((logical_bytes % VDO_BLOCK_SIZE) != 0) {
			*error_ptr = "Logical size must be a multiple of 4096";
			return VDO_PARAMETER_MISMATCH;
		}

		result = prepare_to_resize_logical(
			layer, logical_bytes / VDO_BLOCK_SIZE);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Device prepare_to_grow_logical failed";
			return result;
		}
	}

	if (config->physical_blocks != extant_config->physical_blocks) {
		int result = prepare_to_resize_physical(
			layer, config->physical_blocks);
		if (result != VDO_SUCCESS) {
			if (result == VDO_TOO_MANY_SLABS) {
				*error_ptr = "Device prepare_to_grow_physical failed (specified physical size too big based on formatted slab size)";
			} else {
				*error_ptr = "Device prepare_to_grow_physical failed";
			}
			return result;
		}
	}
	if (strcmp(config->parent_device_name,
		   extant_config->parent_device_name) != 0) {
		const char *device_name
			= get_vdo_device_name(config->owning_target);
	        log_info("Updating backing device of %s from %s to %s",
			 device_name,
	                 extant_config->parent_device_name,
	                 config->parent_device_name);
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int modify_kernel_layer(struct kernel_layer *layer,
			struct device_config *config)
{
	int result;
	struct device_config *extant_config = layer->vdo.device_config;
	enum kernel_layer_state state = get_kernel_layer_state(layer);

	if (state == LAYER_RUNNING) {
		return VDO_SUCCESS;
	} else if (state != LAYER_SUSPENDED) {
		uds_log_error("pre-resume invoked while in unexpected kernel layer state %d",
			      state);
		return -EINVAL;
	}
	set_kernel_layer_state(layer, LAYER_RESUMING);

	// A failure here is unrecoverable. So there is no problem if it
	// happens.

	if (config->owning_target->len != extant_config->owning_target->len) {
		size_t logical_bytes = to_bytes(config->owning_target->len);
		result = resize_logical(layer, logical_bytes / VDO_BLOCK_SIZE);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Grow physical if the version is 0, so we can't tell if we
	// got an old-style growPhysical command, or if size changed.
	if ((config->physical_blocks != extant_config->physical_blocks) ||
	    (config->version == 0)) {
		result = resize_physical(layer, config->physical_blocks);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
static void free_compression_context(struct kernel_layer *layer)
{
	int i;
	for (i = 0;
	     i < layer->vdo.device_config->thread_counts.cpu_threads;
	     i++) {
		FREE(layer->compression_context[i]);
	}
	FREE(layer->compression_context);
}

/**********************************************************************/
void free_kernel_layer(struct kernel_layer *layer)
{
	/*
	 * This is not the cleanest implementation, but given the current
	 * timing uncertainties in the shutdown process for work queues, we
	 * need to store information to enable a late-in-process deallocation
	 * of funnel-queue data structures in work queues.
	 */
	bool used_bio_ack_queue = false;
	bool used_cpu_queue = false;

	enum kernel_layer_state state = get_kernel_layer_state(layer);

	switch (state) {
	case LAYER_STOPPING:
		uds_log_error("re-entered free_kernel_layer while stopping");
		break;

	case LAYER_RUNNING:
		suspend_kernel_layer(layer);
		// fall through

	case LAYER_STARTING:
	case LAYER_RESUMING:
	case LAYER_SUSPENDED:
		stop_kernel_layer(layer);
		// fall through

	case LAYER_STOPPED:
	case LAYER_CPU_QUEUE_INITIALIZED:
		finish_work_queue(layer->cpu_queue);
		used_cpu_queue = true;
		// fall through

	case LAYER_BIO_ACK_QUEUE_INITIALIZED:
		if (use_bio_ack_queue(&layer->vdo)) {
			finish_work_queue(layer->bio_ack_queue);
			used_bio_ack_queue = true;
		}
		// fall through

	case LAYER_BIO_DATA_INITIALIZED:
		cleanup_io_submitter(layer->vdo.io_submitter);
		// fall through

	case LAYER_REQUEST_QUEUE_INITIALIZED:
		finish_vdo(&layer->vdo);
		// fall through

	case LAYER_BUFFER_POOLS_INITIALIZED:
		free_buffer_pool(&layer->data_vio_pool);
		// fall through

	case LAYER_SIMPLE_THINGS_INITIALIZED:
		if (layer->compression_context != NULL) {
			free_compression_context(layer);
		}
		if (layer->dedupe_index != NULL) {
			finish_dedupe_index(layer->dedupe_index);
		}
		free_batch_processor(&layer->data_vio_releaser);
		unregister_vdo(&layer->vdo);
		break;

	default:
		uds_log_error("Unknown Kernel Layer state: %d", state);
	}

	// Late deallocation of resources in work queues.
	if (used_cpu_queue) {
		free_work_queue(&layer->cpu_queue);
	}
	if (used_bio_ack_queue) {
		free_work_queue(&layer->bio_ack_queue);
	}
	if (layer->vdo.io_submitter) {
		free_io_submitter(layer->vdo.io_submitter);
	}

	free_dedupe_index(&layer->dedupe_index);
	destroy_vdo(&layer->vdo);
}

/**********************************************************************/
static void pool_stats_release(struct kobject *directory)
{
	struct vdo *vdo = container_of(directory, struct vdo, stats_directory);
	complete(&(vdo_as_kernel_layer(vdo)->stats_shutdown));
}

/**********************************************************************/
int preload_kernel_layer(struct kernel_layer *layer, char **reason)
{
	int result;

	if (get_kernel_layer_state(layer) != LAYER_CPU_QUEUE_INITIALIZED) {
		*reason = "preload_kernel_layer() may only be invoked after initialization";
		return UDS_BAD_STATE;
	}

	set_kernel_layer_state(layer, LAYER_STARTING);
	result = prepare_to_load_vdo(&layer->vdo);
	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		stop_kernel_layer(layer);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int start_kernel_layer(struct kernel_layer *layer, char **reason)
{
	static struct kobj_type stats_directory_type = {
		.release = pool_stats_release,
		.sysfs_ops = &pool_stats_sysfs_ops,
		.default_attrs = pool_stats_attrs,
	};
	int result;

	if (get_kernel_layer_state(layer) != LAYER_STARTING) {
		*reason = "Cannot start kernel from non-starting state";
		stop_kernel_layer(layer);
		return UDS_BAD_STATE;
	}

	result = start_vdo(&layer->vdo, reason);
	if (result != VDO_SUCCESS) {
		stop_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	kobject_init(&layer->vdo.stats_directory, &stats_directory_type);
	result = kobject_add(&layer->vdo.stats_directory,
			     &layer->vdo.vdo_directory,
			     "statistics");
	if (result != 0) {
		*reason = "Cannot add sysfs statistics node";
		stop_kernel_layer(layer);
		return result;
	}
	layer->stats_added = true;

	if (layer->vdo.device_config->deduplication) {
		// Don't try to load or rebuild the index first (and log
		// scary error messages) if this is known to be a
		// newly-formatted volume.
		start_dedupe_index(layer->dedupe_index, was_new(&layer->vdo));
	}

	layer->vdo.allocations_allowed = false;
	return VDO_SUCCESS;
}

/**********************************************************************/
void stop_kernel_layer(struct kernel_layer *layer)
{
	layer->vdo.allocations_allowed = true;

	// Stop services that need to gather VDO statistics from the worker
	// threads.
	if (layer->stats_added) {
		layer->stats_added = false;
		init_completion(&layer->stats_shutdown);
		kobject_put(&layer->vdo.stats_directory);
		wait_for_completion(&layer->stats_shutdown);
	}

	switch (get_kernel_layer_state(layer)) {
	case LAYER_RUNNING:
		suspend_kernel_layer(layer);
		// fall through

	case LAYER_SUSPENDED:
		set_kernel_layer_state(layer, LAYER_STOPPING);
		stop_dedupe_index(layer->dedupe_index);
		// fall through

	case LAYER_STOPPING:
	case LAYER_STOPPED:
	default:
		set_kernel_layer_state(layer, LAYER_STOPPED);
	}
}

/**
 * Issue a flush request and wait for it to complete.
 *
 * @param layer The kernel layer
 *
 * @return VDO_SUCCESS or an error
 */
static int synchronous_flush(struct kernel_layer *layer)
{
	int result;
	struct bio bio;
	bio_init(&bio, 0, 0);
	bio_set_dev(&bio, get_vdo_backing_device(&layer->vdo));
	bio.bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
	submit_bio_wait(&bio);
	result = blk_status_to_errno(bio.bi_status);

	atomic64_inc(&layer->flush_out);
	if (result != 0) {
		log_error_strerror(result, "synchronous flush failed");
		result = -EIO;
	}

	bio_uninit(&bio);
	return result;
}

/**********************************************************************/
int suspend_kernel_layer(struct kernel_layer *layer)
{
	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	enum kernel_layer_state state = get_kernel_layer_state(layer);
	int result, suspend_result;

	if (state == LAYER_SUSPENDED) {
		return VDO_SUCCESS;
	}
	if (state != LAYER_RUNNING) {
		uds_log_error("Suspend invoked while in unexpected kernel layer state %d",
			      state);
		return -EINVAL;
	}

	/*
	 * Attempt to flush all I/O before completing post suspend work. We
	 * believe a suspended device is expected to have persisted all data
	 * written before the suspend, even if it hasn't been flushed yet.
	 */
	vdo_wait_for_no_requests_active(&layer->vdo);
	result = synchronous_flush(layer);
	if (result != VDO_SUCCESS) {
		set_vdo_read_only(&layer->vdo, result);
	}

	/*
	 * Suspend the VDO, writing out all dirty metadata if the no-flush flag
	 * was not set on the dmsetup suspend call. This will ensure that we
	 * don't have cause to write while suspended [VDO-4402].
	 */
	suspend_result = suspend_vdo(&layer->vdo);

	if (result == VDO_SUCCESS) {
		result = suspend_result;
	}

	suspend_dedupe_index(layer->dedupe_index,
			     !layer->vdo.no_flush_suspend);
	set_kernel_layer_state(layer, LAYER_SUSPENDED);
	return result;
}

/**********************************************************************/
int resume_kernel_layer(struct kernel_layer *layer)
{
	int result;

	if (get_kernel_layer_state(layer) == LAYER_RUNNING) {
		return VDO_SUCCESS;
	}

	resume_dedupe_index(layer->dedupe_index);
	result = resume_vdo(&layer->vdo);

	if (result != VDO_SUCCESS) {
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	return VDO_SUCCESS;
}

/***********************************************************************/
int prepare_to_resize_physical(struct kernel_layer *layer,
			       block_count_t physical_count)
{
	int result;

	log_info("Preparing to resize physical to %llu", physical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	result = prepare_to_grow_physical(&layer->vdo, physical_count);
	if (result != VDO_SUCCESS) {
		// prepare_to_grow_physical logs errors.
		if (result == VDO_PARAMETER_MISMATCH) {
			/*
			 * If we don't trap this case, map_to_system_error()
			 * will remap it to -EIO, which is misleading and
			 * ahistorical.
			 */
			return -EINVAL;
		} else {
			return result;
		}
	}

	log_info("Done preparing to resize physical");
	return VDO_SUCCESS;
}

/***********************************************************************/
int resize_physical(struct kernel_layer *layer, block_count_t physical_count)
{
	/*
	 * We must not mark the layer as allowing allocations when it is
	 * suspended lest an allocation attempt block on writing IO to the
	 * suspended VDO.
	 */
	int result = vdo_resize_physical(&layer->vdo, physical_count);

	if (result != VDO_SUCCESS) {
		// vdo_resize_physical logs errors
		return result;
	}
	return VDO_SUCCESS;
}

/***********************************************************************/
int prepare_to_resize_logical(struct kernel_layer *layer,
			      block_count_t logical_count)
{
	int result;

	log_info("Preparing to resize logical to %llu", logical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	result = prepare_to_grow_logical(&layer->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		// prepare_to_grow_logical logs errors
		return result;
	}

	log_info("Done preparing to resize logical");
	return VDO_SUCCESS;
}

/***********************************************************************/
int resize_logical(struct kernel_layer *layer, block_count_t logical_count)
{
	int result;

	log_info("Resizing logical to %llu", logical_count);
	/*
	 * We must not mark the layer as allowing allocations when it is
	 * suspended lest an allocation attempt block on writing IO to the
	 * suspended VDO.
	 */
	result = vdo_resize_logical(&layer->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		// vdo_resize_logical logs errors
		return result;
	}

	log_info("Logical blocks now %llu", logical_count);
	return VDO_SUCCESS;
}

