// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/*
 * This file contains the main entry points for normal operations on a vdo as
 * well as functions for constructing and destroying vdo instances (in memory).
 */

#include "vdo.h"

#include <linux/device-mapper.h>
#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "bio.h"
#include "block-map.h"
#include "data-vio-pool.h"
#include "dedupe.h"
#include "device-registry.h"
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
#include "vdo-layout.h"
#include "vdo-resize.h"
#include "vdo-resize-logical.h"
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

static const struct vdo_work_queue_type default_queue_type = {
	.start = start_vdo_request_queue,
	.finish = finish_vdo_request_queue,
	.max_priority = VDO_DEFAULT_Q_MAX_PRIORITY,
	.default_priority = VDO_DEFAULT_Q_COMPLETION_PRIORITY,
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
 * vdo_make_thread() - Construct a single vdo work_queue and its associated
 *                     thread (or threads for round-robin queues).
 * @vdo: The vdo which owns the thread.
 * @thread_id: The id of the thread to create (as determined by the
 *             thread_config).
 * @type: The description of the work queue for this thread.
 * @queue_count: The number of actual threads/queues contained in the "thread".
 * @contexts: An array of queue_count contexts, one for each individual queue;
 *            may be NULL.
 *
 * Each "thread" constructed by this method is represented by a unique thread
 * id in the thread config, and completions can be enqueued to the queue and
 * run on the threads comprising this entity.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make_thread(struct vdo *vdo,
		    thread_id_t thread_id,
		    const struct vdo_work_queue_type *type,
		    unsigned int queue_count,
		    void *contexts[])
{
	struct vdo_thread *thread = &vdo->threads[thread_id];
	char queue_name[MAX_VDO_WORK_QUEUE_NAME_LEN];

	if (type == NULL) {
		type = &default_queue_type;
	}

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
	return make_work_queue(vdo->thread_name_prefix,
			       queue_name,
			       thread,
			       type,
			       queue_count,
			       contexts,
			       &thread->queue);
}

/**
 * initialize_vdo() - Do the portion of initializing a vdo which will clean
 *                    up after itself on error.
 * @vdo: The vdo being initialized
 * @config: The configuration of the vdo
 * @instance: The instance number of the vdo
 * @reason: The buffer to hold the failure reason on error
 **/
static int initialize_vdo(struct vdo *vdo,
			  struct device_config *config,
			  unsigned int instance,
			  char **reason)
{
	int result;
	zone_count_t i;

	vdo->device_config = config;
	vdo->starting_sector_offset = config->owning_target->begin;
	vdo->instance = instance;
	vdo->allocations_allowed = true;
	vdo_set_admin_state_code(&vdo->admin_state, VDO_ADMIN_STATE_NEW);
	INIT_LIST_HEAD(&vdo->device_config_list);
	vdo_initialize_admin_completion(vdo, &vdo->admin_completion);
	mutex_init(&vdo->stats_mutex);
	result = vdo_read_geometry_block(vdo_get_backing_device(vdo),
					 &vdo->geometry);
	if (result != VDO_SUCCESS) {
		*reason = "Could not load geometry block";
		return result;
	}

	result = vdo_make_thread_config(config->thread_counts,
					&vdo->thread_config);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot create thread configuration";
		return result;
	}

	uds_log_info("zones: %d logical, %d physical, %d hash; total threads: %d",
		     config->thread_counts.logical_zones,
		     config->thread_counts.physical_zones,
		     config->thread_counts.hash_zones,
		     vdo->thread_config->thread_count);

	/* Compression context storage */
	result = UDS_ALLOCATE(config->thread_counts.cpu_threads,
			      char *,
			      "LZ4 context",
			      &vdo->compression_context);
	if (result != VDO_SUCCESS) {
		*reason = "cannot allocate LZ4 context";
		return result;
	}

	for (i = 0; i < config->thread_counts.cpu_threads; i++) {
		result = UDS_ALLOCATE(LZ4_MEM_COMPRESS,
				      char,
				      "LZ4 context",
				      &vdo->compression_context[i]);
		if (result != VDO_SUCCESS) {
			*reason = "cannot allocate LZ4 context";
			return result;
		}
	}

	result = vdo_register(vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot add VDO to device registry";
		return result;
	}

	vdo_set_admin_state_code(&vdo->admin_state,
				 VDO_ADMIN_STATE_INITIALIZED);
	return result;
}

/**
 * vdo_make() - Allocate and initialize a vdo.
 * @instance: Device instantiation counter.
 * @config: The device configuration.
 * @reason: The reason for any failure during this call.
 * @vdo_ptr: A pointer to hold the created vdo.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_make(unsigned int instance,
	     struct device_config *config,
	     char **reason,
	     struct vdo **vdo_ptr)
{
	int result;
	struct vdo *vdo;

	/* VDO-3769 - Set a generic reason so we don't ever return garbage. */
	*reason = "Unspecified error";

	result = UDS_ALLOCATE(1, struct vdo, __func__, &vdo);
	if (result != UDS_SUCCESS) {
		*reason = "Cannot allocate VDO";
		vdo_release_instance(instance);
		return result;
	}

	result = initialize_vdo(vdo, config, instance, reason);
	if (result != VDO_SUCCESS) {
		vdo_destroy(vdo);
		return result;
	}

	/* From here on, the caller will clean up if there is an error. */
	*vdo_ptr = vdo;

	snprintf(vdo->thread_name_prefix,
		 sizeof(vdo->thread_name_prefix),
		 "%s%u",
		 MODULE_NAME,
		 instance);
	BUG_ON(vdo->thread_name_prefix[0] == '\0');
	result = UDS_ALLOCATE(vdo->thread_config->thread_count,
			      struct vdo_thread,
			      __func__,
			      &vdo->threads);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate thread structures";
		return result;
	}

	result = vdo_make_thread(vdo,
				 vdo->thread_config->admin_thread,
				 &default_queue_type,
				 1,
				 NULL);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot make admin thread";
		return result;
	}

	result = vdo_make_flusher(vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot make flusher zones";
		return result;
	}

	result = vdo_make_packer(vdo, DEFAULT_PACKER_BINS, &vdo->packer);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot make packer zones";
		return result;
	}

	BUG_ON(vdo->device_config->logical_block_size <= 0);
	BUG_ON(vdo->device_config->owned_device == NULL);
	result = make_data_vio_pool(vdo,
				    MAXIMUM_VDO_USER_VIOS,
				    MAXIMUM_VDO_USER_VIOS * 3 / 4,
				    &vdo->data_vio_pool);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot allocate data_vio pool";
		return result;
	}

	result = vdo_make_io_submitter(config->thread_counts.bio_threads,
				       config->thread_counts.bio_rotation_interval,
				       get_data_vio_pool_request_limit(vdo->data_vio_pool),
				       vdo,
				       &vdo->io_submitter);
	if (result != VDO_SUCCESS) {
		*reason = "bio submission initialization failed";
		return result;
	}

	if (vdo_uses_bio_ack_queue(vdo)) {
		result = vdo_make_thread(vdo,
					 vdo->thread_config->bio_ack_thread,
					 &bio_ack_q_type,
					 config->thread_counts.bio_ack_threads,
					 NULL);
		if (result != VDO_SUCCESS) {
			*reason = "bio ack queue initialization failed";
			return result;
		}
	}

	result = vdo_make_thread(vdo,
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
	vdo_finish_dedupe_index(vdo->hash_zones);

	for (i = 0; i < vdo->thread_config->thread_count; i++) {
		finish_work_queue(vdo->threads[i].queue);
	}
}

/**
 * vdo_destroy() - Destroy a vdo instance.
 * @vdo: The vdo to destroy (may be NULL).
 */
void vdo_destroy(struct vdo *vdo)
{
	int i;

	if (vdo == NULL) {
		return;
	}

	/* A running VDO should never be destroyed without suspending first. */
	BUG_ON(vdo_get_admin_state(vdo)->normal);

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
	free_data_vio_pool(vdo->data_vio_pool);
	vdo_free_io_submitter(UDS_FORGET(vdo->io_submitter));
	vdo_free_flusher(UDS_FORGET(vdo->flusher));
	vdo_free_packer(UDS_FORGET(vdo->packer));
	vdo_free_recovery_journal(UDS_FORGET(vdo->recovery_journal));
	vdo_free_slab_depot(UDS_FORGET(vdo->depot));
	vdo_free_layout(UDS_FORGET(vdo->layout));
	vdo_free_super_block(UDS_FORGET(vdo->super_block));
	vdo_free_block_map(UDS_FORGET(vdo->block_map));
	vdo_free_hash_zones(UDS_FORGET(vdo->hash_zones));
	vdo_free_physical_zones(UDS_FORGET(vdo->physical_zones));
	vdo_free_logical_zones(UDS_FORGET(vdo->logical_zones));
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
 * pool_stats_release() - Signal that sysfs stats have been shut down.
 * @directory: The vdo stats directory.
 */
static void pool_stats_release(struct kobject *directory)
{
	struct vdo *vdo = container_of(directory, struct vdo, stats_directory);

	complete(&vdo->stats_shutdown);
}

ATTRIBUTE_GROUPS(vdo_pool_stats);
static struct kobj_type stats_directory_type = {
	.release = pool_stats_release,
	.sysfs_ops = &vdo_pool_stats_sysfs_ops,
	.default_groups = vdo_pool_stats_groups,
};

/**
 * vdo_add_sysfs_stats_dir() - Add the stats directory to the vdo sysfs
 *                             directory.
 * @vdo: The vdo.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_add_sysfs_stats_dir(struct vdo *vdo)
{
	int result;

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
 * vdo_prepare_to_modify() - Prepare to modify a vdo.
 * @vdo: The vdo being resumed.
 * @config: The new device configuration.
 * @may_grow: Set to true if growing the logical and physical size of
 *            the vdo is currently permitted.
 * @error_ptr: A pointer to store the reason for any failure.
 *
 * This method is called during preresume to prepare for modifications which
 * could result if the table has changed.
 *
 * Return: VDO_SUCCESS or an error.
 */
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
 * vdo_get_backing_device() - Get the block device object underlying a vdo.
 * @vdo: The vdo.
 *
 * Return: The vdo's current block device.
 */
struct block_device *vdo_get_backing_device(const struct vdo *vdo)
{
	return vdo->device_config->owned_device->bdev;
}

/**
 * vdo_get_device_name() - Get the device name associated with the vdo target.
 * @target: The target device interface.
 *
 * Return: The block device name.
 */
const char *vdo_get_device_name(const struct dm_target *target)
{
	return dm_device_name(dm_table_get_md(target->table));
}

/**
 * vdo_synchronous_flush() - Issue a flush request and wait for it to
 *                           complete.
 * @vdo: The vdo.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_synchronous_flush(struct vdo *vdo)
{
	int result;
	struct bio bio;

#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0))
#endif

#if USE_ALTERNATE
	bio_init(&bio, 0, 0);
	bio_set_dev(&bio, vdo_get_backing_device(vdo));
	bio.bi_opf = REQ_OP_WRITE | REQ_PREFLUSH;
#else
	bio_init(&bio, vdo_get_backing_device(vdo), 0, 0,
		 REQ_OP_WRITE | REQ_PREFLUSH);
#endif
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
 * vdo_get_state() - Get the current state of the vdo.
 * @vdo: The vdo.

 * Context: This method may be called from any thread.
 *
 * Return: The current state of the vdo.
 */
enum vdo_state vdo_get_state(const struct vdo *vdo)
{
	enum vdo_state state = atomic_read(&vdo->state);

	smp_rmb();
	return state;
}

/**
 * vdo_set_state() - Set the current state of the vdo.
 * @vdo: The vdo whose state is to be set.
 * @state: The new state of the vdo.
 *
 * Context: This method may be called from any thread.
 */
void vdo_set_state(struct vdo *vdo, enum vdo_state state)
{
	smp_wmb();
	atomic_set(&vdo->state, state);
}

/**
 * vdo_get_admin_state() - Get the admin state of the vdo.
 * @vdo: The vdo.
 *
 * Return: The code for the vdo's current admin state.
 */
const struct admin_state_code *vdo_get_admin_state(const struct vdo *vdo)
{
	return vdo_get_admin_state_code(&vdo->admin_state);
}

/**
 * record_vdo() - Record the state of the VDO for encoding in the super block.
 */
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
 * vdo_save_components() - Encode the vdo and save the super block
 *                         asynchronously.
 * @vdo: The vdo whose state is being saved.
 * @parent: The completion to notify when the save is complete.
 *
 * All non-user mode super block savers should use this bottle neck instead of
 * calling vdo_save_super_block() directly.
 */
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
 * notify_vdo_of_read_only_mode() - Notify a vdo that it is going read-only.
 * @listener: The vdo.
 * @parent: The completion to notify in order to acknowledge the notification.
 *
 * This will save the read-only state to the super block.
 *
 * Implements vdo_read_only_notification.
 */
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
 * vdo_enable_read_only_entry() - Enable a vdo to enter read-only mode on
 *                                errors.
 * @vdo: The vdo to enable.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_enable_read_only_entry(struct vdo *vdo)
{
	return vdo_register_read_only_listener(vdo->read_only_notifier,
					       vdo,
					       notify_vdo_of_read_only_mode,
					       vdo->thread_config->admin_thread);
}

/**
 * vdo_in_read_only_mode() - Check whether a vdo is in read-only mode.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo is in read-only mode.
 */
bool vdo_in_read_only_mode(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_READ_ONLY_MODE);
}

/**
 * vdo_in_recovery_mode() - Check whether the vdo is in recovery mode.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo is in recovery mode.
 */
bool vdo_in_recovery_mode(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_RECOVERING);
}

/**
 * vdo_enter_recovery_mode() - Put the vdo into recovery mode.
 * @vdo: The vdo.
 */
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
 * set_compression_callback() - Callback to turn compression on or off.
 * @completion: The completion.
 */
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
 * vdo_set_compressing() - Turn compression on or off.
 * @vdo: The vdo.
 * @enable: Whether to enable or disable compression.
 *
 * Return: Whether compression was previously on or off.
 */
bool vdo_set_compressing(struct vdo *vdo, bool enable)
{
	vdo_perform_synchronous_action(vdo,
				       set_compression_callback,
				       vdo->thread_config->packer_thread,
				       &enable);
	return enable;
}

/**
 * vdo_get_compressing() - Get whether compression is enabled in a vdo.
 * @vdo: The vdo.
 *
 * Return: State of compression.
 */
bool vdo_get_compressing(struct vdo *vdo)
{
	return READ_ONCE(vdo->compressing);
}

static size_t get_block_map_cache_size(const struct vdo *vdo)
{
	return ((size_t) vdo->device_config->cache_size) * VDO_BLOCK_SIZE;
}

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
 * vdo_get_physical_blocks_allocated() - Get the number of physical blocks in
 *                                       use by user data.
 * @vdo: The vdo.
 *
 * Return: The number of blocks allocated for user data.
 */
static block_count_t __must_check
vdo_get_physical_blocks_allocated(const struct vdo *vdo)
{
	return (vdo_get_slab_depot_allocated_blocks(vdo->depot) -
		vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal));
}

/**
 * vdo_get_physical_blocks_overhead() - Get the number of physical blocks used
 *                                      by vdo metadata.
 * @vdo: The vdo.
 *
 * Return: The number of overhead blocks.
 */
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

static const char *vdo_describe_state(enum vdo_state state)
{
	/* These strings should all fit in the 15 chars of VDOStatistics.mode. */
	switch (state) {
	case VDO_RECOVERING:
		return "recovering";

	case VDO_READ_ONLY_MODE:
		return "read-only";

	default:
		return "normal";
	}
}

/**
 * get_vdo_statistics() - Populate a vdo_statistics structure on the admin
 *                        thread.
 * @vdo: The vdo.
 * @stats: The statistics structure to populate.
 */
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
	vdo_get_dedupe_statistics(vdo->hash_zones, stats);
	stats->errors = get_vdo_error_statistics(vdo);
	stats->in_recovery_mode = (state == VDO_RECOVERING);
	snprintf(stats->mode,
		 sizeof(stats->mode),
		 "%s",
		 vdo_describe_state(state));

	stats->instance = vdo->instance;
	stats->current_vios_in_progress =
		get_data_vio_pool_active_requests(vdo->data_vio_pool);
	stats->max_vios =
		get_data_vio_pool_maximum_requests(vdo->data_vio_pool);

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
}

/**
 * vdo_fetch_statistics_callback() - Action to populate a vdo_statistics
 *                                   structure on the admin thread.
 * @completion: The completion.
 *
 * This callback is registered in vdo_fetch_statistics().
 */
static void vdo_fetch_statistics_callback(struct vdo_completion *completion)
{
	get_vdo_statistics(completion->vdo, completion->parent);
	vdo_complete_completion(completion);
}

/**
 * vdo_fetch_statistics() - Fetch statistics on the correct thread.
 * @vdo: The vdo.
 * @stats: The vdo statistics are returned here.
 */
void vdo_fetch_statistics(struct vdo *vdo, struct vdo_statistics *stats)
{
	vdo_perform_synchronous_action(vdo,
				       vdo_fetch_statistics_callback,
				       vdo->thread_config->admin_thread,
				       stats);
}

/**
 * vdo_get_callback_thread_id() - Get the id of the callback thread on which a
 *                                completion is currently running.
 *
 * Return: The current thread ID, or -1 if no such thread.
 */
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
 * vdo_dump_status() - Dump status information about a vdo to the log for
 *                     debugging.
 * @vdo: The vdo to dump.
 */
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
		vdo_dump_physical_zone(&vdo->physical_zones->zones[zone]);
	}

	vdo_dump_hash_zones(vdo->hash_zones);
}

/**
 * vdo_assert_on_admin_thread() - Assert that we are running on the admin
 *                                thread.
 * @vdo: The vdo.
 * @name: The name of the function which should be running on the admin
 *        thread (for logging).
 */
void vdo_assert_on_admin_thread(const struct vdo *vdo, const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo->thread_config->admin_thread),
			"%s called on admin thread",
			name);
}

/**
 * vdo_assert_on_logical_zone_thread() - Assert that this function was called
 *                                       on the specified logical zone thread.
 * @vdo: The vdo.
 * @logical_zone: The number of the logical zone.
 * @name: The name of the calling function.
 */
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
 * vdo_assert_on_physical_zone_thread() - Assert that this function was called
 *                                        on the specified physical zone
 *                                        thread.
 * @vdo: The vdo.
 * @physical_zone: The number of the physical zone.
 * @name: The name of the calling function.
 */
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

void assert_on_vdo_cpu_thread(const struct vdo *vdo, const char *name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo->thread_config->cpu_thread),
			"%s called on cpu thread",
			name);
}

/**
 * vdo_get_physical_zone() - Get the physical zone responsible for a given
 *                           physical block number.
 * @vdo: The vdo containing the physical zones.
 * @pbn: The PBN of the data block.
 * @zone_ptr: A pointer to return the physical zone.
 *
 * Gets the physical zone responsible for a given physical block number of a
 * data block in this vdo instance, or of the zero block (for which a NULL
 * zone is returned). For any other block number that is not in the range of
 * valid data block numbers in any slab, an error will be returned. This
 * function is safe to call on invalid block numbers; it will not put the vdo
 * into read-only mode.
 *
 * Return: VDO_SUCCESS or VDO_OUT_OF_RANGE if the block number is invalid
 *         or an error code for any other failure.
 */
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

	*zone_ptr =
		&vdo->physical_zones->zones[vdo_get_slab_zone_number(slab)];
	return VDO_SUCCESS;
}

/**
 * vdo_get_bio_zone() - Get the bio queue zone for submitting I/O to a given
 *                      physical block number.
 * @vdo: The vdo to query.
 * @pbn: The physical block number of the I/O to be sent.
 *
 * Return: The bio queue zone number for submitting I/O to the specified pbn.
 */
zone_count_t
vdo_get_bio_zone(const struct vdo *vdo, physical_block_number_t pbn)
{
	return ((pbn
		 / vdo->device_config->thread_counts.bio_rotation_interval)
		% vdo->thread_config->bio_thread_count);
}
