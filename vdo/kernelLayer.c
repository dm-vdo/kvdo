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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.c#226 $
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
#include "adminState.h"
#include "flush.h"
#include "releaseVersions.h"
#include "statistics.h"
#include "vdo.h"
#include "vdoLoad.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"
#include "vdoSuspend.h"
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

	uds_log_warning("vdo_launch_bio called from within a VDO thread!");
	/*
	 * We're not yet entirely sure what circumstances are causing this
	 * situation in [ESC-638], but it does appear to be happening and
	 * causing VDO to deadlock.
	 *
	 * Somehow vdo_launch_bio is being called from generic_make_request
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
		add_to_vdo_deadlock_queue(&vdo->deadlock_queue,
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
int vdo_launch_bio(struct vdo *vdo, struct bio *bio)
{
	int result;
	uint64_t arrival_jiffies = jiffies;
	struct vdo_work_queue *current_work_queue;
	bool has_discard_permit = false;
	const struct admin_state_code *code
		= get_vdo_admin_state_code(&vdo->admin_state);

	ASSERT_LOG_ONLY(code->normal,
			"vdo_launch_bio should not be called while in state %s",
			code->name);

	// Count all incoming bios.
	vdo_count_bios(&vdo->stats.bios_in, bio);


	// Handle empty bios.  Empty flush bios are not associated with a vio.
	if ((bio_op(bio) == REQ_OP_FLUSH) ||
	    ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		launch_vdo_flush(vdo, bio);
		return DM_MAPIO_SUBMITTED;
	}

	current_work_queue = get_current_work_queue();

	if ((current_work_queue != NULL) &&
	    (vdo == get_work_queue_owner(current_work_queue))) {
		/*
		 * This prohibits sleeping during I/O submission to VDO from
		 * its own thread.
		 */
		return launch_data_vio_from_vdo_thread(vdo,
						       bio,
						       arrival_jiffies);
	}

	if (bio_op(bio) == REQ_OP_DISCARD) {
		limiter_wait_for_one_free(&vdo->discard_limiter);
		has_discard_permit = true;
	}
	limiter_wait_for_one_free(&vdo->request_limiter);

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
void complete_many_requests(struct vdo *vdo, uint32_t count)
{
	// If we had to buffer some requests to avoid deadlock, release them
	// now.
	while (count > 0) {
		bool has_discard_permit;
		int result;
		uint64_t arrival_jiffies = 0;
		struct bio *bio = poll_vdo_deadlock_queue(&vdo->deadlock_queue,
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
			vdo_complete_bio(bio, result);
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
	int result;
	struct kernel_layer *layer;
	char thread_name_prefix[MAX_VDO_WORK_QUEUE_NAME_LEN];

	// VDO-3769 - Set a generic reason so we don't ever return garbage.
	*reason = "Unspecified error";

	/*
	 * Part 1 - Allocate the kernel layer, its essential parts, and set
	 * up the sysfs node. These must come first so that the sysfs node
	 * works correctly through the freeing of the kernel layer. After this
	 * part you must use free_kernel_layer.
	 */
	result = UDS_ALLOCATE(1,
			      struct kernel_layer,
			      "VDO configuration",
			      &layer);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO";
		release_vdo_instance(instance);
		return result;
	}

	result = initialize_vdo(&layer->vdo,
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
	snprintf(thread_name_prefix,
		 sizeof(thread_name_prefix),
		 "%s%u",
		 THIS_MODULE->name,
		 instance);

	result = make_batch_processor(&layer->vdo,
				      return_data_vio_batch_to_pool,
				      &layer->vdo,
				      &layer->vdo.data_vio_releaser);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate vio-freeing batch processor";
		free_kernel_layer(layer);
		return result;
	}

	// Dedupe Index
	BUG_ON(thread_name_prefix[0] == '\0');
	result = make_vdo_dedupe_index(&layer->vdo.dedupe_index,
				       &layer->vdo,
				       thread_name_prefix);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot initialize dedupe index";
		free_kernel_layer(layer);
		return result;
	}

	/*
	 * Part 3 - Do initializations that depend upon other previous
	 * initializations, but have no order dependencies at freeing time.
	 * Order dependencies for initialization are identified using BUG_ON.
	 */


	// Data vio pool
	BUG_ON(layer->vdo.device_config->logical_block_size <= 0);
	BUG_ON(layer->vdo.request_limiter.limit <= 0);
	BUG_ON(layer->vdo.device_config->owned_device == NULL);
	result = make_data_vio_buffer_pool(layer->vdo.request_limiter.limit,
					   &layer->vdo.data_vio_pool);
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
	result = make_vdo_threads(&layer->vdo, thread_name_prefix, reason);
	if (result != VDO_SUCCESS) {
		free_kernel_layer(layer);
		return result;
	}

	// Bio queue
	result = make_vdo_io_submitter(thread_name_prefix,
				       config->thread_counts.bio_threads,
				       config->thread_counts.bio_rotation_interval,
				       layer->vdo.request_limiter.limit,
				       &layer->vdo,
				       &layer->vdo.io_submitter);
	if (result != VDO_SUCCESS) {
		// If initialization of the bio-queues failed, they are cleaned
		// up already, so just free the rest of the kernel layer.
		free_kernel_layer(layer);
		*reason = "bio submission initialization failed";
		return result;
	}

	// Bio ack queue
	if (use_bio_ack_queue(&layer->vdo)) {
		result = make_work_queue(thread_name_prefix,
					 "ackQ",
					 &layer->vdo.work_queue_directory,
					 &layer->vdo,
					 layer,
					 &bio_ack_q_type,
					 config->thread_counts.bio_ack_threads,
					 NULL,
					 &layer->vdo.bio_ack_queue);
		if (result != VDO_SUCCESS) {
			*reason = "bio ack queue initialization failed";
			free_kernel_layer(layer);
			return result;
		}
	}

	// CPU Queues
	result = make_work_queue(thread_name_prefix,
				 "cpuQ",
				 &layer->vdo.work_queue_directory,
				 &layer->vdo,
				 layer,
				 &cpu_q_type,
				 config->thread_counts.cpu_threads,
				 (void **) layer->vdo.compression_context,
				 &layer->vdo.cpu_queue);
	if (result != VDO_SUCCESS) {
		*reason = "CPU queue initialization failed";
		free_kernel_layer(layer);
		return result;
	}

	*layer_ptr = layer;
	return VDO_SUCCESS;
}

/**********************************************************************/
int prepare_to_modify_kernel_layer(struct kernel_layer *layer,
				   struct device_config *config,
				   char **error_ptr)
{
	struct device_config *extant_config = layer->vdo.device_config;
	int result;

	result = validate_new_device_config(extant_config, config, error_ptr);
	if (result != VDO_SUCCESS) {
		return -EINVAL;
	}

	// Below here are the actions to take when a non-immutable property
	// changes.

	if (config->logical_blocks > extant_config->logical_blocks) {
		result = prepare_to_resize_logical(layer,
						   config->logical_blocks);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Device prepare_vdo_to_grow_logical failed";
			return result;
		}
	}

	if (config->physical_blocks > extant_config->physical_blocks) {
		result = prepare_to_resize_physical(layer,
						    config->physical_blocks);
		if (result != VDO_SUCCESS) {
			if (result == VDO_TOO_MANY_SLABS) {
				*error_ptr = "Device prepare_vdo_to_grow_physical failed (specified physical size too big based on formatted slab size)";
			} else {
				*error_ptr = "Device prepare_vdo_to_grow_physical failed";
			}
			return result;
		}
	}

	if (strcmp(config->parent_device_name,
		   extant_config->parent_device_name) != 0) {
		const char *device_name
			= get_vdo_device_name(config->owning_target);
		uds_log_info("Updating backing device of %s from %s to %s",
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
void free_kernel_layer(struct kernel_layer *layer)
{
	/*
	 * This is not the cleanest implementation, but given the current
	 * timing uncertainties in the shutdown process for work queues, we
	 * need to store information to enable a late-in-process deallocation
	 * of funnel-queue data structures in work queues.
	 */
	enum kernel_layer_state state = get_kernel_layer_state(layer);
	const struct admin_state_code *code;

	switch (state) {
	case LAYER_STOPPING:
		uds_log_error("re-entered free_kernel_layer while stopping");
		break;

	case LAYER_RUNNING:
		suspend_kernel_layer(layer);
		fallthrough;

	case LAYER_STARTING:
	case LAYER_RESUMING:
	case LAYER_SUSPENDED:
		stop_kernel_layer(layer);
		fallthrough;

	case LAYER_STOPPED:
		break;

	case LAYER_NEW:
		code = get_vdo_admin_state_code(&layer->vdo.admin_state);
		if ((code == VDO_ADMIN_STATE_NEW)
		    || (code == VDO_ADMIN_STATE_INITIALIZED)
		    || (code == VDO_ADMIN_STATE_PRE_LOADED)) {
			break;
		}

		uds_log_error("New kernel layer in unexpected state %s",
			      code->name);
		break;

	default:
		uds_log_error("Unknown Kernel Layer state: %d", state);
	}

	destroy_vdo(&layer->vdo);
}

/**********************************************************************/
int start_kernel_layer(struct kernel_layer *layer, char **reason)
{
	int result;

	set_kernel_layer_state(layer, LAYER_STARTING);
	result = load_vdo(&layer->vdo);
	if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
		*reason = "Cannot load metadata from device";
		stop_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	return VDO_SUCCESS;
}

/**********************************************************************/
void stop_kernel_layer(struct kernel_layer *layer)
{
	layer->vdo.allocations_allowed = true;

	// Stop services that need to gather VDO statistics from the worker
	// threads.
	if (layer->vdo.stats_added) {
		layer->vdo.stats_added = false;
		init_completion(&layer->vdo.stats_shutdown);
		kobject_put(&layer->vdo.stats_directory);
		wait_for_completion(&layer->vdo.stats_shutdown);
	}

	switch (get_kernel_layer_state(layer)) {
	case LAYER_RUNNING:
		suspend_kernel_layer(layer);
		fallthrough;

	case LAYER_SUSPENDED:
		set_kernel_layer_state(layer, LAYER_STOPPING);
		stop_vdo_dedupe_index(layer->vdo.dedupe_index);
		fallthrough;

	case LAYER_STOPPING:
	case LAYER_STOPPED:
	default:
		set_kernel_layer_state(layer, LAYER_STOPPED);
	}
}

/**********************************************************************/
int suspend_kernel_layer(struct kernel_layer *layer)
{
	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	int result = suspend_vdo(&layer->vdo);

	if (result == VDO_INVALID_ADMIN_STATE) {
		uds_log_error("Suspend invoked while in unexpected kernel layer state %d",
			      get_kernel_layer_state(layer));
		return -EINVAL;
	}

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

	resume_vdo_dedupe_index(layer->vdo.dedupe_index);
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

	uds_log_info("Preparing to resize physical to %llu", physical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	result = prepare_vdo_to_grow_physical(&layer->vdo, physical_count);
	if (result != VDO_SUCCESS) {
		// prepare_vdo_to_grow_physical logs errors.
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

	uds_log_info("Done preparing to resize physical");
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

	uds_log_info("Preparing to resize logical to %llu", logical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	result = prepare_vdo_to_grow_logical(&layer->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		// prepare_vdo_to_grow_logical logs errors
		return result;
	}

	uds_log_info("Done preparing to resize logical");
	return VDO_SUCCESS;
}

/***********************************************************************/
int resize_logical(struct kernel_layer *layer, block_count_t logical_count)
{
	int result;

	uds_log_info("Resizing logical to %llu", logical_count);
	/*
	 * We must not mark the VDO as allowing allocations when it is
	 * suspended lest an allocation attempt block on writing IO to the
	 * suspended VDO.
	 */
	result = vdo_resize_logical(&layer->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		// vdo_resize_logical logs errors
		return result;
	}

	uds_log_info("Logical blocks now %llu", logical_count);
	return VDO_SUCCESS;
}

