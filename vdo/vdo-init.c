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

#include "vdo-init.h"

#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/lz4.h>
#include <linux/mutex.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "admin-completion.h"
#include "device-registry.h"
#include "instance-number.h"
#include "limiter.h"
#include "pool-sysfs.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "volume-geometry.h"

/**
 * Get the device name associated with the vdo target
 *
 * @param target  The target device interface
 *
 * @return The block device name
 **/
const char *vdo_get_device_name(const struct dm_target *target)
{
	return dm_device_name(dm_table_get_md(target->table));
}

/**
 * Allocate a vdos threads, queues, and other structures which scale with the
 * thread config.
 *
 * @param vdo     The vdo being initialized
 * @param reason  A pointer to hold an error message on failure
 *
 * @return VDO_SUCCESS or an error
 **/
static int allocate_vdo_threads(struct vdo *vdo, char **reason)
{
	int i;
	struct device_config *config = vdo->device_config;

	int result = vdo_make_thread_config(config->thread_counts,
					    &vdo->thread_config);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot create thread configuration";
		return result;
	}

	uds_log_info("zones: %d logical, %d physical, %d hash; base threads: %d",
		     config->thread_counts.logical_zones,
		     config->thread_counts.physical_zones,
		     config->thread_counts.hash_zones,
		     vdo->thread_config->base_thread_count);

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

	return VDO_SUCCESS;
}

/**
 * Perform the first steps in initializing a vdo as part of device creation.
 *
 * @param vdo       The vdo being initialized
  * @param config    The configuration of the vdo being initialized
 * @param instance  The device instantiation counter
 * @param reason    A pointer to hold an error message on failure
 *
 * @return VDO_SUCCESS or an error code
 **/
int vdo_initialize_internal(struct vdo *vdo,
			    struct device_config *config,
			    unsigned int instance,
			    char **reason)
{
	int result;

	vdo->device_config = config;
	vdo->starting_sector_offset = config->owning_target->begin;
	vdo->instance = instance;
	vdo->allocations_allowed = true;
	vdo_set_admin_state_code(&vdo->admin_state, VDO_ADMIN_STATE_NEW);
	INIT_LIST_HEAD(&vdo->device_config_list);
	vdo_initialize_admin_completion(vdo, &vdo->admin_completion);
	mutex_init(&vdo->stats_mutex);
	initialize_limiter(&vdo->request_limiter, MAXIMUM_VDO_USER_VIOS);
	initialize_limiter(&vdo->discard_limiter,
			   MAXIMUM_VDO_USER_VIOS * 3 / 4);
	result = vdo_read_geometry_block(vdo_get_backing_device(vdo),
					 &vdo->geometry);
	if (result != VDO_SUCCESS) {
		*reason = "Could not load geometry block";
		vdo_destroy(vdo);
		return result;
	}

	result = allocate_vdo_threads(vdo, reason);
	if (result != VDO_SUCCESS) {
		vdo_destroy(vdo);
		return result;
	}

	result = vdo_register(vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot add VDO to device registry";
		vdo_destroy(vdo);
		return result;
	}

	vdo_set_admin_state_code(&vdo->admin_state,
				 VDO_ADMIN_STATE_INITIALIZED);
	return result;
}
