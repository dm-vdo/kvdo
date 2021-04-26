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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dmvdo.c#114 $
 */

#include "dmvdo.h"

#include <linux/module.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "constants.h"
#include "threadConfig.h"
#include "vdo.h"

#include "dedupeIndex.h"
#include "deviceRegistry.h"
#include "dump.h"
#include "instanceNumber.h"
#include "ioSubmitter.h"
#include "kernelLayer.h"
#include "kvdoFlush.h"
#include "memoryUsage.h"
#include "messageStats.h"
#include "stringUtils.h"
#include "sysfs.h"
#include "threadDevice.h"
#include "threadRegistry.h"
#include "vdo.h"
#include "vdoInit.h"

struct vdo_module_globals vdo_globals;

/**********************************************************************/

/**
 * Get the vdo associated with a dm target structure.
 *
 * @param ti  The dm target structure
 *
 * @return The vdo NULL.
 **/
static struct vdo *get_vdo_for_target(struct dm_target *ti)
{
	return ((struct device_config *) ti->private)->vdo;
}

/**
 * Get the kernel layer associated with a dm target structure.
 *
 * @param ti  The dm target structure
 *
 * @return The kernel layer, or NULL.
 **/
static struct kernel_layer *get_kernel_layer_for_target(struct dm_target *ti)
{
	return vdo_as_kernel_layer(get_vdo_for_target(ti));
}

/**
 * Begin VDO processing of a bio.  This is called by the device mapper
 * through the "map" function, and has resulted from a bio being
 * submitted.
 *
 * @param ti   The dm_target.  We only need the "private" member to access
 *             the kernel_layer.
 * @param bio  The bio.
 *
 * @return One of these values:
 *
 *         negative            A negative value is an error code.
 *                             Usually -EIO.
 *
 *         DM_MAPIO_SUBMITTED  VDO will take care of this I/O, either
 *                             processing it completely and calling
 *                             bio_endio, or forwarding it onward by
 *                             submitting it to the next layer.
 *
 *         DM_MAPIO_REMAPPED   VDO has modified the bio and the device
 *                             mapper will immediately forward the bio
 *                             onward by submitting it to the next layer.
 *
 *         DM_MAPIO_REQUEUE    We do not use this.  It is used by device
 *                             mapper devices to defer an I/O request
 *                             during suspend/resume processing.
 **/
static int vdo_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);

	return kvdo_map_bio(layer, bio);
}

/**********************************************************************/
static void vdo_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);

	limits->logical_block_size =
		layer->vdo.device_config->logical_block_size;
	limits->physical_block_size = VDO_BLOCK_SIZE;

	// The minimum io size for random io
	blk_limits_io_min(limits, VDO_BLOCK_SIZE);
	// The optimal io size for streamed/sequential io
	blk_limits_io_opt(limits, VDO_BLOCK_SIZE);

	/*
	 * Sets the maximum discard size that will be passed into VDO. This
	 * value comes from a table line value passed in during dmsetup create.
	 *
	 * The value 1024 is the largest usable value on HD systems.  A 2048
	 * sector discard on a busy HD system takes 31 seconds.  We should use a
	 * value no higher than 1024, which takes 15 to 16 seconds on a busy HD
	 * system.
	 *
	 * But using large values results in 120 second blocked task warnings in
	 * /var/log/kern.log.  In order to avoid these warnings, we choose to
	 * use the smallest reasonable value.  See VDO-3062 and VDO-3087.
	 *
	 * The value is displayed in sysfs, and also used by dm-thin to
	 * determine whether to pass down discards. The block layer splits
	 * large discards on this boundary when this is set.
	 */
	limits->max_discard_sectors =
		layer->vdo.device_config->max_discard_blocks *
		VDO_SECTORS_PER_BLOCK;

	// Force discards to not begin or end with a partial block by stating
	// the granularity is 4k.
	limits->discard_granularity = VDO_BLOCK_SIZE;
}

/**********************************************************************/
static int vdo_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn,
			       void *data)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	sector_t len =
		block_to_sector(layer->vdo.device_config->physical_blocks);

	return fn(ti, layer->vdo.device_config->owned_device, 0, len, data);
}

/*
 * Status line is:
 *    <device> <operating mode> <in recovery> <index state>
 *    <compression state> <used physical blocks> <total physical blocks>
 */

/**********************************************************************/
static void vdo_status(struct dm_target *ti,
		       status_type_t status_type,
		       unsigned int status_flags,
		       char *result,
		       unsigned int maxlen)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct vdo_statistics *stats;
	struct device_config *device_config;
	char name_buffer[BDEVNAME_SIZE];
	// N.B.: The DMEMIT macro uses the variables named "sz", "result",
	// "maxlen".
	int sz = 0;

	switch (status_type) {
	case STATUSTYPE_INFO:
		// Report info for dmsetup status
		mutex_lock(&layer->stats_mutex);
		get_kvdo_statistics(&layer->vdo, &layer->vdo_stats_storage);
		stats = &layer->vdo_stats_storage;

		DMEMIT("/dev/%s %s %s %s %s %llu %llu",
		       bdevname(get_vdo_backing_device(&layer->vdo),
				name_buffer),
		       stats->mode,
		       stats->in_recovery_mode ? "recovering" : "-",
		       get_dedupe_state_name(layer->dedupe_index),
		       get_vdo_compressing(&layer->vdo) ? "online" : "offline",
		       stats->data_blocks_used + stats->overhead_blocks_used,
		       stats->physical_blocks);
		mutex_unlock(&layer->stats_mutex);
		break;

	case STATUSTYPE_TABLE:
		// Report the string actually specified in the beginning.
		device_config = (struct device_config *) ti->private;
		DMEMIT("%s", device_config->original_string);
		break;
	}

	//  spin_unlock_irqrestore(&layer->lock, flags);
}


/**
 * Get the size of a vdo's underlying device, in blocks.
 *
 * @param vdo  The vdo
 *
 * @return The size in blocks
 **/
static block_count_t __must_check
get_underlying_device_block_count(const struct vdo *vdo)
{
	return (i_size_read(get_vdo_backing_device(vdo)->bd_inode)
		/ VDO_BLOCK_SIZE);
}

/**********************************************************************/
static int
vdo_prepare_to_grow_logical(struct kernel_layer *layer, char *size_string)
{
	block_count_t logical_count;

	if (sscanf(size_string, "%llu", &logical_count) != 1) {
		uds_log_warning("Logical block count \"%s\" is not a number",
				size_string);
		return -EINVAL;
	}

	if (logical_count > MAXIMUM_LOGICAL_BLOCKS) {
		uds_log_warning("Logical block count \"%llu\" exceeds the maximum (%llu)",
				logical_count, MAXIMUM_LOGICAL_BLOCKS);
		return -EINVAL;
	}

	return prepare_to_resize_logical(layer, logical_count);
}

/**********************************************************************/
static int vdo_grow_physical(struct vdo *vdo)
{
	// The actual growPhysical will happen when the device is resumed.
	if (vdo->device_config->version != 0) {
		/*
		 * XXX Uncomment this branch when new VDO manager is updated to
		 * not send this message. Old style message on new style table
		 * is unexpected; it means the user started the VDO with new
		 * manager and is growing with old.
		 */
		// log_info("Mismatch between growPhysical method and table version.");
		// return -EINVAL;
	} else {
		vdo->device_config->physical_blocks =
			get_underlying_device_block_count(vdo);

	}
	return 0;
}

/**
 * Process a dmsetup message now that we know no other message is being
 * processed.
 *
 * @param layer  The layer to which the message was sent
 * @param argc   The argument count of the message
 * @param argv   The arguments to the message
 *
 * @return -EINVAL if the message is unrecognized or the result of processing
 *                 the message
 **/
static int __must_check
process_vdo_message_locked(struct kernel_layer *layer,
			   unsigned int argc,
			   char **argv)
{
	// Messages with fixed numbers of arguments.
	switch (argc) {
	case 1:
		if (strcasecmp(argv[0], "sync-dedupe") == 0) {
			vdo_wait_for_no_requests_active(&layer->vdo);
			return 0;
		}


		if (strcasecmp(argv[0], "prepareToGrowPhysical") == 0) {
			block_count_t backing_blocks =
				get_underlying_device_block_count(&layer->vdo);
			return prepare_to_resize_physical(layer,
							  backing_blocks);
		}

		if (strcasecmp(argv[0], "growPhysical") == 0) {
			return vdo_grow_physical(&layer->vdo);
		}

		break;

	case 2:
		if (strcasecmp(argv[0], "compression") == 0) {
			if (strcasecmp(argv[1], "on") == 0) {
				set_kvdo_compressing(&layer->vdo, true);
				return 0;
			}

			if (strcasecmp(argv[1], "off") == 0) {
				set_kvdo_compressing(&layer->vdo, false);
				return 0;
			}

			uds_log_warning("invalid argument '%s' to dmsetup compression message",
					argv[1]);
			return -EINVAL;
		}

		if (strcasecmp(argv[0], "prepareToGrowLogical") == 0) {
			return vdo_prepare_to_grow_logical(layer, argv[1]);
		}

		break;


	default:
		break;
	}

	uds_log_warning("unrecognized dmsetup message '%s' received", argv[0]);
	return -EINVAL;
}

/**
 * Process a dmsetup message. If the message is a dump, just do it. Otherwise,
 * check that no other message is being processed, and only proceed if so.
 *
 * @param layer  The layer to which the message was sent
 * @param argc   The argument count of the message
 * @param argv   The arguments to the message
 *
 * @return -EBUSY if another message is being processed or the result of
 *                processsing the message
 **/
static int __must_check
process_vdo_message(struct kernel_layer *layer, unsigned int argc, char **argv)
{
	int result;

	/*
	 * All messages which may be processed in parallel with other messages
	 * should be handled here before the atomic check below. Messages which
	 * should be exclusive should be processed in
	 * process_vdo_message_locked().
	 */

	// Dump messages should always be processed
	if (strcasecmp(argv[0], "dump") == 0) {
		return vdo_dump(layer, argc, argv, "dmsetup message");
	}

	if (argc == 1) {
		if (strcasecmp(argv[0], "dump-on-shutdown") == 0) {
			layer->dump_on_shutdown = true;
			return 0;
		}

		// Index messages should always be processed
		if ((strcasecmp(argv[0], "index-close") == 0) ||
		    (strcasecmp(argv[0], "index-create") == 0) ||
		    (strcasecmp(argv[0], "index-disable") == 0) ||
		    (strcasecmp(argv[0], "index-enable") == 0)) {
			return message_dedupe_index(layer->dedupe_index,
						    argv[0]);
		}

		/*
		 * XXX - the "connect" messages are misnamed for the kernel
		 * index. These messages should go away when all callers have
		 * been fixed to use "index-enable" or "index-disable".
		 */
		if (strcasecmp(argv[0], "reconnect") == 0) {
			return message_dedupe_index(layer->dedupe_index,
						    "index-enable");
		}

		if (strcasecmp(argv[0], "connect") == 0) {
			return message_dedupe_index(layer->dedupe_index,
						    "index-enable");
		}

		if (strcasecmp(argv[0], "disconnect") == 0) {
			return message_dedupe_index(layer->dedupe_index,
						    "index-disable");
		}
	}

	if (atomic_cmpxchg(&layer->processing_message, 0, 1) != 0) {
		return -EBUSY;
	}

	result = process_vdo_message_locked(layer, argc, argv);

	smp_wmb();
	atomic_set(&layer->processing_message, 0);
	return result;
}

/**********************************************************************/
static int vdo_message(struct dm_target *ti,
		       unsigned int argc,
		       char **argv,
		       char *result_buffer,
		       unsigned int maxlen)
{
	struct registered_thread allocating_thread, instance_thread;
	struct kernel_layer *layer;
	int result;
	if (argc == 0) {
		uds_log_warning("unspecified dmsetup message");
		return -EINVAL;
	}

	layer = get_kernel_layer_for_target(ti);

	register_allocating_thread(&allocating_thread, NULL);
	register_thread_device_id(&instance_thread, &layer->vdo.instance);

	// Must be done here so we don't map return codes. The code in
	// dm-ioctl expects a 1 for a return code to look at the buffer
	// and see if it is full or not.
	if (argc == 1) {
		if (strcasecmp(argv[0], "dedupe_stats") == 0) {
			write_vdo_stats(layer, result_buffer, maxlen);
			unregister_thread_device_id();
			unregister_allocating_thread();
			return 1;
		}

		if (strcasecmp(argv[0], "kernel_stats") == 0) {
			write_kernel_stats(layer, result_buffer, maxlen);
			unregister_thread_device_id();
			unregister_allocating_thread();
			return 1;
		}
	}

	result = process_vdo_message(layer, argc, argv);

	unregister_thread_device_id();
	unregister_allocating_thread();
	return map_to_system_error(result);
}

/**
 * Configure the dm_target with our capabilities.
 *
 * @param ti     The device mapper target representing our device
 * @param layer  The kernel layer to get the write policy from
 **/
static void configure_target_capabilities(struct dm_target *ti,
					  struct kernel_layer *layer)
{
	ti->discards_supported = 1;
	ti->flush_supported = true;
	ti->num_discard_bios = 1;
	ti->num_flush_bios = 1;

	// If this value changes, please make sure to update the
	// value for max_discard_sectors accordingly.
	BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);
}

/**
 * Implements vdo_filter_t.
 **/
static bool vdo_uses_device(struct vdo *vdo, void *context)
{
	struct device_config *config = context;
	return (get_vdo_backing_device(vdo)->bd_dev
		== config->owned_device->bdev->bd_dev);
}

/**
 * Initializes a single VDO instance and loads the data from disk
 *
 * @param ti        The device mapper target representing our device
 * @param instance  The device instantiation counter
 * @param config    The parsed config for the instance
 *
 * @return VDO_SUCCESS or an error code
 *
 **/
static int vdo_initialize(struct dm_target *ti,
			  unsigned int instance,
			  struct device_config *config)
{
	char *failure_reason;
	struct vdo *vdo;
	struct kernel_layer *layer;
	int result;

	uint64_t block_size = VDO_BLOCK_SIZE;
	uint64_t logical_size = to_bytes(ti->len);
	block_count_t logical_blocks = logical_size / block_size;

	log_info("loading device '%s'", get_vdo_device_name(ti));
	uds_log_debug("Logical block size     = %llu",
		      (uint64_t) config->logical_block_size);
	uds_log_debug("Logical blocks         = %llu", logical_blocks);
	uds_log_debug("Physical block size    = %llu", (uint64_t) block_size);
	uds_log_debug("Physical blocks        = %llu", config->physical_blocks);
	uds_log_debug("Block map cache blocks = %u", config->cache_size);
	uds_log_debug("Block map maximum age  = %u",
		      config->block_map_maximum_age);
	uds_log_debug("Deduplication          = %s",
		      (config->deduplication ? "on" : "off"));

	vdo = find_vdo_matching(vdo_uses_device, config);
	if (vdo != NULL) {
		uds_log_error("Existing vdo already uses device %s",
			      vdo->device_config->parent_device_name);
		release_vdo_instance(instance);
		ti->error = "Cannot share storage device with already-running VDO";
		return VDO_BAD_CONFIGURATION;
	}

	result = make_kernel_layer(instance,
				   config,
				   &failure_reason,
				   &layer);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not create kernel physical layer. (VDO error %d, message %s)",
			      result,
			      failure_reason);
		ti->error = failure_reason;
		return result;
	}

	result = preload_kernel_layer(layer, &failure_reason);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not start kernel physical layer. (VDO error %d, message %s)",
			      result,
			      failure_reason);
		ti->error = failure_reason;
		free_kernel_layer(layer);
		return result;
	}

	set_device_config_vdo(config, &layer->vdo);
	set_vdo_active_config(&layer->vdo, config);
	ti->private = config;
	configure_target_capabilities(ti, layer);
	return VDO_SUCCESS;
}

/**
 * Implements vdo_filter_t.
 **/
static bool __must_check vdo_is_named(struct vdo *vdo, void *context)
{
	struct dm_target *ti = vdo->device_config->owning_target;
	const char *device_name = get_vdo_device_name(ti);
	return (strcmp(device_name, (const char *) context) == 0);
}

/**********************************************************************/
static int vdo_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	int result = VDO_SUCCESS;
	struct registered_thread allocating_thread, instance_thread;
	const char *device_name;
	struct vdo *old_vdo;
	unsigned int instance;
	struct device_config *config = NULL;

	register_allocating_thread(&allocating_thread, NULL);

	device_name = get_vdo_device_name(ti);
	old_vdo = find_vdo_matching(vdo_is_named, (void *) device_name);
	if (old_vdo == NULL) {
		result = allocate_vdo_instance(&instance);
		if (result != VDO_SUCCESS) {
			unregister_allocating_thread();
			return -ENOMEM;
		}
	} else {
		instance = old_vdo->instance;
	}

	register_thread_device_id(&instance_thread, &instance);
	result = parse_device_config(argc, argv, ti, &config);
	if (result != VDO_SUCCESS) {
		unregister_thread_device_id();
		unregister_allocating_thread();
		if (old_vdo == NULL) {
			release_vdo_instance(instance);
		}
		return -EINVAL;
	}

	// Is there already a device of this name?
	if (old_vdo != NULL) {
		struct kernel_layer *layer = vdo_as_kernel_layer(old_vdo);
		/*
		 * To preserve backward compatibility with old VDO Managers, we
		 * need to allow this to happen when either suspended or not.
		 * We could assert that if the config is version 0, we are
		 * suspended, and if not, we are not, but we can't do that
		 * until new VDO Manager does the right order.
		 */
		log_info("preparing to modify device '%s'", device_name);
		result = prepare_to_modify_kernel_layer(layer,
							config,
							&ti->error);
		if (result != VDO_SUCCESS) {
			result = map_to_system_error(result);
			free_device_config(&config);
		} else {
			set_device_config_vdo(config, old_vdo);
			ti->private = config;
			configure_target_capabilities(ti, layer);
		}
		unregister_thread_device_id();
		unregister_allocating_thread();
		return result;
	}

	result = vdo_initialize(ti, instance, config);
	if (result != VDO_SUCCESS) {
		// vdo_initialize calls into various VDO routines, so map error
		result = map_to_system_error(result);
		free_device_config(&config);
	}

	unregister_thread_device_id();
	unregister_allocating_thread();
	return result;
}

/**********************************************************************/
static void vdo_dtr(struct dm_target *ti)
{
	struct device_config *config = ti->private;
	struct vdo *vdo = config->vdo;
	struct kernel_layer *layer = vdo_as_kernel_layer(vdo);

	set_device_config_vdo(config, NULL);
	if (list_empty(&vdo->device_config_list)) {
		const char *device_name;

		// This was the last config referencing the layer. Free it.
		unsigned int instance = layer->vdo.instance;
		struct registered_thread allocating_thread, instance_thread;

		register_thread_device_id(&instance_thread, &instance);
		register_allocating_thread(&allocating_thread, NULL);

		vdo_wait_for_no_requests_active(vdo);
		device_name = get_vdo_device_name(ti);

		log_info("stopping device '%s'", device_name);

		if (layer->dump_on_shutdown) {
			vdo_dump_all(layer, "device shutdown");
		}

		free_kernel_layer(layer);
		log_info("device '%s' stopped", device_name);
		unregister_thread_device_id();
		unregister_allocating_thread();
	} else if (config == vdo->device_config) {
		// The layer still references this config. Give it a reference
		// to a config that isn't being destroyed.
		vdo->device_config =
			as_device_config(vdo->device_config_list.next);
	}

	free_device_config(&config);
	ti->private = NULL;
}

/**********************************************************************/
static void vdo_presuspend(struct dm_target *ti)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct registered_thread instance_thread;

	register_thread_device_id(&instance_thread, &vdo->instance);
	if (dm_noflush_suspending(ti)) {
		vdo->no_flush_suspend = true;
	}
	unregister_thread_device_id();
}

/**********************************************************************/
static void vdo_postsuspend(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct registered_thread instance_thread;
	const char *device_name;
	int result;

	register_thread_device_id(&instance_thread, &layer->vdo.instance);
	device_name = get_vdo_device_name(ti);

	log_info("suspending device '%s'", device_name);
	result = suspend_kernel_layer(layer);

	if (result == VDO_SUCCESS) {
		log_info("device '%s' suspended", device_name);
	} else {
		uds_log_error("suspend of device '%s' failed with error: %d",
			      device_name,
			      result);
	}

	layer->vdo.no_flush_suspend = false;
	unregister_thread_device_id();
}

/**********************************************************************/
static int vdo_preresume(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct device_config *config = ti->private;
	struct registered_thread instance_thread;
	const char *device_name;
	block_count_t backing_blocks;
	int result;

	register_thread_device_id(&instance_thread, &layer->vdo.instance);
	device_name = get_vdo_device_name(ti);

	backing_blocks = get_underlying_device_block_count(&layer->vdo);
	if (backing_blocks < config->physical_blocks) {
		uds_log_error("resume of device '%s' failed: backing device has %llu blocks but VDO physical size is %llu blocks",
			      device_name, backing_blocks,
			      config->physical_blocks);
		unregister_thread_device_id();
		return -EINVAL;
	}

	if (get_kernel_layer_state(layer) == LAYER_STARTING) {
		char *failure_reason;
		int result;

		// This is the first time this device has been resumed, so run
		// it.
		log_info("starting device '%s'", device_name);
		result = start_kernel_layer(layer, &failure_reason);

		if (result != VDO_SUCCESS) {
			uds_log_error("Could not run kernel physical layer. (VDO error %d, message %s)",
				      result,
				      failure_reason);
			set_vdo_read_only(&layer->vdo, result);
			unregister_thread_device_id();
			return map_to_system_error(result);
		}

		log_info("device '%s' started", device_name);
	}

	log_info("resuming device '%s'", device_name);

	// This is a noop if nothing has changed, and by calling it every time
	// we capture old-style growPhysicals, which change the config in place.
	result = modify_kernel_layer(layer, config);

	if (result != VDO_SUCCESS) {
		log_error_strerror(result,
				   "Commit of modifications to device '%s' failed",
				   device_name);
		set_vdo_active_config(&layer->vdo, config);
		set_vdo_read_only(&layer->vdo, result);
	} else {
		set_vdo_active_config(&layer->vdo, config);
		result = resume_kernel_layer(layer);
		if (result != VDO_SUCCESS) {
			uds_log_error("resume of device '%s' failed with error: %d",
				      device_name, result);
		}
	}
	unregister_thread_device_id();
	return map_to_system_error(result);
}

/**********************************************************************/
static void vdo_resume(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct registered_thread instance_thread;

	register_thread_device_id(&instance_thread, &layer->vdo.instance);
	log_info("device '%s' resumed", get_vdo_device_name(ti));
	unregister_thread_device_id();
}

/*
 * If anything changes that affects how user tools will interact
 * with vdo, update the version number and make sure
 * documentation about the change is complete so tools can
 * properly update their management code.
 */
static struct target_type vdo_target_bio = {
	.features = DM_TARGET_SINGLETON,
	.name = "vdo",
	.version = { 6, 2, 3 },
	.module = THIS_MODULE,
	.ctr = vdo_ctr,
	.dtr = vdo_dtr,
	.io_hints = vdo_io_hints,
	.iterate_devices = vdo_iterate_devices,
	.map = vdo_map_bio,
	.message = vdo_message,
	.status = vdo_status,
	.presuspend = vdo_presuspend,
	.postsuspend = vdo_postsuspend,
	.preresume = vdo_preresume,
	.resume = vdo_resume,
};

static bool dm_registered;
static bool sysfs_initialized;

/**********************************************************************/
static void vdo_destroy(void)
{
	uds_log_debug("in %s", __func__);

	vdo_globals.status = VDO_MODULE_SHUTTING_DOWN;

	if (sysfs_initialized) {
		vdo_put_sysfs(&vdo_globals.kobj);
	}

	vdo_globals.status = VDO_MODULE_UNINITIALIZED;

	if (dm_registered) {
		dm_unregister_target(&vdo_target_bio);
	}

	clean_up_instance_number_tracking();

	log_info("unloaded version %s", CURRENT_VERSION);
}

/**********************************************************************/
static int __init vdo_init(void)
{
	int result = 0;

	initialize_thread_device_registry();
	initialize_device_registry_once();
	log_info("loaded version %s", CURRENT_VERSION);

	// Add VDO errors to the already existing set of errors in UDS.
	result = register_status_codes();
	if (result != UDS_SUCCESS) {
		uds_log_error("register_status_codes failed %d", result);
		vdo_destroy();
		return result;
	}

	result = dm_register_target(&vdo_target_bio);
	if (result < 0) {
		uds_log_error("dm_register_target failed %d", result);
		vdo_destroy();
		return result;
	}
	dm_registered = true;

	vdo_globals.status = VDO_MODULE_UNINITIALIZED;

	result = vdo_init_sysfs(&vdo_globals.kobj);
	if (result < 0) {
		uds_log_error("sysfs initialization failed %d", result);
		vdo_destroy();
		// vdo_init_sysfs only returns system error codes
		return result;
	}
	sysfs_initialized = true;

	initialize_instance_number_tracking();

	vdo_globals.status = VDO_MODULE_READY;
	return result;
}

/**********************************************************************/
static void __exit vdo_exit(void)
{
	vdo_destroy();
}

module_init(vdo_init);
module_exit(vdo_exit);

MODULE_DESCRIPTION(DM_NAME " target for transparent deduplication");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
