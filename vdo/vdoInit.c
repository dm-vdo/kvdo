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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoInit.c#28 $
 */

#include "vdoInit.h"

#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/lz4.h>
#include <linux/mutex.h>

#include "logger.h"
#include "memoryAlloc.h"

#include "adminCompletion.h"
#include "deviceRegistry.h"
#include "instanceNumber.h"
#include "limiter.h"
#include "poolSysfs.h"
#include "types.h"
#include "vdoInternal.h"
#include "volumeGeometry.h"

/**********************************************************************/
const char *get_vdo_device_name(const struct dm_target *target)
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
	int result
		= make_vdo_thread_config(config->thread_counts.logical_zones,
					 config->thread_counts.physical_zones,
					 config->thread_counts.hash_zones,
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

	// Compression context storage
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

/**********************************************************************/
int initialize_vdo(struct vdo *vdo,
		   struct device_config *config,
		   unsigned int instance,
		   char **reason)
{
	int result;

	vdo->device_config = config;
	vdo->starting_sector_offset = config->owning_target->begin;
	vdo->instance = instance;
	vdo->allocations_allowed = true;
	set_vdo_admin_state_code(&vdo->admin_state, VDO_ADMIN_STATE_NEW);
	INIT_LIST_HEAD(&vdo->device_config_list);
	initialize_vdo_admin_completion(vdo, &vdo->admin_completion);
	mutex_init(&vdo->stats_mutex);
	initialize_limiter(&vdo->request_limiter, MAXIMUM_VDO_USER_VIOS);
	initialize_limiter(&vdo->discard_limiter,
			   MAXIMUM_VDO_USER_VIOS * 3 / 4);

	initialize_vdo_deadlock_queue(&vdo->deadlock_queue);
	result = vdo_read_geometry_block(get_vdo_backing_device(vdo),
					 &vdo->geometry);
	if (result != VDO_SUCCESS) {
		*reason = "Could not load geometry block";
		destroy_vdo(vdo);
		return result;
	}

	result = allocate_vdo_threads(vdo, reason);
	if (result != VDO_SUCCESS) {
		destroy_vdo(vdo);
		return result;
	}

	result = register_vdo(vdo);
	if (result != VDO_SUCCESS) {
		*reason = "Cannot add VDO to device registry";
		destroy_vdo(vdo);
		return result;
	}

	set_vdo_admin_state_code(&vdo->admin_state,
				 VDO_ADMIN_STATE_INITIALIZED);
	return result;
}
