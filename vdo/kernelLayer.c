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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.c#147 $
 */

#include "kernelLayer.h"

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/lz4.h>
#include <linux/ratelimit.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "releaseVersions.h"
#include "volumeGeometry.h"
#include "statistics.h"
#include "vdo.h"
#include "vdoResize.h"
#include "vdoResizeLogical.h"

#include "bio.h"
#include "dataKVIO.h"
#include "dedupeIndex.h"
#include "deviceConfig.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"
#include "kvio.h"
#include "poolSysfs.h"
#include "stringUtils.h"

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

// 2000 is half the number of entries currently in our page cache,
// to allow for each in-progress operation to update two pages.
int default_max_requests_active = 2000;

/**********************************************************************/
crc32_checksum_t update_crc32(crc32_checksum_t crc, const byte *buffer,
			      size_t length)
{
	/*
	 * The kernel's CRC 32 implementation does not do pre- and post-
	 * conditioning, so do it ourselves.
	 */
	return crc32(crc ^ 0xffffffff, buffer, length) ^ 0xffffffff;
}

/**********************************************************************/
block_count_t get_vdo_physical_block_count(const struct vdo *vdo)
{
	return as_kernel_layer(vdo->layer)->device_config->physical_blocks;
}

/**********************************************************************/
bool layer_is_named(struct kernel_layer *layer, void *context)
{
	struct dm_target *ti = layer->device_config->owning_target;
	const char *device_name = get_vdo_device_name(ti);
	return (strcmp(device_name, (const char *) context) == 0);
}

/**
 * Implements layer_filter_t.
 **/
static bool layer_uses_device(struct kernel_layer *layer, void *context)
{
	struct device_config *config = context;
	return (layer->device_config->owned_device->bdev->bd_dev
		== config->owned_device->bdev->bd_dev);
}

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

/**********************************************************************/
void wait_for_no_requests_active(struct kernel_layer *layer)
{
	bool was_compressing;

	// Do nothing if there are no requests active.  This check is not
	// necessary for correctness but does reduce log message traffic.
	if (limiter_is_idle(&layer->request_limiter)) {
		return;
	}

	// We have to make sure to flush the packer before waiting. We do this
	// by turning off compression, which also means no new entries coming
	// in while waiting will end up in the packer.
	was_compressing = set_kvdo_compressing(&layer->vdo, false);
	// Now wait for there to be no active requests
	limiter_wait_for_idle(&layer->request_limiter);
	// Reset the compression state after all requests are done
	if (was_compressing) {
		set_kvdo_compressing(&layer->vdo, true);
	}
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
 * @param layer            The kernel layer
 * @param bio              The bio to launch
 * @param arrival_jiffies  The arrival time of the bio
 *
 * @return  DM_MAPIO_SUBMITTED or a system error code
 **/
static int launch_data_vio_from_vdo_thread(struct kernel_layer *layer,
					   struct bio *bio,
					   uint64_t arrival_jiffies)
{
	bool has_discard_permit;
	int result;

	log_warning("kvdo_map_bio called from within a VDO thread!");
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
	if (!limiter_poll(&layer->request_limiter)) {
		add_to_deadlock_queue(&layer->deadlock_queue,
				      bio,
				      arrival_jiffies);
		log_warning("queued an I/O request to avoid deadlock!");

		return DM_MAPIO_SUBMITTED;
	}

	has_discard_permit =
		((bio_op(bio) == REQ_OP_DISCARD) &&
		 limiter_poll(&layer->discard_limiter));
	result = vdo_launch_data_vio_from_bio(layer, bio, arrival_jiffies,
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
		if (should_process_flush(layer)) {
			launch_kvdo_flush(layer, bio);
			return DM_MAPIO_SUBMITTED;
		} else {
			/*
			 * We're not acknowledging this bio now, but we'll
			 * never touch it again, so this is the last chance to
			 * account for it.
			 */
			count_bios(&layer->bios_acknowledged, bio);
			atomic64_inc(&layer->flush_out);
			bio_set_dev(bio, get_kernel_layer_bdev(layer));
			return DM_MAPIO_REMAPPED;
		}
	}

	current_work_queue = get_current_work_queue();

	if ((current_work_queue != NULL) &&
	    (layer == get_work_queue_owner(current_work_queue))) {
		/*
		 * This prohibits sleeping during I/O submission to VDO from
		 * its own thread.
		 */
		return launch_data_vio_from_vdo_thread(layer, bio,
						       arrival_jiffies);
	}

	if (bio_op(bio) == REQ_OP_DISCARD) {
		limiter_wait_for_one_free(&layer->discard_limiter);
		has_discard_permit = true;
	}
	limiter_wait_for_one_free(&layer->request_limiter);

	result = vdo_launch_data_vio_from_bio(layer, bio, arrival_jiffies,
					      has_discard_permit);
	// Succeed or fail, vdo_launch_data_vio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
struct block_device *get_kernel_layer_bdev(const struct kernel_layer *layer)
{
	return layer->device_config->owned_device->bdev;
}

/**********************************************************************/
const char *get_vdo_device_name(const struct dm_target *ti)
{
     	return dm_device_name(dm_table_get_md(ti->table));
}

/**********************************************************************/
void complete_many_requests(struct kernel_layer *layer, uint32_t count)
{
	// If we had to buffer some requests to avoid deadlock, release them
	// now.
	while (count > 0) {
		bool has_discard_permit;
		int result;
		uint64_t arrival_jiffies = 0;
		struct bio *bio = poll_deadlock_queue(&layer->deadlock_queue,
						      &arrival_jiffies);
		if (likely(bio == NULL)) {
			break;
		}

		has_discard_permit =
			((bio_op(bio) == REQ_OP_DISCARD) &&
			 limiter_poll(&layer->discard_limiter));
		result = vdo_launch_data_vio_from_bio(layer, bio,
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
		limiter_release_many(&layer->request_limiter, count);
	}
}

/**
 * Implements extent_reader. Exists only for the geometry block; is unset after
 * it is read.
 **/
static int kvdo_synchronous_read(PhysicalLayer *layer,
				 physical_block_number_t start_block,
				 size_t block_count,
				 char *buffer)
{
	struct kernel_layer *kernel_layer = as_kernel_layer(layer);
	struct bio *bio;
	int result;

	if (block_count != 1) {
		return VDO_NOT_IMPLEMENTED;
	}

	result = create_bio(&bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = reset_bio_with_buffer(bio, buffer, NULL, NULL, REQ_OP_READ,
				       start_block);
	if (result != VDO_SUCCESS) {
		free_bio(bio);
		return result;
	}

	bio_set_dev(bio, get_kernel_layer_bdev(kernel_layer));
	submit_bio_wait(bio);
	result = blk_status_to_errno(bio->bi_status);
	if (result != 0) {
		log_error_strerror(result, "synchronous read failed");
		result = -EIO;
	}
	free_bio(bio);

	if (result != VDO_SUCCESS) {
		return result;
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
static enum write_policy kvdo_get_write_policy(PhysicalLayer *common)
{
	struct kernel_layer *layer = as_kernel_layer(common);
	return get_write_policy(&layer->vdo);
}

/**
 * Function that is called when a synchronous operation is completed. We let
 * the waiting thread know it can continue.
 *
 * <p>Implements operation_complete.
 *
 * @param common  The kernel layer
 **/
static void kvdo_complete_sync_operation(PhysicalLayer *common)
{
	struct kernel_layer *layer = as_kernel_layer(common);

	complete(&layer->callback_sync);
}

/**
 * Wait for a synchronous operation to complete.
 *
 * <p>Implements operation_waiter.
 *
 * @param common  The kernel layer
 **/
static void wait_for_sync_operation(PhysicalLayer *common)
{
	struct kernel_layer *layer = as_kernel_layer(common);
	// Using the "interruptible" interface means that Linux will not log a
	// message when we wait for more than 120 seconds.
	while (wait_for_completion_interruptible(&layer->callback_sync) != 0) {
		// However, if we get a signal in a user-mode process, we could
		// spin...
		msleep(1);
	}
}

/**********************************************************************/
int make_kernel_layer(uint64_t starting_sector,
		      unsigned int instance,
		      struct device_config *config,
		      struct kobject *parent_kobject,
		      struct thread_config **thread_config_pointer,
		      char **reason,
		      struct kernel_layer **layer_ptr)
{
	int result, request_limit, i;
	struct kernel_layer *layer, *old_layer;
	// VDO-3769 - Set a generic reason so we don't ever return garbage.
	*reason = "Unspecified error";

	old_layer = find_layer_matching(layer_uses_device, config);
	if (old_layer != NULL) {
		uds_log_error("Existing layer already uses device %s",
			      old_layer->device_config->parent_device_name);
		*reason = "Cannot share storage device with already-running VDO";
		return VDO_BAD_CONFIGURATION;
	}

	/*
	 * Part 1 - Allocate the kernel layer, its essential parts, and set
	 * up the sysfs node. These must come first so that the sysfs node
	 * works correctly through the freeing of the kernel layer. After this
	 * part you must use free_kernel_layer.
	 */
	result = ALLOCATE(1, struct kernel_layer, "VDO configuration", &layer);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO configuration";
		return result;
	}

	result = initialize_vdo(&layer->common, &layer->vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate VDO";
		FREE(layer);
		return result;
	}

	// After this point, calling kobject_put on kobj will decrement its
	// reference count, and when the count goes to 0 the struct kernel_layer
	// will be freed.
	struct dm_target *ti = config->owning_target;
     	const char *device_name = get_vdo_device_name(ti);

	kobject_init(&layer->kobj, &kernel_layer_kobj_type);
	result = kobject_add(&layer->kobj, parent_kobject, device_name);
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&layer->kobj);
		return result;
	}
	kobject_init(&layer->wq_directory, &work_queue_directory_kobj_type);
	result = kobject_add(&layer->wq_directory, &layer->kobj, "work_queues");
	if (result != 0) {
		*reason = "Cannot add sysfs node";
		kobject_put(&layer->wq_directory);
		kobject_put(&layer->kobj);
		return result;
	}

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

	initialize_deadlock_queue(&layer->deadlock_queue);

	request_limit = default_max_requests_active;
	initialize_limiter(&layer->request_limiter, request_limit);
	initialize_limiter(&layer->discard_limiter, request_limit * 3 / 4);

	layer->allocations_allowed = true;
	layer->instance = instance;
	layer->device_config = config;
	layer->starting_sector_offset = starting_sector;
	INIT_LIST_HEAD(&layer->device_config_list);

	layer->common.getWritePolicy = kvdo_get_write_policy;
	layer->common.completeFlush = kvdo_complete_flush;
	layer->common.waitForAdminOperation = wait_for_sync_operation;
	layer->common.completeAdminOperation = kvdo_complete_sync_operation;
	spin_lock_init(&layer->flush_lock);
	mutex_init(&layer->stats_mutex);
	bio_list_init(&layer->waiting_flushes);

	result = add_layer_to_device_registry(layer);
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

	result = make_thread_config(config->thread_counts.logical_zones,
				    config->thread_counts.physical_zones,
				    config->thread_counts.hash_zones,
				    thread_config_pointer);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot create thread configuration";
		free_kernel_layer(layer);
		return result;
	}

	log_info("zones: %d logical, %d physical, %d hash; base threads: %d",
		 config->thread_counts.logical_zones,
		 config->thread_counts.physical_zones,
		 config->thread_counts.hash_zones,
		 (*thread_config_pointer)->base_thread_count);

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
	result = make_kvdo_flush(&layer->spare_kvdo_flush);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate kvdo_flush record";
		free_kernel_layer(layer);
		return result;
	}

	// Read the geometry block so we know how to set up the index. Allow it
	// to do synchronous reads.
	layer->common.reader = kvdo_synchronous_read;
	result = load_volume_geometry(&layer->common, &layer->geometry);
	layer->common.reader = NULL;
	if (result != VDO_SUCCESS) {
		*reason = "Could not load geometry block";
		free_kernel_layer(layer);
		return result;
	}

	// Dedupe Index
	BUG_ON(layer->thread_name_prefix[0] == '\0');
	result = make_dedupe_index(&layer->dedupe_index, layer);
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
	BUG_ON(layer->device_config->logical_block_size <= 0);
	BUG_ON(layer->request_limiter.limit <= 0);
	BUG_ON(layer->device_config->owned_device == NULL);
	result = make_data_vio_buffer_pool(layer->request_limiter.limit,
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
	result = make_vdo_threads(&layer->vdo, *thread_config_pointer, reason);
	if (result != VDO_SUCCESS) {
		free_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_REQUEST_QUEUE_INITIALIZED);

	// Bio queue
	result = make_io_submitter(layer->thread_name_prefix,
				   config->thread_counts.bio_threads,
				   config->thread_counts.bio_rotation_interval,
				   layer->request_limiter.limit,
				   layer,
				   &layer->io_submitter);
	if (result != VDO_SUCCESS) {
		// If initialization of the bio-queues failed, they are cleaned
		// up already, so just free the rest of the kernel layer.
		free_kernel_layer(layer);
		*reason = "bio submission initialization failed";
		return result;
	}
	set_kernel_layer_state(layer, LAYER_BIO_DATA_INITIALIZED);

	// Bio ack queue
	if (use_bio_ack_queue(layer)) {
		result = make_work_queue(layer->thread_name_prefix,
					 "ackQ",
					 &layer->wq_directory,
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
				 &layer->wq_directory,
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
	struct device_config *extant_config = layer->device_config;

	if (config->owning_target->begin !=
	    extant_config->owning_target->begin) {
		*error_ptr = "Starting sector cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (strcmp(config->parent_device_name,
		   extant_config->parent_device_name) != 0) {
		*error_ptr = "Underlying device cannot change";
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

	return VDO_SUCCESS;
}

/**********************************************************************/
int modify_kernel_layer(struct kernel_layer *layer,
			struct device_config *config)
{
	int result;
	struct device_config *extant_config = layer->device_config;
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

	if (config->write_policy != extant_config->write_policy) {
		/*
		 * Ordinarily, when going from async to sync, we must flush any
		 * metadata written. However, because the underlying storage
		 * must have gone into sync mode before we suspend VDO, and
		 * suspending VDO concludes by issuing a flush, all metadata
		 * written before the suspend is flushed by the suspend and all
		 * metadata between the suspend and the write policy change is
		 * written to synchronous storage.
		 */
		log_info("Modifying device '%s' write policy from %s to %s",
			 get_vdo_device_name(extant_config->owning_target),
			 get_config_write_policy_string(extant_config),
			 get_config_write_policy_string(config));
		set_write_policy(&layer->vdo, config->write_policy);
	}

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
	bool used_bio_ack_queue = false;
	bool used_cpu_queue = false;
	bool used_kvdo = false;
	bool release_instance = false;

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
		release_instance = true;
		// fall through

	case LAYER_BIO_ACK_QUEUE_INITIALIZED:
		if (use_bio_ack_queue(layer)) {
			finish_work_queue(layer->bio_ack_queue);
			used_bio_ack_queue = true;
		}
		// fall through

	case LAYER_BIO_DATA_INITIALIZED:
		cleanup_io_submitter(layer->io_submitter);
		// fall through

	case LAYER_REQUEST_QUEUE_INITIALIZED:
		finish_kvdo(&layer->vdo);
		used_kvdo = true;
		// fall through

	case LAYER_BUFFER_POOLS_INITIALIZED:
		free_buffer_pool(&layer->data_vio_pool);
		// fall through

	case LAYER_SIMPLE_THINGS_INITIALIZED:
		if (layer->compression_context != NULL) {
			int i;

			for (i = 0;
			     i < layer->device_config->thread_counts.cpu_threads;
			     i++) {
				FREE(layer->compression_context[i]);
			}
			FREE(layer->compression_context);
		}
		if (layer->dedupe_index != NULL) {
			finish_dedupe_index(layer->dedupe_index);
		}
		FREE(layer->spare_kvdo_flush);
		layer->spare_kvdo_flush = NULL;
		free_batch_processor(&layer->data_vio_releaser);
		remove_layer_from_device_registry(layer);
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
	if (layer->io_submitter) {
		free_io_submitter(layer->io_submitter);
	}
	if (used_kvdo) {
		destroy_kvdo(&layer->vdo);
	}

	free_dedupe_index(&layer->dedupe_index);

	if (release_instance) {
		release_vdo_instance(layer->instance);
	}

	// The call to kobject_put on the kobj sysfs node will decrement its
	// reference count; when the count goes to zero the VDO object and
	// the kernel layer object will be freed as a side effect.
	kobject_put(&layer->wq_directory);
	kobject_put(&layer->kobj);
}

/**********************************************************************/
static void pool_stats_release(struct kobject *kobj)
{
	struct kernel_layer *layer = container_of(kobj,
						  struct kernel_layer,
						  stats_directory);
	complete(&layer->stats_shutdown);
}

/**********************************************************************/
int preload_kernel_layer(struct kernel_layer *layer,
			 const struct vdo_load_config *load_config,
			 char **reason)
{
	int result;

	if (get_kernel_layer_state(layer) != LAYER_CPU_QUEUE_INITIALIZED) {
		*reason = "preload_kernel_layer() may only be invoked after initialization";
		return UDS_BAD_STATE;
	}

	set_kernel_layer_state(layer, LAYER_STARTING);
	result = preload_kvdo(&layer->vdo, &layer->common, load_config,
			      reason);
	if (result != VDO_SUCCESS) {
		stop_kernel_layer(layer);
		return result;
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int start_kernel_layer(struct kernel_layer *layer, char **reason)
{
	static struct kobj_type stats_directory_kobj_type = {
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

	result = start_kvdo(&layer->vdo, &layer->common, reason);

	if (result != VDO_SUCCESS) {
		stop_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	kobject_init(&layer->stats_directory, &stats_directory_kobj_type);
	result = kobject_add(&layer->stats_directory,
			     &layer->kobj,
			     "statistics");
	if (result != 0) {
		*reason = "Cannot add sysfs statistics node";
		stop_kernel_layer(layer);
		return result;
	}
	layer->stats_added = true;

	if (layer->device_config->deduplication) {
		// Don't try to load or rebuild the index first (and log
		// scary error messages) if this is known to be a
		// newly-formatted volume.
		start_dedupe_index(layer->dedupe_index,
				   was_new(&layer->vdo));
	}

	layer->allocations_allowed = false;

	return VDO_SUCCESS;
}

/**********************************************************************/
void stop_kernel_layer(struct kernel_layer *layer)
{
	layer->allocations_allowed = true;

	// Stop services that need to gather VDO statistics from the worker
	// threads.
	if (layer->stats_added) {
		layer->stats_added = false;
		init_completion(&layer->stats_shutdown);
		kobject_put(&layer->stats_directory);
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
	bio_set_dev(&bio, get_kernel_layer_bdev(layer));
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
		uds_log_error(
			"Suspend invoked while in unexpected kernel layer state %d",
			state);
		return -EINVAL;
	}

	/*
	 * Attempt to flush all I/O before completing post suspend work. This
	 * is needed so that changing write policy upon resume is safe. Also,
	 * we think a suspended device is expected to have persisted all data
	 * written before the suspend, even if it hasn't been flushed yet.
	 */
	wait_for_no_requests_active(layer);
	result = synchronous_flush(layer);

	if (result != VDO_SUCCESS) {
		set_kvdo_read_only(&layer->vdo, result);
	}

	/*
	 * Suspend the VDO, writing out all dirty metadata if the no-flush flag
	 * was not set on the dmsetup suspend call. This will ensure that we
	 * don't have cause to write while suspended [VDO-4402].
	 */
	suspend_result = suspend_kvdo(&layer->vdo);

	if (result == VDO_SUCCESS) {
		result = suspend_result;
	}

	suspend_dedupe_index(layer->dedupe_index, !layer->no_flush_suspend);
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
	result = resume_kvdo(&layer->vdo);

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
	int result = kvdo_resize_physical(&layer->vdo, physical_count);

	if (result != VDO_SUCCESS) {
		// kvdo_resize_physical logs errors
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
	result = kvdo_resize_logical(&layer->vdo, logical_count);

	if (result != VDO_SUCCESS) {
		// kvdo_resize_logical logs errors
		return result;
	}

	log_info("Logical blocks now %llu", logical_count);
	return VDO_SUCCESS;
}

