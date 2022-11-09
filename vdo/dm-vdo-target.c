// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/module.h>

#include "bio.h"
#include "constants.h"
#include "data-vio-pool.h"
#include "dedupe.h"
#include "device-registry.h"
#include "dump.h"
#include "flush.h"
#include "instance-number.h"
#include "io-submitter.h"
#include "logger.h"
#include "memory-alloc.h"
#include "messageStats.h"
#include "string-utils.h"
#include "thread-config.h"
#include "thread-device.h"
#include "thread-registry.h"
#include "uds-sysfs.h"
#include "vdo.h"
#include "vdo-load.h"
#include "vdo-resume.h"
#include "vdo-suspend.h"

static struct vdo *get_vdo_for_target(struct dm_target *ti)
{
	return ((struct device_config *) ti->private)->vdo;
}

static int vdo_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct vdo_work_queue *current_work_queue;
	const struct admin_state_code *code
		= vdo_get_admin_state_code(&vdo->admin_state);

	ASSERT_LOG_ONLY(code->normal,
			"vdo should not receive bios while in state %s",
			code->name);

	/* Count all incoming bios. */
	vdo_count_bios(&vdo->stats.bios_in, bio);


	/* Handle empty bios.  Empty flush bios are not associated with a vio. */
	if ((bio_op(bio) == REQ_OP_FLUSH) ||
	    ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		vdo_launch_flush(vdo, bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* This could deadlock, */
	current_work_queue = get_current_work_queue();
	BUG_ON((current_work_queue != NULL)
	       && (vdo == get_work_queue_owner(current_work_queue)->vdo));
	vdo_launch_bio(vdo->data_vio_pool, bio);
	return DM_MAPIO_SUBMITTED;
}

static void vdo_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct vdo *vdo = get_vdo_for_target(ti);

	limits->logical_block_size = vdo->device_config->logical_block_size;
	limits->physical_block_size = VDO_BLOCK_SIZE;

	/* The minimum io size for random io */
	blk_limits_io_min(limits, VDO_BLOCK_SIZE);
	/* The optimal io size for streamed/sequential io */
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
	limits->max_discard_sectors = (vdo->device_config->max_discard_blocks
				       * VDO_SECTORS_PER_BLOCK);

	/*
	 * Force discards to not begin or end with a partial block by stating
	 * the granularity is 4k.
	 */
	limits->discard_granularity = VDO_BLOCK_SIZE;
}

static int vdo_iterate_devices(struct dm_target *ti,
			       iterate_devices_callout_fn fn,
			       void *data)
{
	struct device_config *config = get_vdo_for_target(ti)->device_config;

	return fn(ti,
		  config->owned_device,
		  0,
		  config->physical_blocks * VDO_SECTORS_PER_BLOCK,
		  data);
}

/*
 * Status line is:
 *    <device> <operating mode> <in recovery> <index state>
 *    <compression state> <used physical blocks> <total physical blocks>
 */

static void vdo_status(struct dm_target *ti,
		       status_type_t status_type,
		       unsigned int status_flags,
		       char *result,
		       unsigned int maxlen)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct vdo_statistics *stats;
	struct device_config *device_config;
	/*
	 * N.B.: The DMEMIT macro uses the variables named "sz", "result",
	 * "maxlen".
	 */
	int sz = 0;

	switch (status_type) {
	case STATUSTYPE_INFO:
		/* Report info for dmsetup status */
		mutex_lock(&vdo->stats_mutex);
		vdo_fetch_statistics(vdo, &vdo->stats_buffer);
		stats = &vdo->stats_buffer;

		DMEMIT("/dev/%pg %s %s %s %s %llu %llu",
		       vdo_get_backing_device(vdo),
		       stats->mode,
		       stats->in_recovery_mode ? "recovering" : "-",
		       vdo_get_dedupe_index_state_name(vdo->hash_zones),
		       vdo_get_compressing(vdo) ? "online" : "offline",
		       stats->data_blocks_used + stats->overhead_blocks_used,
		       stats->physical_blocks);
		mutex_unlock(&vdo->stats_mutex);
		break;

	case STATUSTYPE_TABLE:
		/* Report the string actually specified in the beginning. */
		device_config = (struct device_config *) ti->private;
		DMEMIT("%s", device_config->original_string);
		break;
	/*
	 * FIXME: We ought to print more detailed output here, but this is what
	 * thin does.
	 */
	case STATUSTYPE_IMA:
		*result = '\0';
		break;
	}
}

static block_count_t __must_check
get_underlying_device_block_count(const struct vdo *vdo)
{
	return (i_size_read(vdo_get_backing_device(vdo)->bd_inode)
		/ VDO_BLOCK_SIZE);
}

static int __must_check
process_vdo_message_locked(struct vdo *vdo,
			   unsigned int argc,
			   char **argv)
{
	if (argc == 2) {
		if (strcasecmp(argv[0], "compression") == 0) {
			if (strcasecmp(argv[1], "on") == 0) {
				vdo_set_compressing(vdo, true);
				return 0;
			}

			if (strcasecmp(argv[1], "off") == 0) {
				vdo_set_compressing(vdo, false);
				return 0;
			}

			uds_log_warning("invalid argument '%s' to dmsetup compression message",
					argv[1]);
			return -EINVAL;
		}
	}

	uds_log_warning("unrecognized dmsetup message '%s' received", argv[0]);
	return -EINVAL;
}

/*
 * If the message is a dump, just do it. Otherwise, check that no other message
 * is being processed, and only proceed if so.
 * Returns -EBUSY if another message is being processed
 */
static int __must_check
process_vdo_message(struct vdo *vdo, unsigned int argc, char **argv)
{
	int result;

	/*
	 * All messages which may be processed in parallel with other messages
	 * should be handled here before the atomic check below. Messages which
	 * should be exclusive should be processed in
	 * process_vdo_message_locked().
	 */

	/* Dump messages should always be processed */
	if (strcasecmp(argv[0], "dump") == 0) {
		return vdo_dump(vdo, argc, argv, "dmsetup message");
	}

	if (argc == 1) {
		if (strcasecmp(argv[0], "dump-on-shutdown") == 0) {
			vdo->dump_on_shutdown = true;
			return 0;
		}

		/* Index messages should always be processed */
		if ((strcasecmp(argv[0], "index-close") == 0) ||
		    (strcasecmp(argv[0], "index-create") == 0) ||
		    (strcasecmp(argv[0], "index-disable") == 0) ||
		    (strcasecmp(argv[0], "index-enable") == 0)) {
			return vdo_message_dedupe_index(vdo->hash_zones,
							argv[0]);
		}
	}

	if (atomic_cmpxchg(&vdo->processing_message, 0, 1) != 0) {
		return -EBUSY;
	}

	result = process_vdo_message_locked(vdo, argc, argv);

	smp_wmb();
	atomic_set(&vdo->processing_message, 0);
	return result;
}

static int vdo_message(struct dm_target *ti,
		       unsigned int argc,
		       char **argv,
		       char *result_buffer,
		       unsigned int maxlen)
{
	struct registered_thread allocating_thread, instance_thread;
	struct vdo *vdo;
	int result;

	if (argc == 0) {
		uds_log_warning("unspecified dmsetup message");
		return -EINVAL;
	}

	vdo = get_vdo_for_target(ti);
	uds_register_allocating_thread(&allocating_thread, NULL);
	uds_register_thread_device_id(&instance_thread, &vdo->instance);

	/*
	 * Must be done here so we don't map return codes. The code in
	 * dm-ioctl expects a 1 for a return code to look at the buffer
	 * and see if it is full or not.
	 */
	if ((argc == 1) && (strcasecmp(argv[0], "stats") == 0)) {
		vdo_write_stats(vdo, result_buffer, maxlen);
		result = 1;
	} else {
		result = vdo_map_to_system_error(process_vdo_message(vdo,
								     argc,
								     argv));
	}

	uds_unregister_thread_device_id();
	uds_unregister_allocating_thread();
	return result;
}

static void configure_target_capabilities(struct dm_target *ti)
{
	ti->discards_supported = 1;
	ti->flush_supported = true;
	ti->num_discard_bios = 1;
	ti->num_flush_bios = 1;

	/*
	 * If this value changes, please make sure to update the
	 * value for max_discard_sectors accordingly.
	 */
	BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);
}

/*
 * Implements vdo_filter_t.
 */
static bool vdo_uses_device(struct vdo *vdo, void *context)
{
	struct device_config *config = context;

	return (vdo_get_backing_device(vdo)->bd_dev
		== config->owned_device->bdev->bd_dev);
}

static int vdo_initialize(struct dm_target *ti,
			  unsigned int instance,
			  struct device_config *config)
{
	struct vdo *vdo;
	int result;

	uint64_t block_size = VDO_BLOCK_SIZE;
	uint64_t logical_size = to_bytes(ti->len);
	block_count_t logical_blocks = logical_size / block_size;

	uds_log_info("loading device '%s'", vdo_get_device_name(ti));
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
	uds_log_debug("Compression            = %s",
		      (config->compression ? "on" : "off"));


	vdo = vdo_find_matching(vdo_uses_device, config);
	if (vdo != NULL) {
		uds_log_error("Existing vdo already uses device %s",
			      vdo->device_config->parent_device_name);
		vdo_release_instance(instance);
		ti->error = "Cannot share storage device with already-running VDO";
		return VDO_BAD_CONFIGURATION;
	}

	result = vdo_make(instance, config, &ti->error, &vdo);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not create VDO device. (VDO error %d, message %s)",
			      result,
			      ti->error);
		vdo_destroy(vdo);
		return result;
	}

	result = vdo_prepare_to_load(vdo);
	if (result != VDO_SUCCESS) {
		ti->error = ((result == VDO_INVALID_ADMIN_STATE)
			     ? "Pre-load is only valid immediately after initialization"
			     : "Cannot load metadata from device");
		uds_log_error("Could not start VDO device. (VDO error %d, message %s)",
			      result,
			      ti->error);
		vdo_destroy(vdo);
		return result;
	}

	vdo_set_device_config(config, vdo);
	vdo->device_config = config;
	ti->private = config;
	configure_target_capabilities(ti);
	return VDO_SUCCESS;
}

/*
 * Implements vdo_filter_t.
 */
static bool __must_check vdo_is_named(struct vdo *vdo, void *context)
{
	struct dm_target *ti = vdo->device_config->owning_target;
	const char *device_name = vdo_get_device_name(ti);

	return (strcmp(device_name, (const char *) context) == 0);
}

static int vdo_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	int result = VDO_SUCCESS;
	struct registered_thread allocating_thread, instance_thread;
	const char *device_name;
	struct vdo *old_vdo;
	unsigned int instance;
	struct device_config *config = NULL;

	uds_register_allocating_thread(&allocating_thread, NULL);

	device_name = vdo_get_device_name(ti);
	old_vdo = vdo_find_matching(vdo_is_named, (void *) device_name);
	if (old_vdo == NULL) {
		result = vdo_allocate_instance(&instance);
		if (result != VDO_SUCCESS) {
			uds_unregister_allocating_thread();
			return -ENOMEM;
		}
	} else {
		instance = old_vdo->instance;
	}

	uds_register_thread_device_id(&instance_thread, &instance);
	result = vdo_parse_device_config(argc, argv, ti, &config);
	if (result != VDO_SUCCESS) {
		uds_unregister_thread_device_id();
		uds_unregister_allocating_thread();
		if (old_vdo == NULL) {
			vdo_release_instance(instance);
		}
		return -EINVAL;
	}

	/* Is there already a device of this name? */
	if (old_vdo != NULL) {
		bool may_grow = (vdo_get_admin_state(old_vdo)
				 != VDO_ADMIN_STATE_PRE_LOADED);

		uds_log_info("preparing to modify device '%s'", device_name);
		result = vdo_prepare_to_modify(old_vdo,
					       config,
					       may_grow,
					       &ti->error);
		if (result != VDO_SUCCESS) {
			result = vdo_map_to_system_error(result);
			vdo_free_device_config(UDS_FORGET(config));
		} else {
			vdo_set_device_config(config, old_vdo);
			ti->private = config;
			configure_target_capabilities(ti);
		}
		uds_unregister_thread_device_id();
		uds_unregister_allocating_thread();
		return result;
	}

	result = vdo_initialize(ti, instance, config);
	if (result != VDO_SUCCESS) {
		/* vdo_initialize calls into various VDO routines, so map error */
		result = vdo_map_to_system_error(result);
		vdo_free_device_config(config);
	}

	uds_unregister_thread_device_id();
	uds_unregister_allocating_thread();
	return result;
}

static void vdo_dtr(struct dm_target *ti)
{
	struct device_config *config = ti->private;
	struct vdo *vdo = config->vdo;

	vdo_set_device_config(config, NULL);
	if (list_empty(&vdo->device_config_list)) {
		const char *device_name;

		/* This was the last config referencing the VDO. Free it. */
		unsigned int instance = vdo->instance;
		struct registered_thread allocating_thread, instance_thread;

		uds_register_thread_device_id(&instance_thread, &instance);
		uds_register_allocating_thread(&allocating_thread, NULL);

		device_name = vdo_get_device_name(ti);
		uds_log_info("stopping device '%s'", device_name);
		if (vdo->dump_on_shutdown) {
			vdo_dump_all(vdo, "device shutdown");
		}

		vdo_destroy(UDS_FORGET(vdo));
		uds_log_info("device '%s' stopped", device_name);
		uds_unregister_thread_device_id();
		uds_unregister_allocating_thread();
	} else if (config == vdo->device_config) {
		/*
		 * The VDO still references this config. Give it a reference
		 * to a config that isn't being destroyed.
		 */
		vdo->device_config =
			vdo_as_device_config(vdo->device_config_list.next);
	}

	vdo_free_device_config(config);
	ti->private = NULL;
}

static void vdo_presuspend(struct dm_target *ti)
{
	get_vdo_for_target(ti)->suspend_type
		= (dm_noflush_suspending(ti)
		   ? VDO_ADMIN_STATE_SUSPENDING
		   : VDO_ADMIN_STATE_SAVING);
}

static void vdo_postsuspend(struct dm_target *ti)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct registered_thread instance_thread;

	uds_register_thread_device_id(&instance_thread, &vdo->instance);
	vdo_suspend(vdo);
	uds_unregister_thread_device_id();
}

static int vdo_preresume(struct dm_target *ti)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct device_config *config = ti->private;
	struct registered_thread instance_thread;
	const char *device_name;
	block_count_t backing_blocks;
	int result;

	uds_register_thread_device_id(&instance_thread, &vdo->instance);
	device_name = vdo_get_device_name(ti);

	backing_blocks = get_underlying_device_block_count(vdo);
	if (backing_blocks < config->physical_blocks) {
		/* FIXME: can this still happen? */
		uds_log_error("resume of device '%s' failed: backing device has %llu blocks but VDO physical size is %llu blocks",
			      device_name,
			      (unsigned long long) backing_blocks,
			      (unsigned long long) config->physical_blocks);
		uds_unregister_thread_device_id();
		return -EINVAL;
	}

	if (vdo_get_admin_state(vdo) == VDO_ADMIN_STATE_PRE_LOADED) {
		result = vdo_load(vdo);
		if (result != VDO_SUCCESS) {
			uds_unregister_thread_device_id();
			return vdo_map_to_system_error(result);
		}

	}

	uds_log_info("resuming device '%s'", device_name);
	result = vdo_preresume_internal(vdo, config, device_name);
	if ((result == VDO_PARAMETER_MISMATCH)
	    || (result == VDO_INVALID_ADMIN_STATE)) {
		result = -EINVAL;
	}

	uds_unregister_thread_device_id();
	return vdo_map_to_system_error(result);
}

static void vdo_resume(struct dm_target *ti)
{
	struct registered_thread instance_thread;

	uds_register_thread_device_id(&instance_thread,
				      &get_vdo_for_target(ti)->instance);
	uds_log_info("device '%s' resumed", vdo_get_device_name(ti));
	uds_unregister_thread_device_id();
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
	.version = { 8, 2, 0 },
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

static void vdo_module_destroy(void)
{
	uds_log_debug("in %s", __func__);

	if (dm_registered) {
		dm_unregister_target(&vdo_target_bio);
	}

	vdo_clean_up_instance_number_tracking();

	uds_log_info("unloaded version %s", CURRENT_VERSION);
}

static int __init vdo_init(void)
{
	int result = 0;

	/*
	 * UDS module level initialization must be done first, as VDO
	 * initialization depends on it
	 */
	uds_initialize_thread_device_registry();
	uds_memory_init();
	uds_init_sysfs();

	vdo_initialize_device_registry_once();
	uds_log_info("loaded version %s", CURRENT_VERSION);

	/* Add VDO errors to the already existing set of errors in UDS. */
	result = vdo_register_status_codes();
	if (result != UDS_SUCCESS) {
		uds_log_error("vdo_register_status_codes failed %d", result);
		vdo_module_destroy();
		return result;
	}

	result = dm_register_target(&vdo_target_bio);
	if (result < 0) {
		uds_log_error("dm_register_target failed %d", result);
		vdo_module_destroy();
		return result;
	}
	dm_registered = true;

	vdo_initialize_instance_number_tracking();

	return result;
}

static void __exit vdo_exit(void)
{
	vdo_module_destroy();
	/*
	 * UDS module level exit processing must be done after all VDO
	 * module exit processing is complete.
	 */
	uds_put_sysfs();
	uds_memory_exit();
}

module_init(vdo_init);
module_exit(vdo_exit);

MODULE_DESCRIPTION(DM_NAME " target for transparent deduplication");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
