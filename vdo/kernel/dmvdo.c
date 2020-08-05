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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/dmvdo.c#68 $
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

struct kvdo_module_globals kvdoGlobals;

/*
 * Pre kernel version 4.3, we use the functionality in blkdev_issue_discard
 * and the value in max_discard_sectors to split large discards into smaller
 * ones. 4.3 to 4.18 kernels have removed the code in blkdev_issue_discard
 * and so in place of that, we use the code in device mapper itself to
 * split the discards. Unfortunately, it uses the same value to split large
 * discards as it does to split large data bios.
 *
 * In kernel version 4.18, support for splitting discards was added
 * back into blkdev_issue_discard. Since this mode of splitting
 * (based on max_discard_sectors) is preferable to splitting always
 * on 4k, we are turning off the device mapper splitting from 4.18
 * on.
 */
#define HAS_NO_BLKDEV_SPLIT                              \
	LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0) && \
		LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)

/**********************************************************************/

/**
 * Get the kernel layer associated with a dm target structure.
 *
 * @param ti  The dm target structure
 *
 * @return The kernel layer, or NULL.
 **/
static struct kernel_layer *get_kernel_layer_for_target(struct dm_target *ti)
{
	return ((struct device_config *) ti->private)->layer;
}

/**
 * Begin VDO processing of a bio.  This is called by the device mapper
 * through the "map" function, and has resulted from a call to either
 * submit_bio or generic_make_request.
 *
 * @param ti      The dm_target.  We only need the "private" member to give
 *                us the kernel_layer.
 * @param bio     The bio.
 *
 * @return One of these values:
 *
 *         negative            A negative value is an error code.
 *                             Usually -EIO.
 *
 *         DM_MAPIO_SUBMITTED  VDO will take care of this I/O, either
 *                             processing it completely and calling
 *                             bio_endio, or forwarding it onward by
 *                             calling generic_make_request.
 *
 *         DM_MAPIO_REMAPPED   VDO has modified the bio and the device
 *                             mapper will immediately forward the bio
 *                             onward using generic_make_request.
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

	limits->logical_block_size = layer->device_config->logical_block_size;
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
	 * We allow setting of the value for max_discard_sectors even in
	 * situations where we only split on 4k (see comments for
	 * HAS_NO_BLKDEV_SPLIT) as the value is still used in other code, like
	 * sysfs display of queue limits and most especially in dm-thin to
	 * determine whether to pass down discards.
	 */
	limits->max_discard_sectors =
		layer->device_config->max_discard_blocks *
		VDO_SECTORS_PER_BLOCK;

	limits->discard_granularity = VDO_BLOCK_SIZE;
}

/**********************************************************************/
static int vdo_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn,
			       void *data)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	sector_t len =
		block_to_sector(layer, layer->device_config->physical_blocks);

	return fn(ti, layer->device_config->owned_device, 0, len, data);
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
	char name_buffer[BDEVNAME_SIZE];
	// N.B.: The DMEMIT macro uses the variables named "sz", "result",
	// "maxlen".
	int sz = 0;

	switch (status_type) {
	case STATUSTYPE_INFO:
		// Report info for dmsetup status
		mutex_lock(&layer->statsMutex);
		get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
		struct vdo_statistics *stats = &layer->vdo_stats_storage;

		DMEMIT("/dev/%s %s %s %s %s %llu %llu",
		       bdevname(get_kernel_layer_bdev(layer), name_buffer),
		       stats->mode,
		       stats->in_recovery_mode ? "recovering" : "-",
		       get_dedupe_state_name(layer->dedupe_index),
		       get_kvdo_compressing(&layer->kvdo) ? "online" :
							    "offline",
		       stats->data_blocks_used + stats->overhead_blocks_used,
		       stats->physical_blocks);
		mutex_unlock(&layer->statsMutex);
		break;

	case STATUSTYPE_TABLE:
		// Report the string actually specified in the beginning.
		DMEMIT("%s",
		       ((struct device_config *) ti->private)->original_string);
		break;
	}

	//  spin_unlock_irqrestore(&layer->lock, flags);
}


/**
 * Get the size of the underlying device, in blocks.
 *
 * @param [in] layer  The layer
 *
 * @return The size in blocks
 **/
static block_count_t
get_underlying_device_block_count(struct kernel_layer *layer)
{
	uint64_t physical_size =
		i_size_read(get_kernel_layer_bdev(layer)->bd_inode);
	return physical_size / VDO_BLOCK_SIZE;
}

/**********************************************************************/
static int vdo_prepare_to_grow_logical(struct kernel_layer *layer, char *size_string)
{
	block_count_t logical_count;

	if (sscanf(size_string, "%llu", &logical_count) != 1) {
		log_warning("Logical block count \"%s\" is not a number",
			    size_string);
		return -EINVAL;
	}

	if (logical_count > MAXIMUM_LOGICAL_BLOCKS) {
		log_warning("Logical block count \"%llu\" exceeds the maximum (%llu)",
			    logical_count, MAXIMUM_LOGICAL_BLOCKS);
		return -EINVAL;
	}

	return prepare_to_resize_logical(layer, logical_count);
}

/**
 * Process a dmsetup message now that we know no other message is being
 * processed.
 *
 * @param layer The layer to which the message was sent
 * @param argc  The argument count of the message
 * @param argv  The arguments to the message
 *
 * @return -EINVAL if the message is unrecognized or the result of processing
 *                 the message
 **/
static int __must_check
process_vdo_message_locked(struct kernel_layer *layer,
			   unsigned int argc,
			   char **argv)
{
	// Messages with variable numbers of arguments.
	if (strncasecmp(argv[0], "x-", 2) == 0) {
		int result =
			perform_kvdo_extended_command(&layer->kvdo, argc, argv);
		if (result == VDO_UNKNOWN_COMMAND) {
			log_warning("unknown extended command '%s' to dmsetup message",
				    argv[0]);
			result = -EINVAL;
		}

		return result;
	}

	// Messages with fixed numbers of arguments.
	switch (argc) {
	case 1:
		if (strcasecmp(argv[0], "sync-dedupe") == 0) {
			wait_for_no_requests_active(layer);
			return 0;
		}

		if (strcasecmp(argv[0], "trace-on") == 0) {
			log_info("Tracing on");
			layer->trace_logging = true;
			return 0;
		}

		if (strcasecmp(argv[0], "trace-off") == 0) {
			log_info("Tracing off");
			layer->trace_logging = false;
			return 0;
		}

		if (strcasecmp(argv[0], "prepareToGrowPhysical") == 0) {
			return prepare_to_resize_physical(layer,
							  get_underlying_device_block_count(layer));
		}

		if (strcasecmp(argv[0], "growPhysical") == 0) {
			/*
			 * The actual growPhysical will happen when the device
			 * is resumed.
			 */

			if (layer->device_config->version != 0) {
				/*
				 * XXX Uncomment this branch when new VDO
				 * manager is updated to not send this
				 * message. Old style message on new style
				 * table is unexpected; it means the user
				 * started the VDO with new manager and is
				 * growing with old.
				 */

				// log_info("Mismatch between growPhysical method
				// and table version."); return -EINVAL;
			} else {
				layer->device_config->physical_blocks =
					get_underlying_device_block_count(layer);
			}
			return 0;
		}

		break;

	case 2:
		if (strcasecmp(argv[0], "compression") == 0) {
			if (strcasecmp(argv[1], "on") == 0) {
				set_kvdo_compressing(&layer->kvdo, true);
				return 0;
			}

			if (strcasecmp(argv[1], "off") == 0) {
				set_kvdo_compressing(&layer->kvdo, false);
				return 0;
			}

			log_warning("invalid argument '%s' to dmsetup compression message",
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

	log_warning("unrecognized dmsetup message '%s' received", argv[0]);
	return -EINVAL;
}

/**
 * Process a dmsetup message. If the message is a dump, just do it. Otherwise,
 * check that no other message is being processed, and only proceed if so.
 *
 * @param layer         The layer to which the message was sent
 * @param argc          The argument count of the message
 * @param argv          The arguments to the message
 *
 * @return -EBUSY if another message is being processed or the result of
 *                processsing the message
 **/
static int __must_check
process_vdo_message(struct kernel_layer *layer, unsigned int argc, char **argv)
{
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

	if (!compareAndSwapBool(&layer->processing_message, false, true)) {
		return -EBUSY;
	}

	int result = process_vdo_message_locked(layer, argc, argv);

	atomicStoreBool(&layer->processing_message, false);
	return result;
}

/**********************************************************************/
static int vdo_message(struct dm_target *ti,
		       unsigned int argc,
		       char **argv,
		       char *result_buffer,
		       unsigned int maxlen)
{
	if (argc == 0) {
		log_warning("unspecified dmsetup message");
		return -EINVAL;
	}

	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct registered_thread allocating_thread, instance_thread;

	register_allocating_thread(&allocating_thread, NULL);
	register_thread_device(&instance_thread, layer);

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

	int result = process_vdo_message(layer, argc, argv);

	unregister_thread_device_id();
	unregister_allocating_thread();
	return map_to_system_error(result);
}

/**
 * Configure the dm_target with our capabilities.
 *
 * @param ti    The device mapper target representing our device
 * @param layer The kernel layer to get the write policy from
 **/
static void configure_target_capabilities(struct dm_target *ti,
					  struct kernel_layer *layer)
{
	ti->discards_supported = 1;

	/**
	 * This may appear to indicate we don't support flushes in sync mode.
	 * However, dm will set up the request queue to accept flushes if any
	 * device in the stack accepts flushes. Hence if the device under VDO
	 * accepts flushes, we will receive flushes.
	 **/
	ti->flush_supported = should_process_flush(layer);
	ti->num_discard_bios = 1;
	ti->num_flush_bios = 1;

	// If this value changes, please make sure to update the
	// value for maxDiscardSectors accordingly.
	BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);

/*
 * Please see comments above where the macro is defined.
 */
#if HAS_NO_BLKDEV_SPLIT
	ti->split_discard_bios = 1;
#endif
}

/**
 * Handle a vdo_initialize failure, freeing all appropriate structures.
 *
 * @param ti             The device mapper target representing our device
 * @param thread_config  The thread config (possibly NULL)
 * @param layer          The kernel layer (possibly NULL)
 * @param instance       The instance number to be released
 * @param why            The reason for failure
 **/
static void cleanup_initialize(struct dm_target *ti,
			       struct thread_config *thread_config,
			       struct kernel_layer *layer,
			       unsigned int instance,
			       char *why)
{
	if (thread_config != NULL) {
		free_thread_config(&thread_config);
	}
	if (layer != NULL) {
		// This releases the instance number too.
		free_kernel_layer(layer);
	} else {
		// With no kernel_layer taking ownership we have to release
		// explicitly.
		release_kvdo_instance(instance);
	}

	ti->error = why;
}

/**
 * Initializes a single VDO instance and loads the data from disk
 *
 * @param ti        The device mapper target representing our device
 * @param instance  The device instantiation counter
 * @param config    The parsed config for the instance
 *
 * @return  VDO_SUCCESS or an error code
 *
 **/
static int vdo_initialize(struct dm_target *ti,
			  unsigned int instance,
			  struct device_config *config)
{
	log_info("loading device '%s'", config->pool_name);

	uint64_t block_size = VDO_BLOCK_SIZE;
	uint64_t logical_size = to_bytes(ti->len);
	block_count_t logical_blocks = logical_size / block_size;

	log_debug("Logical block size     = %llu",
		  (uint64_t) config->logical_block_size);
	log_debug("Logical blocks         = %llu", logical_blocks);
	log_debug("Physical block size    = %llu", (uint64_t) block_size);
	log_debug("Physical blocks        = %llu", config->physical_blocks);
	log_debug("Block map cache blocks = %u", config->cache_size);
	log_debug("Block map maximum age  = %u",
		  config->block_map_maximum_age);
	log_debug("MD RAID5 mode          = %s",
		  (config->md_raid5_mode_enabled ? "on" : "off"));
	log_debug("Write policy           = %s",
		  get_config_write_policy_string(config));
	log_debug("Deduplication          = %s",
		  (config->deduplication ? "on" : "off"));

	// The thread_config will be copied by the VDO if it's successfully
	// created.
	struct vdo_load_config load_config = {
		.cache_size = config->cache_size,
		.thread_config = NULL,
		.write_policy = config->write_policy,
		.maximum_age = config->block_map_maximum_age,
	};

	char *failure_reason;
	struct kernel_layer *layer;
	int result = make_kernel_layer(ti->begin,
				       instance,
				       config,
				       &kvdoGlobals.kobj,
				       &load_config.thread_config,
				       &failure_reason,
				       &layer);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not create kernel physical layer. (VDO error %d, message %s)",
			      result,
			      failure_reason);
		cleanup_initialize(ti,
				   load_config.thread_config,
				   NULL,
				   instance,
				   failure_reason);
		return result;
	}

	// Now that we have read the geometry, we can finish setting up the
	// vdo_load_config.
	set_load_config_from_geometry(&layer->geometry, &load_config);

	if (config->cache_size < (2 * MAXIMUM_USER_VIOS *
				  load_config.thread_config->logical_zone_count)) {
		log_warning("Insufficient block map cache for logical zones");
		cleanup_initialize(ti,
				   load_config.thread_config,
				   layer,
				   instance,
				   "Insufficient block map cache for logical zones");
		return VDO_BAD_CONFIGURATION;
	}

	// Henceforth it is the kernel layer's responsibility to clean up the
	// struct thread_config.
	result = preload_kernel_layer(layer, &load_config, &failure_reason);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not start kernel physical layer. (VDO error %d, message %s)",
			      result, failure_reason);
		cleanup_initialize(ti, NULL, layer, instance, failure_reason);
		return result;
	}

	set_device_config_layer(config, layer);
	set_kernel_layer_active_config(layer, config);
	ti->private = config;
	configure_target_capabilities(ti, layer);
	return VDO_SUCCESS;
}

/**********************************************************************/
static int vdo_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	// Mild hack to avoid bumping instance number when we needn't.
	char *pool_name;
	int result = get_pool_name_from_argv(argc,
					     argv,
					     &ti->error,
					     &pool_name);
	if (result != VDO_SUCCESS) {
		return -EINVAL;
	}

	struct registered_thread allocating_thread;

	register_allocating_thread(&allocating_thread, NULL);

	struct kernel_layer *old_layer = find_layer_matching(layer_is_named,
							     pool_name);
	unsigned int instance;

	if (old_layer == NULL) {
		result = allocate_kvdo_instance(&instance);
		if (result != VDO_SUCCESS) {
			unregister_allocating_thread();
			return -ENOMEM;
		}
	} else {
		instance = old_layer->instance;
	}

	struct registered_thread instance_thread;

	register_thread_device_id(&instance_thread, &instance);

	bool verbose = (old_layer == NULL);
	struct device_config *config = NULL;

	result = parse_device_config(argc, argv, ti, verbose, &config);
	if (result != VDO_SUCCESS) {
		unregister_thread_device_id();
		unregister_allocating_thread();
		if (old_layer == NULL) {
			release_kvdo_instance(instance);
		}
		return -EINVAL;
	}

	// Is there already a device of this name?
	if (old_layer != NULL) {
		/*
		 * To preserve backward compatibility with old VDO Managers, we
		 * need to allow this to happen when either suspended or not. We
		 * could assert that if the config is version 0, we are
		 * suspended, and if not, we are not, but we can't do that till
		 * new VDO Manager does the right order.
		 */
		log_info("preparing to modify device '%s'", config->pool_name);
		result = prepare_to_modify_kernel_layer(old_layer,
							config,
							&ti->error);
		if (result != VDO_SUCCESS) {
			result = map_to_system_error(result);
			free_device_config(&config);
		} else {
			set_device_config_layer(config, old_layer);
			ti->private = config;
			configure_target_capabilities(ti, old_layer);
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
	struct kernel_layer *layer = config->layer;

	set_device_config_layer(config, NULL);

	if (list_empty(&layer->device_config_list)) {
		// This was the last config referencing the layer. Free it.
		unsigned int instance = layer->instance;
		struct registered_thread allocating_thread, instance_thread;

		register_thread_device_id(&instance_thread, &instance);
		register_allocating_thread(&allocating_thread, NULL);

		wait_for_no_requests_active(layer);
		log_info("stopping device '%s'", config->pool_name);

		if (layer->dump_on_shutdown) {
			vdo_dump_all(layer, "device shutdown");
		}

		free_kernel_layer(layer);
		log_info("device '%s' stopped", config->pool_name);
		unregister_thread_device_id();
		unregister_allocating_thread();
	} else if (config == layer->device_config) {
		// The layer still references this config. Give it a reference
		// to a config that isn't being destroyed.
		layer->device_config =
			as_device_config(layer->device_config_list.next);
	}

	free_device_config(&config);
	ti->private = NULL;
}

/**********************************************************************/
static void vdo_presuspend(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct registered_thread instance_thread;

	register_thread_device(&instance_thread, layer);
	if (dm_noflush_suspending(ti)) {
		layer->no_flush_suspend = true;
	}
	unregister_thread_device_id();
}

/**********************************************************************/
static void vdo_postsuspend(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct registered_thread instance_thread;

	register_thread_device(&instance_thread, layer);
	const char *pool_name = layer->device_config->pool_name;

	log_info("suspending device '%s'", pool_name);
	int result = suspend_kernel_layer(layer);

	if (result == VDO_SUCCESS) {
		log_info("device '%s' suspended", pool_name);
	} else {
		uds_log_error("suspend of device '%s' failed with error: %d",
			      pool_name,
			      result);
	}
	layer->no_flush_suspend = false;
	unregister_thread_device_id();
}

/**********************************************************************/
static int vdo_preresume(struct dm_target *ti)
{
	struct kernel_layer *layer = get_kernel_layer_for_target(ti);
	struct device_config *config = ti->private;
	struct registered_thread instance_thread;

	register_thread_device(&instance_thread, layer);

	if (get_kernel_layer_state(layer) == LAYER_STARTING) {
		// This is the first time this device has been resumed, so run
		// it.
		log_info("starting device '%s'", config->pool_name);
		char *failure_reason;
		int result = start_kernel_layer(layer, &failure_reason);

		if (result != VDO_SUCCESS) {
			uds_log_error("Could not run kernel physical layer. (VDO error %d, message %s)",
				      result,
				      failure_reason);
			set_kvdo_read_only(&layer->kvdo, result);
			unregister_thread_device_id();
			return map_to_system_error(result);
		}

		log_info("device '%s' started", config->pool_name);
	}

	log_info("resuming device '%s'", config->pool_name);

	// This is a noop if nothing has changed, and by calling it every time
	// we capture old-style growPhysicals, which change the config in place.
	int result = modify_kernel_layer(layer, config);

	if (result != VDO_SUCCESS) {
		log_error_strerror(result,
				   "Commit of modifications to device '%s' failed",
				   config->pool_name);
		set_kernel_layer_active_config(layer, config);
		set_kvdo_read_only(&layer->kvdo, result);
	} else {
		set_kernel_layer_active_config(layer, config);
		result = resume_kernel_layer(layer);
		if (result != VDO_SUCCESS) {
			uds_log_error("resume of device '%s' failed with error: %d",
				      layer->device_config->pool_name, result);
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

	register_thread_device(&instance_thread, layer);
	log_info("device '%s' resumed", layer->device_config->pool_name);
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
	.version = { 6, 2, 2 },
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
	log_debug("in %s", __func__);

	kvdoGlobals.status = SHUTTING_DOWN;

	if (sysfs_initialized) {
		vdo_put_sysfs(&kvdoGlobals.kobj);
	}

	kvdoGlobals.status = UNINITIALIZED;

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

	kvdoGlobals.status = UNINITIALIZED;

	result = vdo_init_sysfs(&kvdoGlobals.kobj);
	if (result < 0) {
		uds_log_error("sysfs initialization failed %d", result);
		vdo_destroy();
		// vdo_init_sysfs only returns system error codes
		return result;
	}
	sysfs_initialized = true;

	init_work_queue_once();
	initialize_trace_logging_once();
	initialize_instance_number_tracking();

	kvdoGlobals.status = READY;
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
