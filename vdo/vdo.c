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

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdo.h"

#include <linux/device-mapper.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "device-registry.h"
#include "hash-zone.h"
#include "header.h"
#include "instance-number.h"
#include "io-submitter.h"
#include "logical-zone.h"
#include "num-utils.h"
#include "packer.h"
#include "physical-zone.h"
#include "pool-sysfs.h"
#include "read-only-notifier.h"
#include "recovery-journal.h"
#include "release-versions.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "statistics.h"
#include "status-codes.h"
#include "super-block.h"
#include "super-block-codec.h"
#include "sync-completion.h"
#include "thread-config.h"
#include "vdo-component-states.h"
#include "vdo-init.h"
#include "vdo-layout.h"
#include "vdo-resize.h"
#include "vdo-resize-logical.h"

#include "bio.h"
#include "batchProcessor.h"
#include "bufferPool.h"
#include "dedupeIndex.h"
#include "workQueue.h"

enum { PARANOID_THREAD_CONSISTENCY_CHECKS = 0 };

static void start_vdo_request_queue(void *ptr)
{
	struct vdo_thread *thread
		= get_work_queue_owner(get_current_work_queue());

	uds_register_allocating_thread(&thread->allocating_thread,
				       &thread->vdo->allocations_allowed);
}

static void finish_vdo_request_queue(void *ptr)
{
	uds_unregister_allocating_thread();
}

#ifdef MODULE
#define MODULE_NAME THIS_MODULE->name
#else
#define MODULE_NAME "dm-vdo"
#endif  /* MODULE */

static const struct vdo_work_queue_type request_queue_type = {
	.start = start_vdo_request_queue,
	.finish = finish_vdo_request_queue,
	.max_priority = VDO_REQ_Q_MAX_PRIORITY,
	.default_priority = VDO_REQ_Q_COMPLETION_PRIORITY,
};

static const struct vdo_work_queue_type bio_ack_q_type = {
	.start = NULL,
	.finish = NULL,
	.max_priority = BIO_ACK_Q_MAX_PRIORITY,
	.default_priority = BIO_ACK_Q_ACK_PRIORITY,
};

static const struct vdo_work_queue_type cpu_q_type = {
	.start = NULL,
	.finish = NULL,
	.max_priority = CPU_Q_MAX_PRIORITY,
	.default_priority = CPU_Q_MAX_PRIORITY,
};

/**
 * Construct a single vdo work_queue and its associated thread (or threads for
 * round-robin queues). Each "thread" constructed by this method is represented
 * by a unique thread id in the thread config, and completions can be enqueued
 * to the queue and run on the threads comprising this entity.
 *
 * @param vdo                 The vdo which owns the thread
 * @param thread_name_prefix  The per-device prefix for the thread name
 * @param thread_id           The id of the thread to create (as determined by
 *                            the thread_config)
 * @param type                The description of the work queue for this thread
 * @param queue_count         The number of actual threads/queues contained in
 *                            the "thread"
 * @param contexts            An array of queue_count contexts one for each
 *                            individual queue, may be NULL &
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_make_thread(struct vdo *vdo,
		    const char *thread_name_prefix,
		    thread_id_t thread_id,
		    const struct vdo_work_queue_type *type,
		    unsigned int queue_count,
		    void *contexts[])
{
	struct vdo_thread *thread = &vdo->threads[thread_id];
	char queue_name[MAX_VDO_WORK_QUEUE_NAME_LEN];

	if (thread->queue != NULL) {
		return ASSERT(vdo_work_queue_type_is(thread->queue, type),
			      "already constructed vdo thread %u is of the correct type",
			      thread_id);
	}

	thread->vdo = vdo;
	thread->thread_id = thread_id;
	vdo_get_thread_name(vdo->thread_config,
			    thread_id,
			    queue_name,
			    sizeof(queue_name));
	return make_work_queue(thread_name_prefix,
			       queue_name,
			       thread,
			       type,
			       queue_count,
			       contexts,
			       &thread->queue);
}

/**
 * Make a set of request threads.
 *
 *
 * @param vdo                 The vdo to be initialized
 * @param thread_name_prefix  The per-device prefix to use in thread
 * @param count               The number of threads to make
 * @param ids                 The ids of the threads to make
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
vdo_make_request_thread_group(struct vdo *vdo,
			      const char *thread_name_prefix,
			      thread_count_t count,
			      thread_id_t ids[])
{
	thread_count_t thread;
	int result;

	for (thread = 0; thread < count; thread++) {
		result = vdo_make_thread(vdo,
					 thread_name_prefix,
					 ids[thread],
					 &request_queue_type,
					 1,
					 NULL);
		if (result != VDO_SUCCESS) {
			return result;
		}
	}

	return VDO_SUCCESS;
}


/**
 * Make base threads.
 *
 * @param [in]  vdo                 The vdo to be initialized
 * @param [in]  thread_name_prefix  The per-device prefix to use in thread
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
vdo_make_request_threads(struct vdo *vdo, const char *thread_name_prefix)
{
	int result;
	const struct thread_config *thread_config = vdo->thread_config;
	thread_id_t singletons[] = {
		thread_config->admin_thread,
		thread_config->journal_thread,
		thread_config->packer_thread,
	};

	result = vdo_make_request_thread_group(vdo,
					       thread_name_prefix,
					       3,
					       singletons);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_request_thread_group(vdo,
					       thread_name_prefix,
					       thread_config->logical_zone_count,
					       thread_config->logical_threads);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_make_request_thread_group(vdo,
					       thread_name_prefix,
					       thread_config->physical_zone_count,
					       thread_config->physical_threads);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_make_request_thread_group(vdo,
					     thread_name_prefix,
					     thread_config->hash_zone_count,
					     thread_config->hash_zone_threads);
}

/**
 * Allocate and initialize a vdo.
 *
 * @param instance   Device instantiation counter
 * @param config     The device configuration
 * @param reason     The reason for any failure during this call
 * @param vdo_ptr    A pointer to hold the created vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_make(unsigned int instance,
	     struct device_config *config,
	     char **reason,
	     struct vdo **vdo_ptr)
{
	int result;
	struct vdo *vdo;
	char thread_name_prefix[MAX_VDO_WORK_QUEUE_NAME_LEN];

	/* VDO-3769 - Set a generic reason so we don't ever return garbage. */
	*reason = "Unspecified error";

	result = UDS_ALLOCATE(1, struct vdo, __func__, &vdo);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO";
		vdo_release_instance(instance);
		return result;
	}

	result = vdo_initialize_internal(vdo, config, instance, reason);
	if (result != VDO_SUCCESS) {
		return result;
	}

	/* From here on, the caller will clean up if there is an error. */
	*vdo_ptr = vdo;

	snprintf(thread_name_prefix,
		 sizeof(thread_name_prefix),
		 "%s%u",
		 MODULE_NAME,
		 instance);
	BUG_ON(thread_name_prefix[0] == '\0');
	result = UDS_ALLOCATE(vdo->thread_config->thread_count,
			      struct vdo_thread,
			      __func__,
			      &vdo->threads);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate thread structures";
		return result;
	}

	/* Request threads, etc */
	result = vdo_make_request_threads(vdo, thread_name_prefix);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot initialize request queues";
		return result;
	}

	result = make_batch_processor(vdo,
				      return_data_vio_batch_to_pool,
				      vdo,
				      &vdo->data_vio_releaser);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate vio-freeing batch processor";
		return result;
	}

	/* Dedupe Index */
	result = vdo_make_dedupe_index(&vdo->dedupe_index,
				       vdo,
				       thread_name_prefix);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot initialize dedupe index";
		return result;
	}

	/*
	 * Part 3 - Do initializations that depend upon other previous
	 * initializations, but have no order dependencies at freeing time.
	 * Order dependencies for initialization are identified using BUG_ON.
	 */

	/* Data vio pool */
	BUG_ON(vdo->device_config->logical_block_size <= 0);
	BUG_ON(vdo->request_limiter.limit <= 0);
	BUG_ON(vdo->device_config->owned_device == NULL);
	result = make_data_vio_buffer_pool(vdo->request_limiter.limit,
					   &vdo->data_vio_pool);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate vio data";
		return result;
	}

	/* Bio queue */
	result = vdo_make_io_submitter(thread_name_prefix,
				       config->thread_counts.bio_threads,
				       config->thread_counts.bio_rotation_interval,
				       vdo->request_limiter.limit,
				       vdo,
				       &vdo->io_submitter);
	if (result != VDO_SUCCESS) {
		*reason = "bio submission initialization failed";
		return result;
	}

	/* Bio ack queue */
	if (vdo_uses_bio_ack_queue(vdo)) {
		result = vdo_make_thread(vdo,
					 thread_name_prefix,
					 vdo->thread_config->bio_ack_thread,
					 &bio_ack_q_type,
					 config->thread_counts.bio_ack_threads,
					 NULL);
		if (result != VDO_SUCCESS) {
			*reason = "bio ack queue initialization failed";
			return result;
		}
	}

	/* CPU Queues */
	result = vdo_make_thread(vdo,
				 thread_name_prefix,
				 vdo->thread_config->cpu_thread,
				 &cpu_q_type,
				 config->thread_counts.cpu_threads,
				 (void **) vdo->compression_context);
	if (result != VDO_SUCCESS) {
		*reason = "CPU queue initialization failed";
		return result;
	}

	return VDO_SUCCESS;
}

static void finish_vdo(struct vdo *vdo)
{
	int i;

	if (vdo->threads == NULL) {
		return;
	}

	vdo_cleanup_io_submitter(vdo->io_submitter);
	vdo_finish_dedupe_index(vdo->dedupe_index);

	for (i = 0; i < vdo->thread_config->thread_count; i++) {
		finish_work_queue(vdo->threads[i].queue);
	}

	free_buffer_pool(UDS_FORGET(vdo->data_vio_pool));
	free_batch_processor(UDS_FORGET(vdo->data_vio_releaser));
}

/**
 * Destroy a vdo instance.
 *
 * @param vdo  The vdo to destroy (may be NULL)
 **/
void vdo_destroy(struct vdo *vdo)
{
	int i;
	zone_count_t zone;
	const struct thread_config *thread_config;

	if (vdo == NULL) {
		return;
	}

	/* A running VDO should never be destroyed without suspending first. */
	BUG_ON(vdo_get_admin_state(vdo)->normal);

	thread_config = vdo->thread_config;
	vdo->allocations_allowed = true;

	/*
	 * Stop services that need to gather VDO statistics from the worker
	 * threads.
	 */
	if (vdo->sysfs_added) {
		init_completion(&vdo->stats_shutdown);
		kobject_put(&vdo->stats_directory);
		wait_for_completion(&vdo->stats_shutdown);
	}

	finish_vdo(vdo);
	vdo_unregister(vdo);
	vdo_free_io_submitter(UDS_FORGET(vdo->io_submitter));
	vdo_free_dedupe_index(UDS_FORGET(vdo->dedupe_index));
	vdo_free_flusher(UDS_FORGET(vdo->flusher));
	vdo_free_packer(UDS_FORGET(vdo->packer));
	vdo_free_recovery_journal(UDS_FORGET(vdo->recovery_journal));
	vdo_free_slab_depot(UDS_FORGET(vdo->depot));
	vdo_free_layout(UDS_FORGET(vdo->layout));
	vdo_free_super_block(UDS_FORGET(vdo->super_block));
	vdo_free_block_map(UDS_FORGET(vdo->block_map));

	if (vdo->hash_zones != NULL) {
		for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
			vdo_free_hash_zone(UDS_FORGET(vdo->hash_zones[zone]));
		}
	}
	UDS_FREE(vdo->hash_zones);
	vdo->hash_zones = NULL;

	vdo_free_logical_zones(UDS_FORGET(vdo->logical_zones));

	if (vdo->physical_zones != NULL) {
		for (zone = 0;
		     zone < thread_config->physical_zone_count;
		     zone++) {
			vdo_destroy_physical_zone(&vdo->physical_zones[zone]);
		}

		UDS_FREE(UDS_FORGET(vdo->physical_zones));
	}

	vdo_free_read_only_notifier(UDS_FORGET(vdo->read_only_notifier));

	if (vdo->threads != NULL) {
		for (i = 0; i < vdo->thread_config->thread_count; i++) {
			free_work_queue(UDS_FORGET(vdo->threads[i].queue));
		}
		UDS_FREE(UDS_FORGET(vdo->threads));
	}

	vdo_free_thread_config(UDS_FORGET(vdo->thread_config));

	if (vdo->compression_context != NULL) {
		for (i = 0;
		     i < vdo->device_config->thread_counts.cpu_threads;
		     i++) {
			UDS_FREE(UDS_FORGET(vdo->compression_context[i]));
		}

		UDS_FREE(UDS_FORGET(vdo->compression_context));
	}

	vdo_release_instance(vdo->instance);

	/*
	 * The call to kobject_put on the kobj sysfs node will decrement its
	 * reference count; when the count goes to zero the VDO object will be
	 * freed as a side effect.
	 */
	if (!vdo->sysfs_added) {
		UDS_FREE(vdo);
	} else {
		kobject_put(&vdo->vdo_directory);
	}
}

/**
 * Signal that sysfs stats have been shut down.
 *
 * @param directory  The vdo stats directory
 **/
static void pool_stats_release(struct kobject *directory)
{
	struct vdo *vdo = container_of(directory, struct vdo, stats_directory);

	complete(&vdo->stats_shutdown);
}

/**
 * Add the stats directory to the vdo sysfs directory.
 *
 * @param vdo  The vdo
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_add_sysfs_stats_dir(struct vdo *vdo)
{
	int result;
	static struct kobj_type stats_directory_type = {
		.release = pool_stats_release,
		.sysfs_ops = &vdo_pool_stats_sysfs_ops,
		.default_attrs = vdo_pool_stats_attrs,
	};

	kobject_init(&vdo->stats_directory, &stats_directory_type);
	result = kobject_add(&vdo->stats_directory,
			     &vdo->vdo_directory,
			     "statistics");
	if (result != 0) {
		return VDO_CANT_ADD_SYSFS_NODE;
	}

	return VDO_SUCCESS;
}

/**
 * Prepare to modify a vdo. This method is called during preresume to prepare
 * for modifications which could result if the table has changed.
 *
 * @param vdo        The vdo being resumed
 * @param config     The new device configuration
 * @param may_grow   Set to true if growing the logical and physical size of
 *                   the vdo is currently permitted
 * @param error_ptr  A pointer to store the reason for any failure
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_prepare_to_modify(struct vdo *vdo,
			  struct device_config *config,
			  bool may_grow,
			  char **error_ptr)
{
	int result = vdo_validate_new_device_config(config,
						    vdo->device_config,
						    may_grow,
						    error_ptr);
	if (result != VDO_SUCCESS) {
		return -EINVAL;
	}

	if (config->logical_blocks > vdo->device_config->logical_blocks) {
		result = vdo_prepare_to_grow_logical(vdo,
						     config->logical_blocks);
		if (result != VDO_SUCCESS) {
			*error_ptr = "Device vdo_prepare_to_grow_logical failed";
			return result;
		}
	}

	if (config->physical_blocks > vdo->device_config->physical_blocks) {
		result = vdo_prepare_to_grow_physical(vdo,
						      config->physical_blocks);
		if (result != VDO_SUCCESS) {
			if (result == VDO_PARAMETER_MISMATCH) {
				/*
				 * If we don't trap this case,
				 * vdo_map_to_system_error() will remap it to
				 * -EIO, which is misleading and ahistorical.
				 */
				result = -EINVAL;
			}

			if (result == VDO_TOO_MANY_SLABS) {
				*error_ptr = "Device vdo_prepare_to_grow_physical failed (specified physical size too big based on formatted slab size)";
			} else {
				*error_ptr = "Device vdo_prepare_to_grow_physical failed";
			}
			return result;
		}
	}

	if (strcmp(config->parent_device_name,
		   vdo->device_config->parent_device_name) != 0) {
		const char *device_name
			= vdo_get_device_name(config->owning_target);
		uds_log_info("Updating backing device of %s from %s to %s",
			     device_name,
			     vdo->device_config->parent_device_name,
			     config->parent_device_name);
	}

	return VDO_SUCCESS;
}

/**
 * Get the block device object underlying a vdo.
 *
 * @param vdo  The vdo
 *
 * @return The vdo's current block device
 **/
struct block_device *vdo_get_backing_device(const struct vdo *vdo)
{
	return vdo->device_config->owned_device->bdev;
}

/**
 * Issue a flush request and wait for it to complete.
 *
 * @param vdo  The vdo
 *
 * @return VDO_SUCCESS or an error
 */
int vdo_synchronous_flush(struct vdo *vdo)
{
	int result;
	struct bio bio;

	bio_init(&bio, 0, 0);
	bio_set_dev(&bio, vdo_get_backing_device(vdo));
	bio.bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
	submit_bio_wait(&bio);
	result = blk_status_to_errno(bio.bi_status);

	atomic64_inc(&vdo->stats.flush_out);
	if (result != 0) {
		uds_log_error_strerror(result, "synchronous flush failed");
		result = -EIO;
	}

	bio_uninit(&bio);
	return result;
}

/**
 * Get the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo  The vdo
 *
 * @return the current state of the vdo
 **/
enum vdo_state vdo_get_state(const struct vdo *vdo)
{
	enum vdo_state state = atomic_read(&vdo->state);

	smp_rmb();
	return state;
}

/**
 * Set the current state of the vdo. This method may be called from any thread.
 *
 * @param vdo    The vdo whose state is to be set
 * @param state  The new state of the vdo
 **/
void vdo_set_state(struct vdo *vdo, enum vdo_state state)
{
	smp_wmb();
	atomic_set(&vdo->state, state);
}

/**
 * Get the admin state of the vdo.
 *
 * @param vdo  The vdo
 *
 * @return The code for the vdo's current admin state
 **/
const struct admin_state_code *vdo_get_admin_state(const struct vdo *vdo)
{
	return vdo_get_admin_state_code(&vdo->admin_state);
}

/**
 * Record the state of the VDO for encoding in the super block.
 **/
static void record_vdo(struct vdo *vdo)
{
	vdo->states.release_version = vdo->geometry.release_version;
	vdo->states.vdo.state = vdo_get_state(vdo);
	vdo->states.block_map = vdo_record_block_map(vdo->block_map);
	vdo->states.recovery_journal =
		vdo_record_recovery_journal(vdo->recovery_journal);
	vdo->states.slab_depot = vdo_record_slab_depot(vdo->depot);
	vdo->states.layout = vdo_get_fixed_layout(vdo->layout);
}

/**
 * Encode the vdo and save the super block asynchronously. All non-user mode
 * super block savers should use this bottle neck instead of calling
 * vdo_save_super_block() directly.
 *
 * @param vdo     The vdo whose state is being saved
 * @param parent  The completion to notify when the save is complete
 **/
void vdo_save_components(struct vdo *vdo, struct vdo_completion *parent)
{
	int result;

	struct buffer *buffer
		= vdo_get_super_block_codec(vdo->super_block)->component_buffer;
	record_vdo(vdo);
	result = vdo_encode_component_states(buffer, &vdo->states);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	vdo_save_super_block(vdo->super_block,
			     vdo_get_data_region_start(vdo->geometry),
			     parent);
}

/**
 * Notify a vdo that it is going read-only. This will save the read-only state
 * to the super block.
 *
 * <p>Implements vdo_read_only_notification.
 *
 * @param listener  The vdo
 * @param parent    The completion to notify in order to acknowledge the
 *                  notification
 **/
static void notify_vdo_of_read_only_mode(void *listener,
				    struct vdo_completion *parent)
{
	struct vdo *vdo = listener;

	if (vdo_in_read_only_mode(vdo)) {
		vdo_complete_completion(parent);
	}

	vdo_set_state(vdo, VDO_READ_ONLY_MODE);
	vdo_save_components(vdo, parent);
}

/**
 * Enable a vdo to enter read-only mode on errors.
 *
 * @param vdo  The vdo to enable
 *
 * @return VDO_SUCCESS or an error
 **/
int vdo_enable_read_only_entry(struct vdo *vdo)
{
	return vdo_register_read_only_listener(vdo->read_only_notifier,
					       vdo,
					       notify_vdo_of_read_only_mode,
					       vdo->thread_config->admin_thread);
}

/**
 * Check whether a vdo is in read-only mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in read-only mode
 **/
bool vdo_in_read_only_mode(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_READ_ONLY_MODE);
}

/**
 * Check whether the vdo is in recovery mode.
 *
 * @param vdo  The vdo to query
 *
 * @return <code>true</code> if the vdo is in recovery mode
 **/
bool vdo_in_recovery_mode(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_RECOVERING);
}

/**
 * Put the vdo into recovery mode
 *
 * @param vdo  The vdo
 **/
void vdo_enter_recovery_mode(struct vdo *vdo)
{
	vdo_assert_on_admin_thread(vdo, __func__);

	if (vdo_in_read_only_mode(vdo)) {
		return;
	}

	uds_log_info("Entering recovery mode");
	vdo_set_state(vdo, VDO_RECOVERING);
}

/**
 * Callback to turn compression on or off.
 *
 * @param completion  The completion
 **/
static void set_compression_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	bool *enable = completion->parent;
	bool was_enabled = vdo_get_compressing(vdo);

	if (*enable != was_enabled) {
		WRITE_ONCE(vdo->compressing, *enable);
		if (was_enabled) {
			/*
			 * Signal the packer to flush since compression has
			 * been disabled.
			 */
			vdo_flush_packer(vdo->packer);
		}
	}

	uds_log_info("compression is %s", (*enable ? "enabled" : "disabled"));
	*enable = was_enabled;
	vdo_complete_completion(completion);
}

/**
 * Turn compression on or off.
 *
 * @param vdo     The vdo
 * @param enable  Whether to enable or disable compression
 *
 * @return Whether compression was previously on or off
 **/
bool vdo_set_compressing(struct vdo *vdo, bool enable)
{
	vdo_perform_synchronous_action(vdo,
				       set_compression_callback,
				       vdo->thread_config->packer_thread,
				       &enable);
	return enable;
}

/**
 * Get whether compression is enabled in a vdo.
 *
 * @param vdo  The vdo
 *
 * @return State of compression
 **/
bool vdo_get_compressing(struct vdo *vdo)
{
	return READ_ONCE(vdo->compressing);
}

/**********************************************************************/
static size_t get_block_map_cache_size(const struct vdo *vdo)
{
	return ((size_t) vdo->device_config->cache_size) * VDO_BLOCK_SIZE;
}

/**
 * Tally the hash lock statistics from all the hash zones.
 *
 * @param vdo  The vdo to query
 *
 * @return The sum of the hash lock statistics from all hash zones
 **/
static struct hash_lock_statistics
get_hash_lock_statistics(const struct vdo *vdo)
{
	zone_count_t zone_count = vdo->thread_config->hash_zone_count;
	zone_count_t zone;
	struct hash_lock_statistics totals;

	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < zone_count; zone++) {
		struct hash_lock_statistics stats =
			vdo_get_hash_zone_statistics(vdo->hash_zones[zone]);
		totals.dedupe_advice_valid += stats.dedupe_advice_valid;
		totals.dedupe_advice_stale += stats.dedupe_advice_stale;
		totals.concurrent_data_matches +=
			stats.concurrent_data_matches;
		totals.concurrent_hash_collisions +=
			stats.concurrent_hash_collisions;
	}

	return totals;
}

/**********************************************************************/
static struct error_statistics __must_check
get_vdo_error_statistics(const struct vdo *vdo)
{
	/*
	 * The error counts can be incremented from arbitrary threads and so
	 * must be incremented atomically, but they are just statistics with no
	 * semantics that could rely on memory order, so unfenced reads are
	 * sufficient.
	 */
	const struct atomic_statistics *atoms = &vdo->stats;

	return (struct error_statistics) {
		.invalid_advice_pbn_count =
			atomic64_read(&atoms->invalid_advice_pbn_count),
		.no_space_error_count =
			atomic64_read(&atoms->no_space_error_count),
		.read_only_error_count =
			atomic64_read(&atoms->read_only_error_count),
	};
}

static void copy_bio_stat(struct bio_stats *b,
			  const struct atomic_bio_stats *a)
{
	b->read = atomic64_read(&a->read);
	b->write = atomic64_read(&a->write);
	b->discard = atomic64_read(&a->discard);
	b->flush = atomic64_read(&a->flush);
	b->empty_flush = atomic64_read(&a->empty_flush);
	b->fua = atomic64_read(&a->fua);
}

static struct bio_stats subtract_bio_stats(struct bio_stats minuend,
					   struct bio_stats subtrahend)
{
	return (struct bio_stats) {
		.read = minuend.read - subtrahend.read,
		.write = minuend.write - subtrahend.write,
		.discard = minuend.discard - subtrahend.discard,
		.flush = minuend.flush - subtrahend.flush,
		.empty_flush = minuend.empty_flush - subtrahend.empty_flush,
		.fua = minuend.fua - subtrahend.fua,
	};
}


/**
 * Get the number of physical blocks in use by user data.
 *
 * @param vdo  The vdo
 *
 * @return The number of blocks allocated for user data
 **/
static block_count_t __must_check
vdo_get_physical_blocks_allocated(const struct vdo *vdo)
{
	return (vdo_get_slab_depot_allocated_blocks(vdo->depot) -
		vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**
 * Get the number of physical blocks used by vdo metadata.
 *
 * @param vdo  The vdo
 *
 * @return The number of overhead blocks
 **/
static block_count_t __must_check
vdo_get_physical_blocks_overhead(const struct vdo *vdo)
{
	/*
	 * XXX config.physical_blocks is actually mutated during resize and is in
	 * a packed structure, but resize runs on admin thread so we're usually
	 * OK.
	 */
	return (vdo->states.vdo.config.physical_blocks -
		vdo_get_slab_depot_data_blocks(vdo->depot) +
		vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**
 * Populate a vdo_statistics structure on the admin thread.
 *
 * @param vdo    The vdo
 * @param stats  The statistics structure to populate
 **/
static void get_vdo_statistics(const struct vdo *vdo,
			       struct vdo_statistics *stats)
{
	struct recovery_journal *journal = vdo->recovery_journal;
	enum vdo_state state = vdo_get_state(vdo);

	vdo_assert_on_admin_thread(vdo, __func__);

	/* start with a clean slate */
	memset(stats, 0, sizeof(struct vdo_statistics));

	/*
	 * These are immutable properties of the vdo object, so it is safe to
	 * query them from any thread.
	 */
	stats->version = STATISTICS_VERSION;
	stats->release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER;
	stats->logical_blocks = vdo->states.vdo.config.logical_blocks;
	/*
	 * XXX config.physical_blocks is actually mutated during resize and is
	 * in a packed structure, but resize runs on the admin thread so we're
	 * usually OK.
	 */
	stats->physical_blocks = vdo->states.vdo.config.physical_blocks;
	stats->block_size = VDO_BLOCK_SIZE;
	stats->complete_recoveries = vdo->states.vdo.complete_recoveries;
	stats->read_only_recoveries = vdo->states.vdo.read_only_recoveries;
	stats->block_map_cache_size = get_block_map_cache_size(vdo);

	/* The callees are responsible for thread-safety. */
	stats->data_blocks_used = vdo_get_physical_blocks_allocated(vdo);
	stats->overhead_blocks_used = vdo_get_physical_blocks_overhead(vdo);
	stats->logical_blocks_used =
		vdo_get_recovery_journal_logical_blocks_used(journal);
	vdo_get_slab_depot_statistics(vdo->depot, stats);
	stats->journal = vdo_get_recovery_journal_statistics(journal);
	stats->packer = vdo_get_packer_statistics(vdo->packer);
	stats->block_map = vdo_get_block_map_statistics(vdo->block_map);
	stats->hash_lock = get_hash_lock_statistics(vdo);
	stats->errors = get_vdo_error_statistics(vdo);
	stats->in_recovery_mode = (state == VDO_RECOVERING);
	snprintf(stats->mode,
		 sizeof(stats->mode),
		 "%s",
		 vdo_describe_state(state));
	stats->version = STATISTICS_VERSION;
	stats->release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER;
	stats->instance = vdo->instance;

	stats->current_vios_in_progress =
		READ_ONCE(vdo->request_limiter.active);
	stats->max_vios = READ_ONCE(vdo->request_limiter.maximum);

	/*
	 * vdo_get_dedupe_index_timeout_count() gives the number of timeouts, 
	 * and dedupe_context_busy gives the number of queries not made because 
	 * of earlier timeouts.
	 */
	stats->dedupe_advice_timeouts =
		(vdo_get_dedupe_index_timeout_count(vdo->dedupe_index) +
		 atomic64_read(&vdo->stats.dedupe_context_busy));
	stats->flush_out = atomic64_read(&vdo->stats.flush_out);
	stats->logical_block_size =
		vdo->device_config->logical_block_size;
	copy_bio_stat(&stats->bios_in, &vdo->stats.bios_in);
	copy_bio_stat(&stats->bios_in_partial, &vdo->stats.bios_in_partial);
	copy_bio_stat(&stats->bios_out, &vdo->stats.bios_out);
	copy_bio_stat(&stats->bios_meta, &vdo->stats.bios_meta);
	copy_bio_stat(&stats->bios_journal, &vdo->stats.bios_journal);
	copy_bio_stat(&stats->bios_page_cache, &vdo->stats.bios_page_cache);
	copy_bio_stat(&stats->bios_out_completed,
		      &vdo->stats.bios_out_completed);
	copy_bio_stat(&stats->bios_meta_completed,
		      &vdo->stats.bios_meta_completed);
	copy_bio_stat(&stats->bios_journal_completed,
		      &vdo->stats.bios_journal_completed);
	copy_bio_stat(&stats->bios_page_cache_completed,
		      &vdo->stats.bios_page_cache_completed);
	copy_bio_stat(&stats->bios_acknowledged, &vdo->stats.bios_acknowledged);
	copy_bio_stat(&stats->bios_acknowledged_partial,
		      &vdo->stats.bios_acknowledged_partial);
	stats->bios_in_progress =
		subtract_bio_stats(stats->bios_in, stats->bios_acknowledged);
	get_uds_memory_stats(&stats->memory_usage.bytes_used,
			     &stats->memory_usage.peak_bytes_used);
	vdo_get_dedupe_index_statistics(vdo->dedupe_index, &stats->index);
}

/**
 * Action to populate a vdo_statistics structure on the admin thread;
 * registered in vdo_fetch_statistics().
 *
 * @param completion  The completion
 **/
static void vdo_fetch_statistics_callback(struct vdo_completion *completion)
{
	get_vdo_statistics(completion->vdo, completion->parent);
	vdo_complete_completion(completion);
}

/**
 * Fetch statistics on the correct thread.
 *
 * @param [in]  vdo    The vdo
 * @param [out] stats  The vdo statistics are returned here
 **/
void vdo_fetch_statistics(struct vdo *vdo, struct vdo_statistics *stats)
{
	vdo_perform_synchronous_action(vdo,
				       vdo_fetch_statistics_callback,
				       vdo->thread_config->admin_thread,
				       stats);
}

/**
 * Get the id of the callback thread on which a completion is currently
 * running, or -1 if no such thread.
 *
 * @return the current thread ID
 **/
thread_id_t vdo_get_callback_thread_id(void)
{
	struct vdo_work_queue *queue = get_current_work_queue();
	struct vdo_thread *thread;
	thread_id_t thread_id;

	if (queue == NULL) {
		return VDO_INVALID_THREAD_ID;
	}

	thread = get_work_queue_owner(queue);
	thread_id = thread->thread_id;

	if (PARANOID_THREAD_CONSISTENCY_CHECKS) {
		struct vdo *vdo = thread->vdo;

		BUG_ON(thread_id >= vdo->thread_config->thread_count);
		BUG_ON(thread != &vdo->threads[thread_id]);
	}

	return thread_id;
}

/**
 * Dump status information about a vdo to the log for debugging.
 *
 * @param vdo  The vdo to dump
 **/
void vdo_dump_status(const struct vdo *vdo)
{
	const struct thread_config *thread_config = vdo->thread_config;
	zone_count_t zone;

	vdo_dump_flusher(vdo->flusher);
	vdo_dump_recovery_journal_statistics(vdo->recovery_journal);
	vdo_dump_packer(vdo->packer);
	vdo_dump_slab_depot(vdo->depot);

	for (zone = 0; zone < thread_config->logical_zone_count; zone++) {
		vdo_dump_logical_zone(&vdo->logical_zones->zones[zone]);
	}

	for (zone = 0; zone < thread_config->physical_zone_count; zone++) {
		vdo_dump_physical_zone(&vdo->physical_zones[zone]);
	}

	for (zone = 0; zone < thread_config->hash_zone_count; zone++) {
		vdo_dump_hash_zone(vdo->hash_zones[zone]);
	}
}

/**
 * Assert that we are running on the admin thread.
 *
 * @param vdo   The vdo
 * @param name  The name of the function which should be running on the admin
 *              thread (for logging).
 **/
void vdo_assert_on_admin_thread(const struct vdo *vdo, const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo->thread_config->admin_thread),
			"%s called on admin thread",
			name);
}

/**
 * Assert that this function was called on the specified logical zone thread.
 *
 * @param vdo           The vdo
 * @param logical_zone  The number of the logical zone
 * @param name          The name of the calling function
 **/
void vdo_assert_on_logical_zone_thread(const struct vdo *vdo,
				       zone_count_t logical_zone,
				       const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo_get_logical_zone_thread(vdo->thread_config,
						     logical_zone)),
			"%s called on logical thread",
			name);
}

/**
 * Assert that this function was called on the specified physical zone thread.
 *
 * @param vdo            The vdo
 * @param physical_zone  The number of the physical zone
 * @param name           The name of the calling function
 **/
void vdo_assert_on_physical_zone_thread(const struct vdo *vdo,
					zone_count_t physical_zone,
					const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo_get_physical_zone_thread(vdo->thread_config,
						      physical_zone)),
			"%s called on physical thread",
			name);
}

/**
 * Select the hash zone responsible for locking a given chunk name.
 *
 * @param vdo   The vdo containing the hash zones
 * @param name  The chunk name
 *
 * @return  The hash zone responsible for the chunk name
 **/
struct hash_zone *vdo_select_hash_zone(const struct vdo *vdo,
				       const struct uds_chunk_name *name)
{
	/*
	 * Use a fragment of the chunk name as a hash code. To ensure uniform
	 * distributions, it must not overlap with fragments used elsewhere.
	 * Eight bits of hash should suffice since the number of hash zones is
	 * small.
	 *
	 * XXX Make a central repository for these offsets ala hashUtils.
	 * XXX Verify that the first byte is independent enough.
	 */
	uint32_t hash = name->name[0];

	/*
	 * Scale the 8-bit hash fragment to a zone index by treating it as a
	 * binary fraction and multiplying that by the zone count. If the hash
	 * is uniformly distributed over [0 .. 2^8-1], then (hash * count / 2^8)
	 * should be uniformly distributed over [0 .. count-1]. The multiply and
	 * shift is much faster than a divide (modulus) on X86 CPUs.
	 */
	return vdo->hash_zones[(hash * vdo->thread_config->hash_zone_count)
			       >> 8];
}

/**
 * Get the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * @param [in]  vdo       The vdo containing the physical zones
 * @param [in]  pbn       The PBN of the data block
 * @param [out] zone_ptr  A pointer to return the physical zone
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure
 **/
int vdo_get_physical_zone(const struct vdo *vdo,
			  physical_block_number_t pbn,
			  struct physical_zone **zone_ptr)
{
	struct vdo_slab *slab;
	int result;

	if (pbn == VDO_ZERO_BLOCK) {
		*zone_ptr = NULL;
		return VDO_SUCCESS;
	}

	/*
	 * Used because it does a more restrictive bounds check than 
	 * vdo_get_slab(), and done first because it won't trigger read-only 
	 * mode on an invalid PBN.
	 */
	if (!vdo_is_physical_data_block(vdo->depot, pbn)) {
		return VDO_OUT_OF_RANGE;
	}

	/*
	 * With the PBN already checked, we should always succeed in finding a
	 * slab.
	 */
	slab = vdo_get_slab(vdo->depot, pbn);
	result =
		ASSERT(slab != NULL, "vdo_get_slab must succeed on all valid PBNs");
	if (result != VDO_SUCCESS) {
		return result;
	}

	*zone_ptr = &vdo->physical_zones[vdo_get_slab_zone_number(slab)];
	return VDO_SUCCESS;
}

/**
 * Check whether a data_location containing potential dedupe advice is
 * well-formed and addresses a data block in one of the configured physical
 * zones of the vdo. If it is, return the location and zone as a zoned_pbn;
 * otherwise increment statistics tracking invalid advice and return an
 * unmapped zoned_pbn.
 *
 * @param vdo     The vdo
 * @param advice  The advice to validate (NULL indicates no advice)
 * @param lbn     The logical block number of the write that requested advice,
 *                which is only used for debug-level logging of invalid advice
 *
 * @return The zoned_pbn representing the advice, if valid, otherwise an
 *         unmapped zoned_pbn if the advice was invalid or NULL
 **/
struct zoned_pbn
vdo_validate_dedupe_advice(struct vdo *vdo,
			   const struct data_location *advice,
			   logical_block_number_t lbn)
{
	struct zoned_pbn no_advice = { .pbn = VDO_ZERO_BLOCK };
	struct physical_zone *zone;
	int result;

	if (advice == NULL) {
		return no_advice;
	}

	/* Don't use advice that's clearly meaningless. */
	if ((advice->state == VDO_MAPPING_STATE_UNMAPPED) ||
	    (advice->pbn == VDO_ZERO_BLOCK)) {
		uds_log_debug("Invalid advice from deduplication server: pbn %llu, state %u. Giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn, advice->state,
			      (unsigned long long) lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return no_advice;
	}

	result = vdo_get_physical_zone(vdo, advice->pbn, &zone);
	if ((result != VDO_SUCCESS) || (zone == NULL)) {
		uds_log_debug("Invalid physical block number from deduplication server: %llu, giving up on deduplication of logical block %llu",
			      (unsigned long long) advice->pbn,
			      (unsigned long long) lbn);
		atomic64_inc(&vdo->stats.invalid_advice_pbn_count);
		return no_advice;
	}

	return (struct zoned_pbn) {
		.pbn = advice->pbn,
		.state = advice->state,
		.zone = zone,
	};
}

/**
 * Get the bio queue zone for submitting I/O to a given physical block number.
 *
 * @param vdo  The vdo to query
 * @param pbn  The physical block number of the I/O to be sent
 *
 * @return The bio queue zone number for submitting I/O to the specified pbn
 **/
zone_count_t
vdo_get_bio_zone(const struct vdo *vdo, physical_block_number_t pbn)
{
	return ((pbn
		 / vdo->device_config->thread_counts.bio_rotation_interval)
		% vdo->thread_config->bio_thread_count);
}


