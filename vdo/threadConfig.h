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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/threadConfig.h#1 $
 */

#ifndef THREAD_CONFIG_H
#define THREAD_CONFIG_H

#include "permassert.h"

#include "types.h"

struct thread_config {
	zone_count_t logical_zone_count;
	zone_count_t physical_zone_count;
	zone_count_t hash_zone_count;
	thread_count_t base_thread_count;
	thread_id_t admin_thread;
	thread_id_t journal_thread;
	thread_id_t packer_thread;
	thread_id_t *logical_threads;
	thread_id_t *physical_threads;
	thread_id_t *hash_zone_threads;
};

/**
 * Make a thread configuration. If both the logical zone count and the
 * physical zone count are set to 0, a one thread configuration will be
 * made.
 *
 * @param [in]  logical_zone_count    The number of logical zones
 * @param [in]  physical_zone_count   The number of physical zones
 * @param [in]  hash_zone_count       The number of hash zones
 * @param [out] config_ptr            A pointer to hold the new thread
 *                                    configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_thread_config(zone_count_t logical_zone_count,
				    zone_count_t physical_zone_count,
				    zone_count_t hash_zone_count,
				    struct thread_config **config_ptr);

/**
 * Make a thread configuration that uses only one thread.
 *
 * @param [out] config_ptr      A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check make_one_thread_config(struct thread_config **config_ptr);

/**
 * Make a new thread config which is a copy of an existing one.
 *
 * @param [in]  old_config       The thread configuration to copy
 * @param [out] config_ptr       A pointer to hold the new thread configuration
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check copy_thread_config(const struct thread_config *old_config,
				    struct thread_config **config_ptr);

/**
 * Destroy a thread configuration and null out the reference to it.
 *
 * @param config_ptr  The reference to the thread configuration to destroy
 **/
void free_thread_config(struct thread_config **config_ptr);

/**
 * Get the thread id for a given logical zone.
 *
 * @param thread_config  the thread config
 * @param logical_zone   the number of the logical zone
 *
 * @return the thread id for the given zone
 **/
static inline thread_id_t __must_check
get_logical_zone_thread(const struct thread_config *thread_config,
			zone_count_t logical_zone)
{
	ASSERT_LOG_ONLY((logical_zone <= thread_config->logical_zone_count),
			"logical zone valid");
	return thread_config->logical_threads[logical_zone];
}

/**
 * Get the thread id for a given physical zone.
 *
 * @param thread_config  the thread config
 * @param physical_zone  the number of the physical zone
 *
 * @return the thread id for the given zone
 **/
static inline thread_id_t __must_check
get_physical_zone_thread(const struct thread_config *thread_config,
			 zone_count_t physical_zone)
{
	ASSERT_LOG_ONLY((physical_zone <= thread_config->physical_zone_count),
			"physical zone valid");
	return thread_config->physical_threads[physical_zone];
}

/**
 * Get the thread id for a given hash zone.
 *
 * @param thread_config  the thread config
 * @param hash_zone      the number of the hash zone
 *
 * @return the thread id for the given zone
 **/
static inline thread_id_t __must_check
get_hash_zone_thread(const struct thread_config *thread_config,
		     zone_count_t hash_zone)
{
	ASSERT_LOG_ONLY((hash_zone <= thread_config->hash_zone_count),
			"hash zone valid");
	return thread_config->hash_zone_threads[hash_zone];
}

/**
 * Get the thread id for the journal zone.
 *
 * @param thread_config  the thread config
 *
 * @return the thread id for the journal zone
 **/
static inline
thread_id_t __must_check
get_journal_zone_thread(const struct thread_config *thread_config)
{
	return thread_config->journal_thread;
}

/**
 * Get the thread id for the packer zone.
 *
 * @param thread_config  the thread config
 *
 * @return the thread id for the packer zone
 **/
static inline
thread_id_t __must_check
get_packer_zone_thread(const struct thread_config *thread_config)
{
	return thread_config->packer_thread;
}

/**
 * Get the thread ID for admin requests.
 *
 * @param thread_config  The thread config
 *
 * @return the thread id to use for admin requests
 **/
static inline
thread_id_t __must_check
get_admin_thread(const struct thread_config *thread_config)
{
	return thread_config->admin_thread;
}

/**
 * Format the name of the worker thread desired to support a given
 * work queue. The physical layer may add a prefix identifying the
 * product; the output from this function should just identify the
 * thread.
 *
 * @param thread_config  The thread configuration
 * @param thread_id      The thread id
 * @param buffer         Where to put the formatted name
 * @param buffer_length  Size of the output buffer
 **/
void get_vdo_thread_name(const struct thread_config *thread_config,
			 thread_id_t thread_id,
			 char *buffer,
			 size_t buffer_length);

#endif /* THREAD_CONFIG_H */
