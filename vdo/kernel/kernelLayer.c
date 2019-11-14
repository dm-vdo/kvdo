/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kernelLayer.c#59 $
 */

#include "kernelLayer.h"

#include <linux/crc32.h>
#include <linux/blkdev.h>
#include <linux/module.h>
#include <linux/backing-dev.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "murmur/MurmurHash3.h"

#include "lz4.h"
#include "releaseVersions.h"
#include "volumeGeometry.h"
#include "statistics.h"
#include "vdo.h"

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
#include "statusProcfs.h"
#include "stringUtils.h"

static const struct kvdo_work_queue_type bio_ack_q_type = {
	.action_table = {
		{
			.name = "bio_ack",
			.code = BIO_ACK_Q_ACTION_ACK,
			.priority = 0
		},
	},
};

static const struct kvdo_work_queue_type cpu_q_type = {
	.action_table = {
		{
			.name = "cpu_complete_kvio",
			.code = CPU_Q_ACTION_COMPLETE_KVIO,
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
CRC32Checksum update_crc32(CRC32Checksum crc, const byte *buffer, size_t length)
{
	/*
	 * The kernel's CRC 32 implementation does not do pre- and post-
	 * conditioning, so do it ourselves.
	 */
	return crc32(crc ^ 0xffffffff, buffer, length) ^ 0xffffffff;
}

/**********************************************************************/
static BlockCount kvdo_get_block_count(PhysicalLayer *header)
{
	return as_kernel_layer(header)->device_config->physical_blocks;
}

/**********************************************************************/
int map_to_system_error(int error)
{
	// 0 is success, negative a system error code
	if (likely(error <= 0)) {
		return error;
	}
	if (error < 1024) {
		// errno macro used without negating - may be a minor bug
		return -error;
	}
	// VDO or UDS error
	char error_name[80], error_message[ERRBUF_SIZE];
	switch (sans_unrecoverable(error)) {
	case VDO_NO_SPACE:
		return -ENOSPC;
	case VDO_READ_ONLY:
		return -EIO;
	default:
		logInfo("%s: mapping internal status code %d (%s: %s) to EIO",
			__func__,
			error,
			string_error_name(error,
					  error_name,
					  sizeof(error_name)),
			string_error(error,
				     error_message,
				     sizeof(error_message)));
		return -EIO;
	}
}

/**********************************************************************/
static void set_kernel_layer_state(struct kernel_layer *layer,
				   kernel_layer_state new_state)
{
	atomicStore32(&layer->state, new_state);
}

/**********************************************************************/
void wait_for_no_requests_active(struct kernel_layer *layer)
{
	// Do nothing if there are no requests active.  This check is not
	// necessary for correctness but does reduce log message traffic.
	if (limiter_is_idle(&layer->request_limiter)) {
		return;
	}

	// We have to make sure to flush the packer before waiting. We do this
	// by turning off compression, which also means no new entries coming
	// in while waiting will end up in the packer.
	bool was_compressing = set_kvdo_compressing(&layer->kvdo, false);
	// Now wait for there to be no active requests
	limiter_wait_for_idle(&layer->request_limiter);
	// Reset the compression state after all requests are done
	if (was_compressing) {
		set_kvdo_compressing(&layer->kvdo, true);
	}
}

/**
 * Start processing a new data KVIO based on the supplied bio, but from within
 * a VDO thread context, when we're not allowed to block. Using this path at
 * all suggests a bug or erroneous usage, but we special-case it to avoid a
 * deadlock that can apparently result. Message will be logged to alert the
 * administrator that something has gone wrong, while we attempt to continue
 * processing other requests.
 *
 * If a request permit can be acquired immediately,
 * kvdo_launch_data_kvio_from_bio will be called. (If the bio is a discard
 * operation, a permit from the discard limiter will be requested but the call
 * will be made with or without it.) If the request permit is not available,
 * the bio will be saved on a list to be launched later. Either way, this
 * function will not block, and will take responsibility for processing the
 * bio.
 *
 * @param layer         The kernel layer
 * @param bio           The bio to launch
 * @param arrival_time  The arrival time of the bio
 *
 * @return  DM_MAPIO_SUBMITTED or a system error code
 **/
static int launch_data_kvio_from_vdo_thread(struct kernel_layer *layer,
					    struct bio *bio,
					    Jiffies arrival_time)
{
	logWarning("kvdo_map_bio called from within a VDO thread!");
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
	 * special queue for processing as soon as KVIOs become free.
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
				arrival_time);
		logWarning("queued an I/O request to avoid deadlock!");

		return DM_MAPIO_SUBMITTED;
	}

	bool has_discard_permit =
		(is_discard_bio(bio) && limiter_poll(&layer->discard_limiter));
	int result = kvdo_launch_data_kvio_from_bio(layer,
						    bio,
						    arrival_time,
						    has_discard_permit);
	// Succeed or fail, kvdo_launch_data_kvio_from_bio owns the permit(s)
	// now.
	if (result != VDO_SUCCESS) {
		return result;
	}

	return DM_MAPIO_SUBMITTED;
}

/**********************************************************************/
int kvdo_map_bio(struct kernel_layer *layer, struct bio *bio)
{
	Jiffies arrival_time = jiffies;
	kernel_layer_state state = get_kernel_layer_state(layer);
	ASSERT_LOG_ONLY(state == LAYER_RUNNING,
			"kvdo_map_bio should not be called while in state %d",
			state);

	// Count all incoming bios.
	count_bios(&layer->biosIn, bio);

	// Handle empty bios.  Empty flush bios are not associated with a VIO.
	if (is_flush_bio(bio)) {
		if (ASSERT(get_bio_size(bio) == 0, "Flush bio is size 0") !=
		    VDO_SUCCESS) {
			// We expect flushes to be of size 0.
			return -EINVAL;
		}
		if (should_process_flush(layer)) {
			launch_kvdo_flush(layer, bio);
			return DM_MAPIO_SUBMITTED;
		} else {
			/*
			 * We're not acknowledging this bio now, but we'll
			 * never touch it again, so this is the last chance to
			 * account for it.
			 */
			count_bios(&layer->biosAcknowledged, bio);
			atomic64_inc(&layer->flushOut);
			set_bio_block_device(bio, get_kernel_layer_bdev(layer));
			return DM_MAPIO_REMAPPED;
		}
	}

	if (ASSERT(get_bio_size(bio) != 0, "Data bio is not size 0") !=
	    VDO_SUCCESS) {
		// We expect non-flushes to be non-zero in size.
		return -EINVAL;
	}

	if (is_discard_bio(bio) && is_read_bio(bio)) {
		// Read and Discard should never occur together
		return -EIO;
	}

	struct kvdo_work_queue *current_work_queue = get_current_work_queue();
	if ((current_work_queue != NULL) &&
	    (layer == get_work_queue_owner(current_work_queue))) {
		/*
		 * This prohibits sleeping during I/O submission to VDO from
		 * its own thread.
		 */
		return launch_data_kvio_from_vdo_thread(layer,
							bio,
							arrival_time);
	}
	bool has_discard_permit = false;
	if (is_discard_bio(bio)) {
		limiter_wait_for_one_free(&layer->discard_limiter);
		has_discard_permit = true;
	}
	limiter_wait_for_one_free(&layer->request_limiter);

	int result = kvdo_launch_data_kvio_from_bio(layer, bio, arrival_time,
						    has_discard_permit);
	// Succeed or fail, kvdo_launch_data_kvio_from_bio owns the permit(s)
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
void complete_many_requests(struct kernel_layer *layer, uint32_t count)
{
	// If we had to buffer some requests to avoid deadlock, release them
	// now.
	while (count > 0) {
		Jiffies arrival_time = 0;
		struct bio *bio = poll_deadlock_queue(&layer->deadlock_queue,
						      &arrival_time);
		if (likely(bio == NULL)) {
			break;
		}

		bool has_discard_permit =
			(is_discard_bio(bio) &&
			 limiter_poll(&layer->discard_limiter));
		int result = kvdo_launch_data_kvio_from_bio(
			layer, bio, arrival_time, has_discard_permit);
		if (result != VDO_SUCCESS) {
			complete_bio(bio, result);
		}
		// Succeed or fail, kvdo_launch_data_kvio_from_bio owns the
		// permit(s) now.
		count--;
	}
	// Notify the limiter, so it can wake any blocked processes.
	if (count > 0) {
		limiter_release_many(&layer->request_limiter, count);
	}
}

/**********************************************************************/
static int kvdo_create_enqueueable(VDOCompletion *completion)
{
	struct kvdo_enqueueable *kvdo_enqueueable;
	int result = ALLOCATE(1,
			      struct kvdo_enqueueable,
			      "kvdo_enqueueable",
			      &kvdo_enqueueable);
	if (result != VDO_SUCCESS) {
		logError("kvdo_enqueueable allocation failure %d", result);
		return result;
	}
	kvdo_enqueueable->enqueueable.completion = completion;
	completion->enqueueable = &kvdo_enqueueable->enqueueable;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void kvdo_destroy_enqueueable(Enqueueable **enqueueable_ptr)
{
	Enqueueable *enqueueable = *enqueueable_ptr;
	if (enqueueable != NULL) {
		struct kvdo_enqueueable *kvdo_enqueueable =
		  container_of(enqueueable,
			       struct kvdo_enqueueable,
			       enqueueable);
		FREE(kvdo_enqueueable);
		*enqueueable_ptr = NULL;
	}
}

/**
 * Implements BufferAllocator.
 **/
static int kvdo_allocate_io_buffer(PhysicalLayer *layer __attribute__((unused)),
				   size_t bytes,
				   const char *why,
				   char **buffer_ptr)
{
	return ALLOCATE(bytes, char, why, buffer_ptr);
}

/**
 * Implements ExtentReader. Exists only for the geometry block; is unset after
 * it is read.
 **/
static int kvdo_synchronous_read(PhysicalLayer *layer,
				 PhysicalBlockNumber start_block,
				 size_t block_count,
				 char *buffer,
				 size_t *blocks_read)
{
	if (block_count != 1) {
		return VDO_NOT_IMPLEMENTED;
	}

	struct kernel_layer *kernel_layer = as_kernel_layer(layer);

	struct bio *bio;
	int result = create_bio(kernel_layer, buffer, &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}
	set_bio_block_device(bio, get_kernel_layer_bdev(kernel_layer));
	set_bio_sector(bio, block_to_sector(kernel_layer, start_block));
	set_bio_operation_read(bio);
	result = submit_bio_and_wait(bio);
	if (result != 0) {
		logErrorWithStringError(result, "synchronous read failed");
		result = -EIO;
	}
	free_bio(bio, kernel_layer);

	if (result != VDO_SUCCESS) {
		return result;
	}
	if (blocks_read != NULL) {
		*blocks_read = block_count;
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
void destroy_vio(VIO **vio_ptr)
{
	VIO *vio = *vio_ptr;
	if (vio == NULL) {
		return;
	}

	BUG_ON(isDataVIO(vio));

	if (isCompressedWriteVIO(vio)) {
		struct compressed_write_kvio *compressed_write_kvio =
			allocating_vio_as_compressed_write_kvio(vioAsAllocatingVIO(vio));
		free_compressed_write_kvio(&compressed_write_kvio);
	} else {
		struct metadata_kvio *metadata_kvio = vio_as_metadata_kvio(vio);
		free_metadata_kvio(&metadata_kvio);
	}

	*vio_ptr = NULL;
}

/**********************************************************************/
static bool is_flush_required(PhysicalLayer *common)
{
	return should_process_flush(as_kernel_layer(common));
}

/**
 * Function that is called when a synchronous operation is completed. We let
 * the waiting thread know it can continue.
 *
 * <p>Implements OperationComplete.
 *
 * @param common  The kernel layer
 **/
static void kvdo_complete_sync_operation(PhysicalLayer *common)
{
	struct kernel_layer *layer = as_kernel_layer(common);
	complete(&layer->callbackSync);
}

/**
 * Wait for a synchronous operation to complete.
 *
 * <p>Implements OperationWaiter.
 *
 * @param common  The kernel layer
 **/
static void wait_for_sync_operation(PhysicalLayer *common)
{
	struct kernel_layer *layer = as_kernel_layer(common);
	// Using the "interruptible" interface means that Linux will not log a
	// message when we wait for more than 120 seconds.
	while (wait_for_completion_interruptible(&layer->callbackSync) != 0) {
	}
}

/**
 * Check whether a VDO, or its backing storage, is congested.
 *
 * @param callbacks  The callbacks structure inside the kernel layer
 * @param bdi_bits   Some info things to pass through.
 *
 * @return true if VDO or one of its backing storage stack is congested.
 **/
static int kvdo_is_congested(struct dm_target_callbacks *callbacks,
			     int bdi_bits)
{
	struct kernel_layer *layer =
		container_of(callbacks, struct kernel_layer, callbacks);
	if (!limiter_has_one_free(&layer->request_limiter)) {
		return 1;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	struct backing_dev_info *backing =
		bdev_get_queue(get_kernel_layer_bdev(layer))->backing_dev_info;
#else
	struct backing_dev_info *backing =
		&bdev_get_queue(get_kernel_layer_bdev(layer))->backing_dev_info;
#endif
	return bdi_congested(backing, bdi_bits);
}

/**********************************************************************/
int make_kernel_layer(uint64_t starting_sector,
		      unsigned int instance,
		      struct device_config *config,
		      struct kobject *parent_kobject,
		      ThreadConfig **thread_config_pointer,
		      char **reason,
		      struct kernel_layer **layer_ptr)
{
	// VDO-3769 - Set a generic reason so we don't ever return garbage.
	*reason = "Unspecified error";

	/*
	 * Part 1 - Allocate the kernel layer, its essential parts, and set
	 * up the sysfs node. These must come first so that the sysfs node
	 * works correctly through the freeing of the kernel layer. After this
	 * part you must use free_kernel_layer.
	 */
	struct kernel_layer *layer;
	int result =
		ALLOCATE(1, struct kernel_layer, "VDO configuration", &layer);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO configuration";
		return result;
	}

	// Allow the base VDO to allocate buffers and construct or destroy
	// enqueuables as part of its allocation.
	layer->common.allocateIOBuffer = kvdo_allocate_io_buffer;
	layer->common.createEnqueueable = kvdo_create_enqueueable;
	layer->common.destroyEnqueueable = kvdo_destroy_enqueueable;

	result = allocateVDO(&layer->common, &layer->kvdo.vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate VDO";
		FREE(layer);
		return result;
	}

	// After this point, calling kobject_put on kobj will decrement its
	// reference count, and when the count goes to 0 the struct kernel_layer
	// will be freed.
	kobject_init(&layer->kobj, &kernel_layer_kobj_type);
	result = kobject_add(&layer->kobj, parent_kobject, config->pool_name);
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

	int request_limit = default_max_requests_active;
	initialize_limiter(&layer->request_limiter, request_limit);
	initialize_limiter(&layer->discard_limiter, request_limit * 3 / 4);

	layer->allocations_allowed = true;
	layer->instance = instance;
	layer->device_config = config;
	layer->starting_sector_offset = starting_sector;
	initializeRing(&layer->device_config_ring);

	layer->common.getBlockCount = kvdo_get_block_count;
	layer->common.isFlushRequired = is_flush_required;
	layer->common.createMetadataVIO = kvdo_create_metadata_vio;
	layer->common.createCompressedWriteVIO =
		kvdo_create_compressed_write_vio;
	layer->common.completeFlush = kvdo_complete_flush;
	layer->common.enqueue = kvdo_enqueue;
	layer->common.waitForAdminOperation = wait_for_sync_operation;
	layer->common.completeAdminOperation = kvdo_complete_sync_operation;
	layer->common.flush = kvdo_flush_vio;
	spin_lock_init(&layer->flush_lock);
	mutex_init(&layer->statsMutex);
	bio_list_init(&layer->waiting_flushes);

	// This can go anywhere, as long as it is not registered until the
	// layer is fully allocated.
	layer->callbacks.congested_fn = kvdo_is_congested;

	result = add_layer_to_device_registry(config->pool_name, layer);
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

	result = makeThreadConfig(config->thread_counts.logical_zones,
				  config->thread_counts.physical_zones,
				  config->thread_counts.hash_zones,
				  thread_config_pointer);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot create thread configuration";
		free_kernel_layer(layer);
		return result;
	}

	logInfo("zones: %d logical, %d physical, %d hash; base threads: %d",
		config->thread_counts.logical_zones,
		config->thread_counts.physical_zones,
		config->thread_counts.hash_zones,
		(*thread_config_pointer)->baseThreadCount);

	result = make_batch_processor(layer,
				      return_data_kvio_batch_to_pool,
				      layer,
				      &layer->data_kvio_releaser);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate KVIO-freeing batch processor";
		free_kernel_layer(layer);
		return result;
	}

	// Spare KVDOFlush, so that we will always have at least one available
	result = make_kvdo_flush(&layer->spare_kvdo_flush);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate KVDOFlush record";
		free_kernel_layer(layer);
		return result;
	}

	// Read the geometry block so we know how to set up the index. Allow it
	// to do synchronous reads.
	layer->common.reader = kvdo_synchronous_read;
	result = loadVolumeGeometry(&layer->common, &layer->geometry);
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
			  &layer->compressionContext);
	if (result != VDO_SUCCESS) {
		*reason = "cannot allocate LZ4 context";
		free_kernel_layer(layer);
		return result;
	}
	for (int i = 0; i < config->thread_counts.cpu_threads; i++) {
		result = ALLOCATE(LZ4_context_size(),
				  char,
				  "LZ4 context",
				  &layer->compressionContext[i]);
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

	// Trace pool
	BUG_ON(layer->request_limiter.limit <= 0);
	result = trace_kernel_layer_init(layer);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot initialize trace data";
		free_kernel_layer(layer);
		return result;
	}

	// KVIO and VIO pool
	BUG_ON(layer->device_config->logical_block_size <= 0);
	BUG_ON(layer->request_limiter.limit <= 0);
	BUG_ON(layer->device_config->owned_device == NULL);
	result = make_data_kvio_buffer_pool(layer,
					    layer->request_limiter.limit,
					    &layer->data_kvio_pool);
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
	result = initialize_kvdo(&layer->kvdo, *thread_config_pointer, reason);
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
				 (void **)layer->compressionContext,
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

	if (config->md_raid5_mode_enabled !=
	    extant_config->md_raid5_mode_enabled) {
		*error_ptr = "mdRaid5Mode cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	if (memcmp(&config->thread_counts, &extant_config->thread_counts,
		   sizeof(struct thread_count_config)) != 0) {
		*error_ptr = "Thread configuration cannot change";
		return VDO_PARAMETER_MISMATCH;
	}

	// Below here are the actions to take when a non-immutable property
	// changes.

	if (config->write_policy != extant_config->write_policy) {
		// Nothing needs doing right now for a write policy change.
	}

	if (config->owning_target->len != extant_config->owning_target->len) {
		size_t logical_bytes = to_bytes(config->owning_target->len);
		if ((logical_bytes % VDO_BLOCK_SIZE) != 0) {
			*error_ptr = "Logical size must be a multiple of 4096";
			return VDO_PARAMETER_MISMATCH;
		}

		int result = prepare_to_resize_logical(
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
			*error_ptr = "Device prepare_to_grow_physical failed";
			return result;
		}
	}

	return VDO_SUCCESS;
}

/**********************************************************************/
int modify_kernel_layer(struct kernel_layer *layer,
			struct device_config *config)
{
	struct device_config *extant_config = layer->device_config;
	kernel_layer_state state = get_kernel_layer_state(layer);
	if (state == LAYER_RUNNING) {
		return VDO_SUCCESS;
	} else if (state != LAYER_SUSPENDED) {
		logError("pre-resume invoked while in unexpected kernel layer state %d",
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
		logInfo("Modifying device '%s' write policy from %s to %s",
			config->pool_name,
			get_config_write_policy_string(extant_config),
			get_config_write_policy_string(config));
		setWritePolicy(layer->kvdo.vdo, config->write_policy);
	}

	if (config->owning_target->len != extant_config->owning_target->len) {
		size_t logical_bytes = to_bytes(config->owning_target->len);
		int result =
			resize_logical(layer, logical_bytes / VDO_BLOCK_SIZE);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	// Grow physical if the version is 0, so we can't tell if we
	// got an old-style growPhysical command, or if size changed.
	if ((config->physical_blocks != extant_config->physical_blocks) ||
	    (config->version == 0)) {
		int result = resize_physical(layer, config->physical_blocks);
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

	kernel_layer_state state = get_kernel_layer_state(layer);
	switch (state) {
	case LAYER_STOPPING:
		logError("re-entered free_kernel_layer while stopping");
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
		finish_kvdo(&layer->kvdo);
		used_kvdo = true;
		// fall through

	case LAYER_BUFFER_POOLS_INITIALIZED:
		free_buffer_pool(&layer->data_kvio_pool);
		free_buffer_pool(&layer->trace_buffer_pool);
		// fall through

	case LAYER_SIMPLE_THINGS_INITIALIZED:
		if (layer->compressionContext != NULL) {
			for (int i = 0;
			     i < layer->device_config->thread_counts.cpu_threads;
			     i++) {
				FREE(layer->compressionContext[i]);
			}
			FREE(layer->compressionContext);
		}
		if (layer->dedupe_index != NULL) {
			finish_dedupe_index(layer->dedupe_index);
		}
		FREE(layer->spare_kvdo_flush);
		layer->spare_kvdo_flush = NULL;
		free_batch_processor(&layer->data_kvio_releaser);
		remove_layer_from_device_registry(
			layer->device_config->pool_name);
		break;

	default:
		logError("Unknown Kernel Layer state: %d", state);
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
		destroy_kvdo(&layer->kvdo);
	}

	free_dedupe_index(&layer->dedupe_index);

	if (release_instance) {
		release_kvdo_instance(layer->instance);
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
						  statsDirectory);
	complete(&layer->stats_shutdown);
}

/**********************************************************************/
int preload_kernel_layer(struct kernel_layer *layer,
			 const VDOLoadConfig *load_config,
			 char **reason)
{
	if (get_kernel_layer_state(layer) != LAYER_CPU_QUEUE_INITIALIZED) {
		*reason = "preload_kernel_layer() may only be invoked after initialization";
		return UDS_BAD_STATE;
	}

	set_kernel_layer_state(layer, LAYER_STARTING);
	int result = preload_kvdo(&layer->kvdo,
				  &layer->common,
				  load_config,
				  layer->vioTraceRecording,
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
	if (get_kernel_layer_state(layer) != LAYER_STARTING) {
		*reason = "Cannot start kernel from non-starting state";
		stop_kernel_layer(layer);
		return UDS_BAD_STATE;
	}

	int result = start_kvdo(&layer->kvdo, &layer->common, reason);
	if (result != VDO_SUCCESS) {
		stop_kernel_layer(layer);
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	static struct kobj_type stats_directory_kobj_type = {
		.release = pool_stats_release,
		.sysfs_ops = &pool_stats_sysfs_ops,
		.default_attrs = pool_stats_attrs,
	};
	kobject_init(&layer->statsDirectory, &stats_directory_kobj_type);
	result = kobject_add(&layer->statsDirectory,
			     &layer->kobj,
			     "statistics");
	if (result != 0) {
		*reason = "Cannot add sysfs statistics node";
		stop_kernel_layer(layer);
		return result;
	}
	layer->stats_added = true;

	// Don't try to load or rebuild the index first (and log scary error
	// messages) if this is known to be a newly-formatted volume.
	start_dedupe_index(layer->dedupe_index, wasNew(layer->kvdo.vdo));

	result = vdo_create_procfs_entry(layer,
					 layer->device_config->pool_name,
					 &layer->procfs_private);
	if (result != VDO_SUCCESS) {
		*reason = "Could not create proc filesystem entry";
		stop_kernel_layer(layer);
		return result;
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
		kobject_put(&layer->statsDirectory);
		wait_for_completion(&layer->stats_shutdown);
	}
	vdo_destroy_procfs_entry(layer->device_config->pool_name,
				 layer->procfs_private);

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

/**********************************************************************/
int suspend_kernel_layer(struct kernel_layer *layer)
{
	/*
	 * It's important to note any error here does not actually stop
	 * device-mapper from suspending the device. All this work is done
	 * post suspend.
	 */
	kernel_layer_state state = get_kernel_layer_state(layer);
	if (state == LAYER_SUSPENDED) {
		return VDO_SUCCESS;
	}
	if (state != LAYER_RUNNING) {
		logError(
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
	int result = synchronous_flush(layer);
	if (result != VDO_SUCCESS) {
		set_kvdo_read_only(&layer->kvdo, result);
	}

	/*
	 * Suspend the VDO, writing out all dirty metadata if the no-flush flag
	 * was not set on the dmsetup suspend call. This will ensure that we
	 * don't have cause to write while suspended [VDO-4402].
	 */
	int suspend_result = suspend_kvdo(&layer->kvdo);
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
	if (get_kernel_layer_state(layer) == LAYER_RUNNING) {
		return VDO_SUCCESS;
	}

	int result = resume_kvdo(&layer->kvdo);
	if (result != VDO_SUCCESS) {
		return result;
	}

	set_kernel_layer_state(layer, LAYER_RUNNING);
	return VDO_SUCCESS;
}

/***********************************************************************/
int prepare_to_resize_physical(struct kernel_layer *layer,
			       BlockCount physical_count)
{
	logInfo("Preparing to resize physical to %llu", physical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	int result =
		kvdo_prepare_to_grow_physical(&layer->kvdo, physical_count);
	if (result != VDO_SUCCESS) {
		// kvdo_prepare_to_grow_physical logs errors.
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

	logInfo("Done preparing to resize physical");
	return VDO_SUCCESS;
}

/***********************************************************************/
int resize_physical(struct kernel_layer *layer, BlockCount physical_count)
{
	/*
	 * We must not mark the layer as allowing allocations when it is
	 * suspended lest an allocation attempt block on writing IO to the
	 * suspended VDO.
	 */
	int result = kvdo_resize_physical(&layer->kvdo, physical_count);
	if (result != VDO_SUCCESS) {
		// kvdo_resize_physical logs errors
		return result;
	}
	return VDO_SUCCESS;
}

/***********************************************************************/
int prepare_to_resize_logical(struct kernel_layer *layer,
			      BlockCount logical_count)
{
	logInfo("Preparing to resize logical to %llu", logical_count);
	// Allocations are allowed and permissible through this non-VDO thread,
	// since IO triggered by this allocation to VDO can finish just fine.
	int result = kvdo_prepare_to_grow_logical(&layer->kvdo, logical_count);
	if (result != VDO_SUCCESS) {
		// kvdo_prepare_to_grow_logical logs errors
		return result;
	}

	logInfo("Done preparing to resize logical");
	return VDO_SUCCESS;
}

/***********************************************************************/
int resize_logical(struct kernel_layer *layer, BlockCount logical_count)
{
	logInfo("Resizing logical to %llu", logical_count);
	/*
	 * We must not mark the layer as allowing allocations when it is
	 * suspended lest an allocation attempt block on writing IO to the
	 * suspended VDO.
	 */
	int result = kvdo_resize_logical(&layer->kvdo, logical_count);
	if (result != VDO_SUCCESS) {
		// kvdo_resize_logical logs errors
		return result;
	}

	logInfo("Logical blocks now %llu", logical_count);
	return VDO_SUCCESS;
}

