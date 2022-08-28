/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DEDUPE_H
#define DEDUPE_H

#include <linux/kobject.h>
#include <linux/list.h>

#include "uds.h"

#include "completion.h"
#include "kernel-types.h"
#include "statistics.h"
#include "types.h"
#include "wait-queue.h"

struct hash_lock;
struct hash_zone;
struct hash_zones;

struct pbn_lock * __must_check
vdo_get_duplicate_lock(struct data_vio *data_vio);

int __must_check vdo_acquire_hash_lock(struct data_vio *data_vio);

void vdo_enter_hash_lock(struct data_vio *data_vio);

void vdo_continue_hash_lock(struct data_vio *data_vio);

void vdo_continue_hash_lock_on_error(struct data_vio *data_vio);

void vdo_release_hash_lock(struct data_vio *data_vio);

void vdo_share_compressed_write_lock(struct data_vio *data_vio,
				     struct pbn_lock *pbn_lock);

int __must_check
vdo_make_hash_zones(struct vdo *vdo, struct hash_zones **zones_ptr);

void vdo_free_hash_zones(struct hash_zones *zones);

thread_id_t __must_check
vdo_get_hash_zone_thread_id(const struct hash_zone *zone);

void vdo_drain_hash_zones(struct hash_zones *zones,
			  struct vdo_completion *parent);

void vdo_get_dedupe_statistics(struct hash_zones *zones,
			       struct vdo_statistics *stats);

struct hash_zone * __must_check
vdo_select_hash_zone(struct hash_zones *zones,
		     const struct uds_chunk_name *name);

void vdo_dump_hash_zones(struct hash_zones *zones);

const char *vdo_get_dedupe_index_state_name(struct hash_zones *zones);

uint64_t vdo_get_dedupe_index_timeout_count(struct hash_zones *zones);

int vdo_message_dedupe_index(struct hash_zones *zones, const char *name);

int vdo_add_dedupe_index_sysfs(struct hash_zones *zones);

void vdo_start_dedupe_index(struct hash_zones *zones, bool create_flag);

void vdo_resume_hash_zones(struct hash_zones *zones,
			   struct vdo_completion *parent);

void vdo_finish_dedupe_index(struct hash_zones *zones);

/*
 * Interval (in milliseconds) from submission until switching to fast path and
 * skipping UDS.
 */
extern unsigned int vdo_dedupe_index_timeout_interval;

/*
 * Minimum time interval (in milliseconds) between timer invocations to
 * check for requests waiting for UDS that should now time out.
 */
extern unsigned int vdo_dedupe_index_min_timer_interval;

void vdo_set_dedupe_index_timeout_interval(unsigned int value);
void vdo_set_dedupe_index_min_timer_interval(unsigned int value);

#endif /* DEDUPE_H */
