/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef THREAD_CONFIG_H
#define THREAD_CONFIG_H

#include "permassert.h"

#include "kernel-types.h"
#include "types.h"

struct thread_config {
	zone_count_t logical_zone_count;
	zone_count_t physical_zone_count;
	zone_count_t hash_zone_count;
	thread_count_t bio_thread_count;
	thread_count_t thread_count;
	thread_id_t admin_thread;
	thread_id_t journal_thread;
	thread_id_t packer_thread;
	thread_id_t dedupe_thread;
	thread_id_t bio_ack_thread;
	thread_id_t cpu_thread;
	thread_id_t *logical_threads;
	thread_id_t *physical_threads;
	thread_id_t *hash_zone_threads;
	thread_id_t *bio_threads;
};

int __must_check
vdo_make_thread_config(struct thread_count_config counts,
		       struct thread_config **config_ptr);

void vdo_free_thread_config(struct thread_config *config);

/**
 * Get the thread id for a given logical zone.
 *
 * @param thread_config  the thread config
 * @param logical_zone   the number of the logical zone
 *
 * @return the thread id for the given zone
 **/
static inline thread_id_t __must_check
vdo_get_logical_zone_thread(const struct thread_config *thread_config,
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
vdo_get_physical_zone_thread(const struct thread_config *thread_config,
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
vdo_get_hash_zone_thread(const struct thread_config *thread_config,
			 zone_count_t hash_zone)
{
	ASSERT_LOG_ONLY((hash_zone <= thread_config->hash_zone_count),
			"hash zone valid");
	return thread_config->hash_zone_threads[hash_zone];
}

void vdo_get_thread_name(const struct thread_config *thread_config,
			 thread_id_t thread_id,
			 char *buffer,
			 size_t buffer_length);

#endif /* THREAD_CONFIG_H */
