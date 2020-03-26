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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/threadConfig.c#5 $
 */

#include "threadConfig.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "constants.h"
#include "types.h"

/**********************************************************************/
static int allocate_thread_config(ZoneCount logical_zone_count,
				  ZoneCount physical_zone_count,
				  ZoneCount hash_zone_count,
				  ZoneCount base_thread_count,
				  struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result =
		ALLOCATE(1, struct thread_config, "thread config", &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ALLOCATE(logical_zone_count,
			  ThreadID,
			  "logical thread array",
			  &config->logical_threads);
	if (result != VDO_SUCCESS) {
		free_thread_config(&config);
		return result;
	}

	result = ALLOCATE(physical_zone_count,
			  ThreadID,
			  "physical thread array",
			  &config->physical_threads);
	if (result != VDO_SUCCESS) {
		free_thread_config(&config);
		return result;
	}

	result = ALLOCATE(hash_zone_count,
			  ThreadID,
			  "hash thread array",
			  &config->hash_zone_threads);
	if (result != VDO_SUCCESS) {
		free_thread_config(&config);
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
assign_thread_ids(ThreadID thread_ids[], ZoneCount count, ThreadID *id_ptr)
{
	ZoneCount zone;
	for (zone = 0; zone < count; zone++) {
		thread_ids[zone] = (*id_ptr)++;
	}
}

/**********************************************************************/
int make_thread_config(ZoneCount logical_zone_count,
		       ZoneCount physical_zone_count,
		       ZoneCount hash_zone_count,
		       struct thread_config **config_ptr)
{
	if ((logical_zone_count == 0) && (physical_zone_count == 0) &&
	    (hash_zone_count == 0)) {
		return make_one_thread_config(config_ptr);
	}

	if (physical_zone_count > MAX_PHYSICAL_ZONES) {
		return logErrorWithStringError(
			VDO_BAD_CONFIGURATION,
			"Physical zone count %u exceeds maximum "
			"(%u)",
			physical_zone_count,
			MAX_PHYSICAL_ZONES);
	}

	if (logical_zone_count > MAX_LOGICAL_ZONES) {
		return logErrorWithStringError(
			VDO_BAD_CONFIGURATION,
			"Logical zone count %u exceeds maximum "
			"(%u)",
			logical_zone_count,
			MAX_LOGICAL_ZONES);
	}

	struct thread_config *config;
	ThreadCount total =
		logical_zone_count + physical_zone_count + hash_zone_count + 2;
	int result = allocate_thread_config(logical_zone_count,
					    physical_zone_count,
					    hash_zone_count,
					    total,
					    &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	ThreadID id = 0;
	config->admin_thread = id;
	config->journal_thread = id++;
	config->packer_thread = id++;
	assign_thread_ids(config->logical_threads, logical_zone_count, &id);
	assign_thread_ids(config->physical_threads, physical_zone_count, &id);
	assign_thread_ids(config->hash_zone_threads, hash_zone_count, &id);

	ASSERT_LOG_ONLY(id == total, "correct number of thread IDs assigned");

	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_zero_thread_config(struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result = ALLOCATE(1, struct thread_config, __func__, &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	config->logical_zone_count = 0;
	config->physical_zone_count = 0;
	config->hash_zone_count = 0;
	config->base_thread_count = 0;
	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
int make_one_thread_config(struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result = allocate_thread_config(1, 1, 1, 1, &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	config->logical_threads[0] = 0;
	config->physical_threads[0] = 0;
	config->hash_zone_threads[0] = 0;
	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
int copy_thread_config(const struct thread_config *old_config,
		       struct thread_config **config_ptr)
{
	struct thread_config *config;
	int result = allocate_thread_config(old_config->logical_zone_count,
					    old_config->physical_zone_count,
					    old_config->hash_zone_count,
					    old_config->base_thread_count,
					    &config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	config->admin_thread = old_config->admin_thread;
	config->journal_thread = old_config->journal_thread;
	config->packer_thread = old_config->packer_thread;
	ZoneCount i;
	for (i = 0; i < config->logical_zone_count; i++) {
		config->logical_threads[i] = old_config->logical_threads[i];
	}
	for (i = 0; i < config->physical_zone_count; i++) {
		config->physical_threads[i] = old_config->physical_threads[i];
	}
	for (i = 0; i < config->hash_zone_count; i++) {
		config->hash_zone_threads[i] = old_config->hash_zone_threads[i];
	}

	*config_ptr = config;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_thread_config(struct thread_config **config_ptr)
{
	if (*config_ptr == NULL) {
		return;
	}

	struct thread_config *config = *config_ptr;
	*config_ptr = NULL;

	FREE(config->logical_threads);
	FREE(config->physical_threads);
	FREE(config->hash_zone_threads);
	FREE(config);
}

/**********************************************************************/
static bool get_zone_thread_name(const ThreadID thread_ids[],
				 ZoneCount count,
				 ThreadID id,
				 const char *prefix,
				 char *buffer,
				 size_t buffer_length)
{
	if (id >= thread_ids[0]) {
		ThreadID index = id - thread_ids[0];
		if (index < count) {
			snprintf(buffer, buffer_length, "%s%d", prefix, index);
			return true;
		}
	}
	return false;
}

/**********************************************************************/
void get_vdo_thread_name(const struct thread_config *thread_config,
			 ThreadID thread_id,
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
