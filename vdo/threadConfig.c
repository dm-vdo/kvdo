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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/threadConfig.c#24 $
 */

#include "threadConfig.h"


#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

#include "deviceConfig.h"
#include "kernelTypes.h"
#include "statusCodes.h"
#include "types.h"

/**********************************************************************/
static int allocate_thread_config(zone_count_t logical_zone_count,
				  zone_count_t physical_zone_count,
				  zone_count_t hash_zone_count,
				  zone_count_t base_thread_count,
				  struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result =
		UDS_ALLOCATE(1, struct thread_config, "thread config", &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(logical_zone_count,
			      thread_id_t,
			      "logical thread array",
			      &config->logical_threads);
	if (result != VDO_SUCCESS) {
		free_vdo_thread_config(config);
		return result;
	}

	result = UDS_ALLOCATE(physical_zone_count,
			      thread_id_t,
			      "physical thread array",
			      &config->physical_threads);
	if (result != VDO_SUCCESS) {
		free_vdo_thread_config(config);
		return result;
	}

	result = UDS_ALLOCATE(hash_zone_count,
			      thread_id_t,
			      "hash thread array",
			      &config->hash_zone_threads);
	if (result != VDO_SUCCESS) {
		free_vdo_thread_config(config);
		return result;
	}

	config->logical_zone_count = logical_zone_count;
	config->physical_zone_count = physical_zone_count;
	config->hash_zone_count = hash_zone_count;
	config->base_thread_count = base_thread_count;

	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
static void
assign_thread_ids(thread_id_t thread_ids[],
		  zone_count_t count,
		  thread_id_t *id_ptr)
{
	zone_count_t zone;

	for (zone = 0; zone < count; zone++) {
		thread_ids[zone] = (*id_ptr)++;
	}
}

/**********************************************************************/
int make_vdo_thread_config(struct thread_count_config counts,
			   struct thread_config **config_ptr)
{
	struct thread_config *config;
	thread_count_t total;
	int result;
	thread_id_t id = 0;

	total = (counts.logical_zones
		 + counts.physical_zones
		 + counts.hash_zones);
	if (total == 0) {
		result = allocate_thread_config(1, 1, 1, 1, &config);
		if (result != VDO_SUCCESS) {
			return result;
		}

		config->logical_threads[0] = 0;
		config->physical_threads[0] = 0;
		config->hash_zone_threads[0] = 0;
	} else {
		// Add in the packer and admin/recovery journal threads (1
		// each).
		total += 2;
		result = allocate_thread_config(counts.logical_zones,
						counts.physical_zones,
						counts.hash_zones,
						total,
						&config);
		if (result != VDO_SUCCESS) {
			return result;
		}

		config->admin_thread = id;
		config->journal_thread = id++;
		config->packer_thread = id++;
		assign_thread_ids(config->logical_threads,
				  counts.logical_zones,
				  &id);
		assign_thread_ids(config->physical_threads,
				  counts.physical_zones,
				  &id);
		assign_thread_ids(config->hash_zone_threads,
				  counts.hash_zones,
				  &id);
	}

	ASSERT_LOG_ONLY(id == total, "correct number of thread IDs assigned");

	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_vdo_thread_config(struct thread_config *config)
{
	if (config == NULL) {
		return;
	}

	UDS_FREE(UDS_FORGET(config->logical_threads));
	UDS_FREE(UDS_FORGET(config->physical_threads));
	UDS_FREE(UDS_FORGET(config->hash_zone_threads));
	UDS_FREE(config);
}

/**********************************************************************/
static bool get_zone_thread_name(const thread_id_t thread_ids[],
				 zone_count_t count,
				 thread_id_t id,
				 const char *prefix,
				 char *buffer,
				 size_t buffer_length)
{
	if (id >= thread_ids[0]) {
		thread_id_t index = id - thread_ids[0];

		if (index < count) {
			snprintf(buffer, buffer_length, "%s%d", prefix, index);
			return true;
		}
	}
	return false;
}

/**********************************************************************/
void vdo_get_thread_name(const struct thread_config *thread_config,
			 thread_id_t thread_id,
			 char *buffer,
			 size_t buffer_length)
{
	if (thread_config->base_thread_count == 1) {
		// Historically this was the "request queue" thread.
		snprintf(buffer, buffer_length, "reqQ");
		return;
	}
	if (thread_id == thread_config->journal_thread) {
		snprintf(buffer, buffer_length, "journalQ");
		return;
	} else if (thread_id == thread_config->admin_thread) {
		// Theoretically this could be different from the journal
		// thread.
		snprintf(buffer, buffer_length, "adminQ");
		return;
	} else if (thread_id == thread_config->packer_thread) {
		snprintf(buffer, buffer_length, "packerQ");
		return;
	}
	if (get_zone_thread_name(thread_config->logical_threads,
				 thread_config->logical_zone_count,
				 thread_id,
				 "logQ",
				 buffer,
				 buffer_length)) {
		return;
	}
	if (get_zone_thread_name(thread_config->physical_threads,
				 thread_config->physical_zone_count,
				 thread_id,
				 "physQ",
				 buffer,
				 buffer_length)) {
		return;
	}
	if (get_zone_thread_name(thread_config->hash_zone_threads,
				 thread_config->hash_zone_count,
				 thread_id,
				 "hashQ",
				 buffer,
				 buffer_length)) {
		return;
	}

	// Some sort of misconfiguration?
	snprintf(buffer, buffer_length, "reqQ%d", thread_id);
}
